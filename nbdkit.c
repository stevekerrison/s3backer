
/*
 * s3backer - FUSE-based single file backing store via Amazon S3
 *
 * Copyright (C) 2022 Nikolaus Rath <Nikolaus@rath.org>
 * Copyright (C) 2022 Archie L. Cobbs <archie.cobbs@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations including
 * the two.
 *
 * You must obey the GNU General Public License in all respects for all
 * of the code used other than OpenSSL. If you modify file(s) with this
 * exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do
 * so, delete this exception statement from your version. If you delete
 * this exception statement from all source files in the program, then
 * also delete it here.
 */

#include "s3backer.h"
#include "block_cache.h"
#include "ec_protect.h"
#include "zero_cache.h"
#include "fuse_ops.h"
#include "http_io.h"
#include "test_io.h"
#include "s3b_config.h"
#include "util.h"

#define NBDKIT_API_VERSION              2
#include <nbdkit-plugin.h>

// Parameter name
#define CONFIG_FILE_PARAMETER_NAME      "configFile"

// Concurrent requests are supported
#define THREAD_MODEL                    NBDKIT_THREAD_MODEL_PARALLEL

// s3backer state
static char *configFile = NULL;
static struct s3b_config *config;
static const struct fuse_operations *fuse_ops;
static struct s3backer_store *s3b;
static struct fuse_ops_private *fuse_priv;

// Internal functions
static void s3b_nbd_logger(int level, const char *fmt, ...);
static int handle_unknown_option(void *data, const char *arg, int key, struct fuse_args *outargs);

// NBDKit plugin functions
static int s3b_nbd_plugin_config(const char *key, const char *value);
static int s3b_nbd_plugin_config_complete(void);
static int s3b_nbd_plugin_get_ready(void);
static int s3b_nbd_plugin_after_fork(void);
static void *s3b_nbd_plugin_open(int readonly);
static int64_t s3b_nbd_plugin_get_size(void *handle);
static int s3b_nbd_plugin_pread(void *handle, void *bufp, uint32_t size, uint64_t offset, uint32_t flags);
static int s3b_nbd_plugin_pwrite(void *handle, const void *bufp, uint32_t size, uint64_t offset, uint32_t flags);
static int s3b_nbd_plugin_trim(void *handle, uint32_t size, uint64_t offset, uint32_t flags);
static int s3b_nbd_plugin_can_multi_conn(void *handle);
static int s3b_nbd_plugin_can_cache(void *handle);
static void s3b_nbd_plugin_cleanup(void);
static void s3b_nbd_plugin_unload(void);

#define PLUGIN_HELP             \
    "    " CONFIG_FILE_PARAMETER_NAME "=<path>   s3backer config file with command line flags and bucket[/prefix]"

// NBDKit plugin declaration
static struct nbdkit_plugin plugin = {

    // Meta-data
    .name=                  PACKAGE,
    .version=               PACKAGE_VERSION,
    .longname=              PACKAGE,
    .description=           "Block-based backing store via Amazon S3",
    .magic_config_key=      CONFIG_FILE_PARAMETER_NAME,
    .config_help=           PLUGIN_HELP,
    .errno_is_preserved=    0,
    .can_multi_conn=        s3b_nbd_plugin_can_multi_conn,
    .can_write=             NULL,
    .can_flush=             NULL,
    .can_trim=              NULL,
    .can_zero=              NULL,
    .can_fast_zero=         NULL,
    .can_extents=           NULL,
    .can_fua=               NULL,
    .can_cache=             s3b_nbd_plugin_can_cache,
    .is_rotational=         NULL,

    // Startup lifecycle callbacks
    .load=                  NULL,
    .dump_plugin=           NULL,
    .config=                s3b_nbd_plugin_config,
    .config_complete=       s3b_nbd_plugin_config_complete,
    .thread_model=          NULL,
    .get_ready=             s3b_nbd_plugin_get_ready,
    .after_fork=            s3b_nbd_plugin_after_fork,

    // Client connection callbacks
    .preconnect=            NULL,
    .list_exports=          NULL,
    .open=                  s3b_nbd_plugin_open,
    .get_size=              s3b_nbd_plugin_get_size,
    .pread=                 s3b_nbd_plugin_pread,
    .pwrite=                s3b_nbd_plugin_pwrite,
    .trim=                  s3b_nbd_plugin_trim,
    .cache=                 NULL,
    .extents=               NULL,
    .zero=                  s3b_nbd_plugin_trim,    // for us, "trim" and "zero" are the same thing
    .close=                 NULL,

