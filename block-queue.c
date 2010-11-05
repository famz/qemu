/*
 * QEMU System Emulator
 *
 * Copyright (c) 2010 Kevin Wolf <kwolf@redhat.com>
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

#include <signal.h>

#include "qemu-common.h"
#include "qemu-queue.h"
#include "block_int.h"
#include "block-queue.h"

#define BLKQUEUE_DEBUG
#ifdef BLKQUEUE_DEBUG
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define DPRINTF(...) do {} while(0)
#endif

#define WRITEBACK_MODES (BDRV_O_NOCACHE | BDRV_O_CACHE_WB)

/* TODO items for blkqueue
 *
 * - Error handling doesn't really exist. If something goes wrong with writing
 *   metadata we can't fail the guest request any more because it's long
 *   completed. Losing this data is actually okay, the guest hasn't flushed yet.
 *
 *   However, we need to be able to fail a flush, and we also need some way to
 *   handle errors transparently. This probably means that we have to stop the VM
 *   and let the user fix things so that we can retry. The only other way would be
 *   to shut down the VM and end up in the same situation as with a host crash.
 *
 *   Or maybe it would even be enough to start failing all new requests.
 *
 * - Need a real bdrv_aio_pwrite implementation
 */

enum blkqueue_req_type {
    REQ_TYPE_WRITE,
    REQ_TYPE_BARRIER,
};

typedef struct BlockQueueAIOCB {
    BlockDriverAIOCB common;
    QLIST_ENTRY(BlockQueueAIOCB) link;
} BlockQueueAIOCB;

typedef struct BlockQueueRequest {
    enum blkqueue_req_type type;
    BlockQueue* bq;

    uint64_t    offset;
    void*       buf;
    uint64_t    size;
    unsigned    section;

    QLIST_HEAD(, BlockQueueAIOCB) acbs;

    QTAILQ_ENTRY(BlockQueueRequest) link;
    QSIMPLEQ_ENTRY(BlockQueueRequest) link_section;
} BlockQueueRequest;

QTAILQ_HEAD(bq_queue_head, BlockQueueRequest);

struct BlockQueue {
    BlockDriverState*   bs;

    int                 barriers_requested;
    int                 barriers_submitted;
    int                 queue_size;
    int                 flushing;

    unsigned int            in_flight_num;
    enum blkqueue_req_type  in_flight_type;

    struct bq_queue_head    queue;
    struct bq_queue_head    in_flight;

    QSIMPLEQ_HEAD(, BlockQueueRequest) sections;
};

typedef int (*blkqueue_rw_fn)(BlockQueueContext *context, uint64_t offset,
    void *buf, uint64_t size);
typedef void (*blkqueue_handle_overlap)(void *new, void *old, size_t size);

static void blkqueue_process_request(BlockQueue *bq);
static void blkqueue_aio_cancel(BlockDriverAIOCB *blockacb);

static AIOPool blkqueue_aio_pool = {
    .aiocb_size         = sizeof(struct BlockQueueAIOCB),
    .cancel             = blkqueue_aio_cancel,
};

BlockQueue *blkqueue_create(BlockDriverState *bs)
{
    BlockQueue *bq = qemu_mallocz(sizeof(BlockQueue));
    bq->bs = bs;

    QTAILQ_INIT(&bq->queue);
    QTAILQ_INIT(&bq->in_flight);
    QSIMPLEQ_INIT(&bq->sections);

    return bq;
}

void blkqueue_init_context(BlockQueueContext* context, BlockQueue *bq)
{
    context->bq = bq;
    context->section = 0;
}

void blkqueue_destroy(BlockQueue *bq)
{
    blkqueue_flush(bq);

//    fprintf(stderr, "blkqueue_destroy: %d/%d barriers left\n",
//        bq->barriers_submitted, bq->barriers_requested);

    assert(QTAILQ_FIRST(&bq->in_flight) == NULL);
    assert(QTAILQ_FIRST(&bq->queue) == NULL);
    assert(QSIMPLEQ_FIRST(&bq->sections) == NULL);
    qemu_free(bq);
}

/*
 * Checks if a new read/write request accesses a region that is written by a
 * write request in the queue. If so, call the given overlap handler that can
 * use memcpy to work on the queue instead of accessing the disk.
 *
 * Returns true if the new request is handled completely, false if the caller
 * needs to continue accessing other queues or the disk.
 */
