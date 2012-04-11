/*
 * Image mirroring
 *
 * Copyright Red Hat, Inc. 2012
 *
 * Authors:
 *  Paolo Bonzini  <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "blockjob.h"
#include "block_int.h"
#include "qemu/ratelimit.h"

enum {
    /*
     * Size of data buffer for populating the image file.  This should be large
     * enough to process multiple clusters in a single call, so that populating
     * contiguous regions of the image is efficient.
     */
    BLOCK_SIZE = 512 * BDRV_SECTORS_PER_DIRTY_CHUNK, /* in bytes */
};

#define SLICE_TIME 100000000ULL /* ns */

typedef struct MirrorBlockJob {
    BlockJob common;
    RateLimit limit;
    BlockDriverState *target;
    bool full;
} MirrorBlockJob;

static int coroutine_fn mirror_populate(MirrorBlockJob *s,
                                        int64_t sector_num, int nb_sectors,
                                        void *buf)
{
    BlockDriverState *source = s->common.bs;
    BlockDriverState *target = s->target;
    struct iovec iov = {
        .iov_base = buf,
        .iov_len  = nb_sectors * 512,
    };
    QEMUIOVector qiov;
    int ret;

    qemu_iovec_init_external(&qiov, &iov, 1);

    /* Copy the dirty cluster.  */
    ret = bdrv_co_readv(source, sector_num, nb_sectors, &qiov);
    if (ret < 0) {
        return ret;
    }
    return bdrv_co_writev(target, sector_num, nb_sectors, &qiov);
}

static void coroutine_fn mirror_run(void *opaque)
{
    MirrorBlockJob *s = opaque;
    BlockDriverState *bs = s->common.bs;
    BlockDriverState *base;
    int64_t sector_num, end;
    int ret = 0;
    int n;
    bool synced = false;
    void *buf;

    if (block_job_is_cancelled(&s->common)) {
        goto immediate_exit;
    }

    s->common.len = bdrv_getlength(bs);
    if (s->common.len < 0) {
        block_job_completed(&s->common, s->common.len);
        return;
    }

    base = s->full ? NULL : bs->backing_hd;
    end = s->common.len >> BDRV_SECTOR_BITS;
    buf = qemu_blockalign(bs, BLOCK_SIZE);

    /* First part, loop on the sectors and initialize the dirty bitmap.  */
    for (sector_num = 0; sector_num < end; ) {
        int64_t next = (sector_num | (BDRV_SECTORS_PER_DIRTY_CHUNK - 1)) + 1;
        ret = bdrv_co_is_allocated_above(bs, base,
                                         sector_num, next - sector_num, &n);

        if (ret < 0) {
            break;
        } else if (ret == 1) {
            bdrv_set_dirty(bs, sector_num, n);
            sector_num = next;
        } else {
            sector_num += n;
        }
    }

    if (ret < 0) {
        block_job_completed(&s->common, ret);
    }

    sector_num = -1;
    for (;;) {
        uint64_t delay_ns;
        int64_t cnt;
        bool should_complete;

        if (bdrv_get_dirty_count(bs) != 0) {
            int nb_sectors;
            sector_num = bdrv_get_next_dirty(bs, sector_num);
            nb_sectors = MIN(BDRV_SECTORS_PER_DIRTY_CHUNK, end - sector_num);
            trace_mirror_one_iteration(s, sector_num);
            bdrv_reset_dirty(bs, sector_num, BDRV_SECTORS_PER_DIRTY_CHUNK);
            ret = mirror_populate(s, sector_num, nb_sectors, buf);
            if (ret < 0) {
                break;
            }
        }

        if (bdrv_get_dirty_count(bs) == 0) {
            /* We're out of the streaming phase.  From now on, if the
             * job is cancelled we will actually complete all pending
             * I/O and report completion, so that drive-reopen can be
             * used to pivot to the mirroring target.
             */
            synced = true;
            s->common.offset = end * BDRV_SECTOR_SIZE;
        }

        should_complete = synced && block_job_is_cancelled(&s->common);
        if (should_complete) {
            /* The dirty bitmap is not updated while operations are pending.
             * If we're about to exit, wait for pending operations before
             * calling bdrv_get_dirty_count(bs), or we may exit while the
             * source has dirty data to copy!
             *
             * Note that I/O can be submitted by the guest while
             * mirror_populate runs.
             */
            bdrv_drain_all();
        }

        ret = 0;
        cnt = bdrv_get_dirty_count(bs);
        if (synced) {
            if (!should_complete) {
                delay_ns = (cnt == 0 ? SLICE_TIME : 0);
                block_job_sleep_ns(&s->common, rt_clock, delay_ns);
                continue;
            }

            if (cnt == 0) {
                /* The two disks are in sync.  Exit and report successful
                 * completion.
                 */
                assert(QLIST_EMPTY(&bs->tracked_requests));
                s->common.cancelled = false;
                break;
            }
        } else {
            /* Publish progress */
            s->common.offset = end * BDRV_SECTOR_SIZE - cnt * BLOCK_SIZE;

            if (s->common.speed) {
                delay_ns = ratelimit_calculate_delay(&s->limit, BDRV_SECTORS_PER_DIRTY_CHUNK);
            } else {
                delay_ns = 0;
            }

            /* Note that even when no rate limit is applied we need to yield
             * with no pending I/O here so that qemu_aio_flush() returns.
             */
            block_job_sleep_ns(&s->common, rt_clock, delay_ns);
            if (block_job_is_cancelled(&s->common)) {
                break;
            }
        }
    }

immediate_exit:
    bdrv_set_dirty_tracking(bs, false);
    bdrv_close(s->target);
    bdrv_delete(s->target);
    block_job_completed(&s->common, ret);
}

static void mirror_set_speed(BlockJob *job, int64_t speed, Error **errp)
{
    MirrorBlockJob *s = container_of(job, MirrorBlockJob, common);

    if (speed < 0) {
        error_set(errp, QERR_INVALID_PARAMETER, "speed");
        return;
    }
    ratelimit_set_speed(&s->limit, speed / BDRV_SECTOR_SIZE, SLICE_TIME);
}

static BlockJobType mirror_job_type = {
    .instance_size = sizeof(MirrorBlockJob),
    .job_type      = "mirror",
    .set_speed     = mirror_set_speed,
};

void mirror_start(BlockDriverState *bs, BlockDriverState *target,
                  int64_t speed, bool full,
                  BlockDriverCompletionFunc *cb,
                  void *opaque, Error **errp)
{
    MirrorBlockJob *s;

    s = block_job_create(&mirror_job_type, bs, speed, cb, opaque, errp);
    if (!s) {
        return;
    }

    s->target = target;
    s->full = full;
    bdrv_set_dirty_tracking(bs, true);
    s->common.co = qemu_coroutine_create(mirror_run);
    trace_mirror_start(bs, s, s->common.co, opaque);
    qemu_coroutine_enter(s->common.co, s);
}