    // Shutdown lifecycle callbacks
    .cleanup=               s3b_nbd_plugin_cleanup,
    .unload=                s3b_nbd_plugin_unload,
};
NBDKIT_REGISTER_PLUGIN(plugin)

// Called for each key=value passed on the nbdkit command line
static int
s3b_nbd_plugin_config(const char *key, const char *value)
{
    // Handle "configFile=xxx"
    if (strcmp(key, CONFIG_FILE_PARAMETER_NAME) == 0) {
        if ((configFile = nbdkit_realpath(value)) == NULL)
            return -1;
        return 0;
    }

    // Unknown parameter
    nbdkit_error("unknown parameter \"%s\"", key);
    return -1;
}

static void
s3b_nbd_logger(int level, const char *fmt, ...)
{
    va_list args;
    char *fmt2;

    // Filter debug if needed
    if ((config == NULL || !config->debug) && level == LOG_DEBUG)
        return;

    // Prefix format string
    if ((fmt2 = prefix_log_format(level, fmt)) == NULL)
        return;

    // Print log message
    va_start(args, fmt);
    nbdkit_vdebug(fmt2, args);
    va_end(args);
    free(fmt2);
}

static int
s3b_nbd_plugin_config_complete(void)
{
    char *argv[3];
    int argc = 0;

    // Sanity check
    if (configFile == NULL) {
        nbdkit_error("missing required \"%s\" parameter", CONFIG_FILE_PARAMETER_NAME);
        return -1;
    }

    // Create fake s3backer command line; strdup() is needed to eliminate "const"
    memset(argv, 0, sizeof(*argv));
    if ((argv[argc++] = strdup("s3backer")) == NULL) {
        nbdkit_error("strdup: %m");
        return -1;
    }
    if (asprintf(&argv[argc++], "--configFile=%s", configFile) == -1) {
        nbdkit_error("strdup: %m");
        return -1;
    }
    assert(argc < sizeof(argv));

    // Parse fake s3backer command line
    if ((config = s3backer_get_config2(argc, argv, 1, handle_unknown_option)) == NULL)
        return -1;

    // Disallow "--erase" and "--reset" flags
    if (config->erase || config->reset) {
        nbdkit_error("NBDKit version of s3backer does not support \"--erase\" or \"--reset\" flags");
        return -1;
    }

    // Configure NBD logging
    config->log = s3b_nbd_logger;

    // Done
    return 0;
}

static int
handle_unknown_option(void *data, const char *arg, int key, struct fuse_args *outargs)
{
    struct s3b_config *const new_config = data;

    // Check options
    if (key == FUSE_OPT_KEY_OPT) {

        // Notice debug flag
        if (strcmp(arg, "-d") == 0)
            new_config->debug = 1;

        // Unknown
        return 1;
    }

    // Get bucket parameter
    if (new_config->bucket == NULL) {
        if ((new_config->bucket = strdup(arg)) == NULL)
            err(1, "strdup");
        return 0;
    }

    // Unknown
    return 1;
}

static int
s3b_nbd_plugin_get_ready(void)
{
    if ((s3b = s3backer_create_store(config)) == NULL) {
        nbdkit_error("error creating s3backer_store: %m");
        return -1;
    }
    if ((fuse_ops = fuse_ops_create(&config->fuse_ops, s3b)) == NULL) {
        (*s3b->shutdown)(s3b);
        (*s3b->destroy)(s3b);
        return -1;
    }
    return 0;
}

static int
s3b_nbd_plugin_after_fork(void)
{
    fuse_priv = (*fuse_ops->init)(NULL);
    return 0;
}

static void
s3b_nbd_plugin_cleanup(void)
{
    if (fuse_priv != NULL)
        (*fuse_ops->destroy)(fuse_priv);
}

static void
s3b_nbd_plugin_unload(void)
{
    free(configFile);
}

static void *
s3b_nbd_plugin_open(int readonly)
{
    (void)readonly;
    return NBDKIT_HANDLE_NOT_NEEDED;
}

/* Size of the data we are going to serve. */
static int64_t
s3b_nbd_plugin_get_size(void *handle)
{
    return fuse_priv->file_size;
}