static bool blkqueue_check_queue_overlap(BlockQueueContext *context,
    struct bq_queue_head *queue, uint64_t *_offset, void **_buf,
    uint64_t *_size,
    blkqueue_rw_fn recurse, blkqueue_handle_overlap handle_overlap,
    int min_section)
{
    BlockQueueRequest *req;

    uint64_t offset = *_offset;
    void *buf       = *_buf;
    uint64_t size   = *_size;

    /* Reverse order to access most current data */
    QTAILQ_FOREACH_REVERSE(req, queue, bq_queue_head, link) {
        uint64_t end = offset + size;
        uint64_t req_end = req->offset + req->size;
        uint8_t *read_buf = buf;
        uint8_t *req_buf = req->buf;

        /* We're only interested in queued writes */
        if (req->type != REQ_TYPE_WRITE) {
            continue;
        }

        /* Ignore requests that are too early (needed for merging requests */
        if (req->section < min_section) {
            continue;
        }

        /*
         * If we read from a write in the queue (i.e. our read overlaps the
         * write request), our next write probably depends on this write, so
         * let's move forward to its section.
         *
         * If we're processing a new write, we definitely have a dependency,
         * because we must not overwrite the newer data by the older one.
         */
        if (end > req->offset && offset < req_end) {
            context->section = MAX(context->section, req->section);
        }

        /* How we continue, depends on the kind of overlap we have */
        if ((offset >= req->offset) && (end <= req_end)) {
            /* Completely contained in the queued request */
            handle_overlap(buf, &req_buf[offset - req->offset], size);
            return true;
        } else if ((end >= req->offset) && (end <= req_end)) {
            /* Overlap in the end of the new request */
            assert(offset < req->offset);
            handle_overlap(&read_buf[req->offset - offset], req_buf,
                end - req->offset);
            size = req->offset - offset;
        } else if ((offset >= req->offset) && (offset < req_end)) {
            /* Overlap in the start of the new request */
            assert(end > req_end);
            handle_overlap(read_buf, &req_buf[offset - req->offset],
                req_end - offset);
            buf = read_buf = &read_buf[req_end - offset];
            offset = req_end;
            size = end - req_end;
        } else if ((req->offset >= offset) && (req_end <= end)) {
            /*
             * The queued request is completely contained in the new request.
             * Use memcpy for the data from the queued request here, continue
             * with the data before the queued request and handle the data
             * after the queued request with a recursive call.
             */
            handle_overlap(&read_buf[req->offset - offset], req_buf,
                req_end - req->offset);
            size = req->offset - offset;
            recurse(context, req_end, &read_buf[req_end - offset],
                end - req_end);
        }
    }

    /* The caller must continue with the request */
    *_offset    = offset;
    *_buf       = buf;
    *_size      = size;

    return false;
}

static void pread_handle_overlap(void *new, void *old, size_t size)
{
    memcpy(new, old, size);
}

