/*
 * Block driver for the Linux Unified Key Setup (LUKS)
 *
 * Copyright (c) 2014 Fam Zheng <famz@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu-common.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "migration/migration.h"

#define LUKS_MAGIC        "LUKS\xBA\xBE"
#define LUKS_SALTSIZE     32
#define LUKS_DIGESTSIZE   20
#define LUKS_NUMKEYS      8
#define LUKS_UUIDSIZE     40
#define LUKS_KEY_DISABLED 0x0000DEAD
#define LUKS_KEY_ENABLED  0x00AC71F3
#define LUKS_STRIPES      4000

typedef struct {
    char magic[6];
    uint16_t version;
    char cipher_name[32];
    char cipher_mode[32];
    char hash_spec[32];
    uint32_t payload_offset;
    uint32_t key_bytes;
    char mk_digest[LUKS_DIGESTSIZE];
    char mk_digest_salt[LUKS_SALTSIZE];
    uint32_t mk_digest_iterations;
    char uuid[LUKS_UUIDSIZE];
    struct {
        uint32_t active;
        /* parameters for PBKDF2 processing */
        uint32_t password_iterations;
        char password_salt[LUKS_SALTSIZE];
        /* parameters for Anti-Forensic store/load */
        uint32_t key_material_offset;
        uint32_t stripes;
    } key_slots[LUKS_NUMKEYS];
} LUKSHeader;

typedef struct {
} BDRVLuksState;

static int luks_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    return 0;
}

static int luks_open(BlockDriverState *bs, QDict *options, int flags,
                     Error **errp)
{
    return 0;
}

static coroutine_fn int luks_co_readv(BlockDriverState *bs, int64_t sector_num,
                                      int remaining_sectors, QEMUIOVector *qiov)
{
    return 0;
}

static coroutine_fn int luks_co_writev(BlockDriverState *bs,
                                       int64_t sector_num,
                                       int remaining_sectors,
                                       QEMUIOVector *qiov)
{
    return 0;
}

static void luks_close(BlockDriverState *bs)
{

}

static BlockDriver bdrv_luks = {
    .format_name                  = "luks",
    .instance_size                = sizeof(BDRVLuksState),
    .bdrv_probe                   = luks_probe,
    .bdrv_open                    = luks_open,
    .bdrv_co_readv                = luks_co_readv,
    .bdrv_co_writev               = luks_co_writev,
    .bdrv_close                   = luks_close,
};

static void bdrv_luks_init(void)
{
    bdrv_register(&bdrv_luks);
}

block_init(bdrv_luks_init);