static int
s3b_nbd_plugin_pread(void *handle, void *buf, uint32_t size, uint64_t offset, uint32_t flags)
{
    struct boundary_info info;
    int r;

    // Calculate what bits to read, then read them
    calculate_boundary_info(&info, config->block_size, buf, size, offset);
    if (info.beg_length > 0
      && (r = (*fuse_priv->s3b->read_block_part)(fuse_priv->s3b,
       info.beg_block, info.beg_offset, info.beg_length, info.beg_data)) != 0) {
        nbdkit_error("error reading block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.beg_block);
        nbdkit_set_error(r);
        return -1;
    }
    while (info.mid_block_count-- > 0) {
        if ((r = (*fuse_priv->s3b->read_block)(fuse_priv->s3b, info.mid_block_start, info.mid_data, NULL, NULL, 0)) != 0) {
            nbdkit_error("error reading block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.mid_block_start);
            nbdkit_set_error(r);
            return -1;
        }
        info.mid_block_start++;
        info.mid_data += config->block_size;
    }
    if (info.end_length > 0
      && (r = (*fuse_priv->s3b->read_block_part)(fuse_priv->s3b, info.end_block, 0, info.end_length, info.end_data)) != 0) {
        nbdkit_error("error reading block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.end_block);
        nbdkit_set_error(r);
        return -1;
    }

    // Done
    return 0;
}

static int
s3b_nbd_plugin_pwrite(void *handle, const void *buf, uint32_t size, uint64_t offset, uint32_t flags)
{
    struct boundary_info info;
    int r;

    // Calculate what bits to write, then write them
    calculate_boundary_info(&info, config->block_size, buf, size, offset);
    if (info.beg_length > 0
      && (r = (*fuse_priv->s3b->write_block_part)(fuse_priv->s3b,
       info.beg_block, info.beg_offset, info.beg_length, info.beg_data)) != 0) {
        nbdkit_error("error writing block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.beg_block);
        nbdkit_set_error(r);
        return -1;
    }
    while (info.mid_block_count-- > 0) {
        if ((r = (*fuse_priv->s3b->write_block)(fuse_priv->s3b, info.mid_block_start, info.mid_data, NULL, NULL, NULL)) != 0) {
            nbdkit_error("error writing block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.mid_block_start);
            nbdkit_set_error(r);
            return -1;
        }
        info.mid_block_start++;
        info.mid_data += config->block_size;
    }
    if (info.end_length > 0
      && (r = (*fuse_priv->s3b->write_block_part)(fuse_priv->s3b, info.end_block, 0, info.end_length, info.end_data)) != 0) {
        nbdkit_error("error writing block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.end_block);
        nbdkit_set_error(r);
        return -1;
    }

    // Done
    return 0;
}

static int
s3b_nbd_plugin_trim(void *handle, uint32_t size, uint64_t offset, uint32_t flags)
{
    struct boundary_info info;
    s3b_block_t *block_nums;
    int i;
    int r;

    // Calculate what bits to trim, then trim them
    calculate_boundary_info(&info, config->block_size, NULL, size, offset);
    if (info.beg_length > 0
      && (r = (*fuse_priv->s3b->write_block_part)(fuse_priv->s3b,
       info.beg_block, info.beg_offset, info.beg_length, zero_block)) != 0) {
        nbdkit_error("error writing block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.beg_block);
        nbdkit_set_error(r);
        return -1;
    }
    if (info.mid_block_count > 0) {

        // Use our "bulk_zero" functionality
        if ((block_nums = malloc(info.mid_block_count * sizeof(*block_nums))) == NULL) {
            nbdkit_set_error(errno);
            return -1;
        }
        for (i = 0; i < info.mid_block_count; i++)
            block_nums[i] = info.mid_block_start + i;
        if ((r = (*fuse_priv->s3b->bulk_zero)(fuse_priv->s3b, block_nums, info.mid_block_count)) != 0) {
            nbdkit_error("error zeroing %jd block(s) starting at %0*jx: %m",
              (uintmax_t)info.mid_block_count, S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.mid_block_start);
            nbdkit_set_error(r);
            return -1;
        }
        free(block_nums);
    }
    if (info.end_length > 0
      && (r = (*fuse_priv->s3b->write_block_part)(fuse_priv->s3b, info.end_block, 0, info.end_length, zero_block)) != 0) {
        nbdkit_error("error writing block %0*jx: %m", S3B_BLOCK_NUM_DIGITS, (uintmax_t)info.end_block);
        nbdkit_set_error(r);
        return -1;
    }

    // Done
    return 0;
}

// Pre-loading the cache is supported when the block cache is enabled
static int
s3b_nbd_plugin_can_cache(void *handle)
{
    return config->block_cache.cache_size > 0 ? NBDKIT_CACHE_EMULATE : NBDKIT_CACHE_NONE;
}

// Since we have no per-connection state, the same client may open multiple connections
static int
s3b_nbd_plugin_can_multi_conn(void *handle)
{
    return 1;
}