int blkqueue_pread(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size)
{
    BlockQueue *bq = context->bq;
    int ret;
    bool completed;

    /*
     * First check if there are any pending writes for the same data.
     *
     * The latest writes are in bq->queue, and if checking those isn't enough,
     * we have a second queue of requests that are already submitted, but
     * haven't completed yet.
     */
    completed = blkqueue_check_queue_overlap(context, &bq->queue, &offset,
        &buf, &size, &blkqueue_pread, &pread_handle_overlap, 0);

    if (!completed) {
        completed = blkqueue_check_queue_overlap(context, &bq->in_flight,
            &offset, &buf, &size, &blkqueue_pread, &pread_handle_overlap, 0);
    }

    if (completed) {
        return 0;
    }

    /* The requested is not written in the queue, read it from disk */
    ret = bdrv_pread(bq->bs, offset, buf, size);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static void pwrite_handle_overlap(void *new, void *old, size_t size)
{
    DPRINTF("update    pwrite: %p <- %p [%ld]\n", old, new, size);
    memcpy(old, new, size);
}

int blkqueue_pwrite(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *section_req;
    bool completed;

    /* Don't use the queue for writethrough images */
    if ((bq->bs->open_flags & WRITEBACK_MODES) == 0) {
        return bdrv_pwrite(bq->bs, offset, buf, size);
    }

    /* First check if there are any pending writes for the same data. */
    DPRINTF("--        pwrite: [%#lx + %ld]\n", offset, size);
    completed = blkqueue_check_queue_overlap(context, &bq->queue, &offset,
        &buf, &size, &blkqueue_pwrite, &pwrite_handle_overlap,
        context->section);

    if (completed) {
        return 0;
    }

    /* Create request structure */
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
    QLIST_INIT(&req->acbs);
    req->type       = REQ_TYPE_WRITE;
    req->bq         = bq;
    req->offset     = offset;
    req->size       = size;
    req->buf        = qemu_malloc(size);
    req->section    = context->section;
    memcpy(req->buf, buf, size);

    /*
     * Find the right place to insert it into the queue:
     * Right before the barrier that closes the current section.
     */
    QSIMPLEQ_FOREACH(section_req, &bq->sections, link_section) {
        if (section_req->section >= req->section) {
            req->section = section_req->section;
            context->section = section_req->section;
            QTAILQ_INSERT_BEFORE(section_req, req, link);
            goto out;
        }
    }

    /* If there was no barrier, just put it at the end. */
    QTAILQ_INSERT_TAIL(&bq->queue, req, link);

out:
    DPRINTF("queue-ins pwrite: %p [%#lx + %ld]\n", req, req->offset, req->size);
    bq->queue_size++;
#ifndef RUN_TESTS
    blkqueue_process_request(bq);
#endif

    return 0;
}

static int insert_barrier(BlockQueueContext *context, BlockQueueAIOCB *acb)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *section_req;

    bq->barriers_requested++;

    /* Create request structure */
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
    QLIST_INIT(&req->acbs);
    req->type       = REQ_TYPE_BARRIER;
    req->bq         = bq;
    req->section    = context->section;
    req->buf        = NULL;

    /* Find another barrier to merge with. */
    QSIMPLEQ_FOREACH(section_req, &bq->sections, link_section) {
        if (section_req->section >= req->section) {
            req->section = section_req->section;
            context->section = section_req->section + 1;
            qemu_free(req);
            req = section_req;
            goto out;
        }
    }

    /*
     * If there wasn't a barrier for the same section yet, insert a new one at
     * the end.
     */
    DPRINTF("queue-ins flush: %p\n", req);
    QTAILQ_INSERT_TAIL(&bq->queue, req, link);
    QSIMPLEQ_INSERT_TAIL(&bq->sections, req, link_section);
    bq->queue_size++;
    context->section++;

    bq->barriers_submitted++;

    /*
     * At this point, req is either the newly inserted request, or a previously
     * existing barrier with which the current request has been merged.
     *
     * Insert the ACB in the list of that request so that the callback is
     * called when the request has completed.
     */
out:
    if (acb) {
        QLIST_INSERT_HEAD(&req->acbs, acb, link);
    }

#ifndef RUN_TESTS
    blkqueue_process_request(bq);
#endif

    return 0;
}

int blkqueue_barrier(BlockQueueContext *context)
{
    /* Don't flush for writethrough images */
    if ((context->bq->bs->open_flags & WRITEBACK_MODES) == 0) {
        return 0;
    }

    return insert_barrier(context, NULL);
}

/*
 * Caller needs to hold the bq->lock mutex
 */
static BlockQueueRequest *blkqueue_pop(BlockQueue *bq)
{
    BlockQueueRequest *req;

    req = QTAILQ_FIRST(&bq->queue);
    if (req == NULL) {
        goto out;
    }

    QTAILQ_REMOVE(&bq->queue, req, link);
    bq->queue_size--;

    if (req->type == REQ_TYPE_BARRIER) {
        assert(QSIMPLEQ_FIRST(&bq->sections) == req);
        QSIMPLEQ_REMOVE_HEAD(&bq->sections, link_section);
    }

out:
    return req;
}

static void blkqueue_free_request(BlockQueueRequest *req)
{
    qemu_free(req->buf);
    qemu_free(req);
}

static void blkqueue_process_request_cb(void *opaque, int ret)
{
    BlockQueueRequest *req = opaque;
    BlockQueue *bq = req->bq;
    BlockQueueAIOCB *acb, *next;

    DPRINTF("  done    req:    %p [%#lx + %ld]\n", req, req->offset, req->size);

    /* TODO Error reporting! */
    QTAILQ_REMOVE(&bq->in_flight, req, link);
//fprintf(stderr, "Removing from in_flight: %p (ret = %d)\n", req, ret);
    QLIST_FOREACH_SAFE(acb, &req->acbs, link, next) {
        acb->common.cb(acb->common.opaque, 0); /* TODO ret */
        qemu_free(acb);
    }

    blkqueue_free_request(req);

    bq->in_flight_num--;

    blkqueue_process_request(bq);
}

static int blkqueue_submit_request(BlockQueue *bq)
{
    BlockDriverAIOCB *acb;
    BlockQueueRequest *req;

    /* Fetch a request */
    req = QTAILQ_FIRST(&bq->queue);
    if (req == NULL) {
        return -1;
    }

    /* Writethrough images aren't supposed to have any queue entries */
    assert((bq->bs->open_flags & WRITEBACK_MODES) != 0);

    /*
     * We need to wait for completion before we can submit new requests:
     * 1. If we're currently processing a barrier, or the new request is a
     *    barrier, we need to guarantee this barrier semantics.
     * 2. We must make sure that newer writes cannot pass older ones.
     */
    if (bq->in_flight_num > 0) {
        return -1;
    }

    /* Process barriers only if the queue is long enough */
    if (!bq->flushing) {
        if (req->type == REQ_TYPE_BARRIER && bq->queue_size < 50) {
            return -1;
        }
    }

    /*
     * Copy the request in the queue of currently processed requests so that
     * blkqueue_pread continues to read from the queue before the request has
     * completed.
     */
    blkqueue_pop(bq);
    QTAILQ_INSERT_TAIL(&bq->in_flight, req, link);

    bq->in_flight_num++;
    bq->in_flight_type = req->type;

    /* Submit the request */
    switch (req->type) {
        case REQ_TYPE_WRITE:
            DPRINTF("  process pwrite: %p [%#lx + %ld]\n",
                req, req->offset, req->size);
            acb = bdrv_aio_pwrite(bq->bs, req->offset, req->buf, req->size,
                blkqueue_process_request_cb, req);
            break;
        case REQ_TYPE_BARRIER:
            DPRINTF("  process flush\n");
            acb = bdrv_aio_flush(bq->bs, blkqueue_process_request_cb, req);
            break;
        default:
            /* Make gcc happy (acb would be uninitialized) */
            return -1;
    }

    if (!acb) {
        blkqueue_process_request_cb(req, -EIO);
        return -1;
    }

    return 0;
}

static void blkqueue_process_request(BlockQueue *bq)
{
    int ret = 0;

    while (ret >= 0) {
        ret = blkqueue_submit_request(bq);
    }
}

static void blkqueue_aio_cancel(BlockDriverAIOCB *blockacb)
{
    BlockQueueAIOCB *acb = (BlockQueueAIOCB*) blockacb;

    /*
     * We can't cancel the flush any more, but that doesn't hurt. We just
     * need to make sure that we don't call the callback when it completes.
     */
    QLIST_REMOVE(acb, link);
    qemu_free(acb);
}

BlockDriverAIOCB* blkqueue_aio_flush(BlockQueueContext *context,
    BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockQueueAIOCB *acb;
    BlockDriverState *bs = context->bq->bs;
    int ret;

    /* Don't use the queue for writethrough images */
    if ((bs->open_flags & WRITEBACK_MODES) == 0) {
        return bdrv_aio_flush(bs, cb, opaque);
    }

    /* Insert a barrier into the queue */
    acb = qemu_aio_get(&blkqueue_aio_pool, NULL, cb, opaque);

    /* FIXME Need to make sure that the barrier is not merged */
    ret = insert_barrier(context, acb);
    if (ret < 0) {
        cb(opaque, ret);
        qemu_free(acb);
    }

    return &acb->common;
}

void blkqueue_flush(BlockQueue *bq)
{
    bq->flushing = 1;

    /* Process any left over requests */
    while (bq->in_flight_num || QTAILQ_FIRST(&bq->queue)) {
        blkqueue_process_request(bq);
        qemu_aio_wait();
    }

    bq->flushing = 0;
}
