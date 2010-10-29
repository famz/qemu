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
#include "qemu-thread.h"
#include "qemu-barrier.h"
#include "block.h"
#include "block-queue.h"

/* TODO items for blkqueue
 *
 * - There's no locking between the worker thread and other functions accessing
 *   the same backend driver. Should be fine for file, but probably not for other
 *   backends.
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
 * - The Makefile integration is obviously very wrong, too. It worked for me good
 *   enough, but you need to be aware when block-queue.o is compiled with
 *   RUN_TESTS and when it isn't. The tests need to be split out properly.
 *
 * - Disable queue for cache=writethrough
 */

enum blkqueue_req_type {
    REQ_TYPE_WRITE,
    REQ_TYPE_BARRIER,
};

typedef struct BlockQueueRequest {
    enum blkqueue_req_type type;
    BlockQueue* bq;

    uint64_t    offset;
    void*       buf;
    uint64_t    size;
    unsigned    section;

    QTAILQ_ENTRY(BlockQueueRequest) link;
    QSIMPLEQ_ENTRY(BlockQueueRequest) link_section;
} BlockQueueRequest;

QTAILQ_HEAD(bq_queue_head, BlockQueueRequest);

struct BlockQueue {
    BlockDriverState*   bs;

    int                 barriers_requested;
    int                 barriers_submitted;
    int                 queue_size;

    unsigned int            in_flight_num;
    enum blkqueue_req_type  in_flight_type;

    struct bq_queue_head    queue;
    struct bq_queue_head    in_flight;

    QSIMPLEQ_HEAD(, BlockQueueRequest) sections;
};

static void blkqueue_process_request(BlockQueue *bq);

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

    fprintf(stderr, "blkqueue_destroy: %d/%d barriers left\n",
        bq->barriers_submitted, bq->barriers_requested);

    assert(QTAILQ_FIRST(&bq->in_flight) == NULL);
    assert(QTAILQ_FIRST(&bq->queue) == NULL);
    assert(QSIMPLEQ_FIRST(&bq->sections) == NULL);
    qemu_free(bq);
}

/*
 * Checks if a read request accesses a region that is written by a write
 * request in the queue. If so, memcpy the data from the write request.
 *
 * Returns true if the read request is handled completely, false if the caller
 * needs to continue reading from other queues or from the disk.
 */
static bool blkqueue_pread_check_queues(BlockQueueContext *context,
    struct bq_queue_head *queue, uint64_t *_offset, void **_buf,
    uint64_t *_size)
{
    BlockQueueRequest *req;

    uint64_t offset = *_offset;
    void *buf       = *_buf;
    uint64_t size   = *_size;

    /* Reverse order to return data written by the latest write. */
    QTAILQ_FOREACH_REVERSE(req, queue, bq_queue_head, link) {
        uint64_t end = offset + size;
        uint64_t req_end = req->offset + req->size;
        uint8_t *read_buf = buf;
        uint8_t *req_buf = req->buf;

        /* We're only interested in queued writes */
        if (req->type != REQ_TYPE_WRITE) {
            continue;
        }

        /*
         * If we read from a write in the queue (i.e. our read overlaps the
         * write request), our next write probably depends on this write, so
         * let's move forward to its section.
         */
        if (end > req->offset && offset < req_end) {
            context->section = MAX(context->section, req->section);
        }

        /* How we continue, depends on the kind of overlap we have */
        if ((offset >= req->offset) && (end <= req_end)) {
            /* Completely contained in the write request */
            memcpy(buf, &req_buf[offset - req->offset], size);
            return true;
        } else if ((end >= req->offset) && (end <= req_end)) {
            /* Overlap in the end of the read request */
            assert(offset < req->offset);
            memcpy(&read_buf[req->offset - offset], req_buf, end - req->offset);
            size = req->offset - offset;
        } else if ((offset >= req->offset) && (offset < req_end)) {
            /* Overlap in the start of the read request */
            assert(end > req_end);
            memcpy(read_buf, &req_buf[offset - req->offset], req_end - offset);
            buf = read_buf = &read_buf[req_end - offset];
            offset = req_end;
            size = end - req_end;
        } else if ((req->offset >= offset) && (req_end <= end)) {
            /*
             * The write request is completely contained in the read request.
             * memcpy the data from the write request here, continue with the
             * data before the write request and handle the data after the
             * write request with a recursive call.
             */
            memcpy(&read_buf[req->offset - offset], req_buf, req_end - req->offset);
            size = req->offset - offset;
            blkqueue_pread(context, req_end, &read_buf[req_end - offset], end - req_end);
        }
    }

    /* The caller must continue with the request */
    *_offset    = offset;
    *_buf       = buf;
    *_size      = size;

    return false;
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
    completed = blkqueue_pread_check_queues(context, &bq->queue, &offset,
        &buf, &size);

    if (!completed) {
        completed = blkqueue_pread_check_queues(context, &bq->in_flight,
            &offset, &buf, &size);
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

int blkqueue_pwrite(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *section_req;

    /* Create request structure */
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
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
            bq->queue_size++;
            goto out;
        }
    }

    /* If there was no barrier, just put it at the end. */
    QTAILQ_INSERT_TAIL(&bq->queue, req, link);
    bq->queue_size++;

#ifndef RUN_TESTS
    blkqueue_process_request(bq);
#endif

out:
    return 0;
}

int blkqueue_barrier(BlockQueueContext *context)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *section_req;

    bq->barriers_requested++;

    /* Create request structure */
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
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
            goto out;
        }
    }

    /*
     * If there wasn't a barrier for the same section yet, insert a new one at
     * the end.
     */
    QTAILQ_INSERT_TAIL(&bq->queue, req, link);
    QSIMPLEQ_INSERT_TAIL(&bq->sections, req, link_section);
    bq->queue_size++;
    context->section++;

    bq->barriers_submitted++;

#ifndef RUN_TESTS
    blkqueue_process_request(bq);
#endif

out:
    return 0;
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

    /* TODO Error reporting! */
    QTAILQ_REMOVE(&bq->in_flight, req, link);
fprintf(stderr, "Removing from in_flight: %p (ret = %d)\n", req, ret);
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

    /*
     * If we're currently processing a barrier, or the new request is a
     * barrier, we need to guarantee this barrier semantics, i.e. we need to
     * wait for completion before we can submit new requests.
     */
    if (bq->in_flight_num > 0 && bq->in_flight_type != req->type) {
        return -1;
    }

    /*
     * Copy the request in the queue of currently processed requests so that
     * blkqueue_pread continues to read from the queue before the request has
     * completed.
     */
    blkqueue_pop(bq);
    QTAILQ_INSERT_TAIL(&bq->in_flight, req, link);
fprintf(stderr, "Inserting to in_flight: %p\n", req);

    bq->in_flight_num++;
    bq->in_flight_type = req->type;

    /* Submit the request */
    switch (req->type) {
        case REQ_TYPE_WRITE:
            acb = bdrv_aio_pwrite(bq->bs, req->offset, req->buf, req->size,
                blkqueue_process_request_cb, req);
            break;
        case REQ_TYPE_BARRIER:
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

struct blkqueue_flush_aiocb {
    BlockQueue *bq;
    BlockDriverCompletionFunc *cb;
    void *opaque;
};

static void *blkqueue_aio_flush_thread(void *opaque)
{
    struct blkqueue_flush_aiocb *acb = opaque;

    /* Process any left over requests */
    blkqueue_flush(acb->bq);

    acb->cb(acb->opaque, 0);
    qemu_free(acb);

    return NULL;
}

void blkqueue_aio_flush(BlockQueue *bq, BlockDriverCompletionFunc *cb,
    void *opaque)
{
    struct blkqueue_flush_aiocb *acb;

    acb = qemu_malloc(sizeof(*acb));
    acb->bq = bq;
    acb->cb = cb;
    acb->opaque = opaque;

    /* FIXME This is very broken */
    qemu_thread_create(NULL, blkqueue_aio_flush_thread, acb);
}

void blkqueue_flush(BlockQueue *bq)
{
    /* Process any left over requests */
    while (bq->in_flight_num || QTAILQ_FIRST(&bq->queue)) {
        blkqueue_process_request(bq);
        qemu_aio_wait();
    }
}

#ifdef RUN_TESTS

#define CHECK_WRITE(req, _bq, _offset, _size, _buf, _section) \
    do { \
        assert(req != NULL); \
        assert(req->type == REQ_TYPE_WRITE); \
        assert(req->bq == _bq); \
        assert(req->offset == _offset); \
        assert(req->size == _size); \
        assert(req->section == _section); \
        assert(!memcmp(req->buf, _buf, _size)); \
    } while(0)

#define CHECK_BARRIER(req, _bq, _section) \
    do { \
        assert(req != NULL); \
        assert(req->type == REQ_TYPE_BARRIER); \
        assert(req->bq == _bq); \
        assert(req->section == _section); \
    } while(0)

#define CHECK_READ(_context, _offset, _buf, _size, _cmpbuf) \
    do { \
        int ret; \
        memset(buf, 0, 512); \
        ret = blkqueue_pread(_context, _offset, _buf, _size); \
        assert(ret == 0); \
        assert(!memcmp(_cmpbuf, _buf, _size)); \
    } while(0)

#define QUEUE_WRITE(_context, _offset, _buf, _size, _pattern) \
    do { \
        int ret; \
        memset(_buf, _pattern, _size); \
        ret = blkqueue_pwrite(_context, _offset, _buf, _size); \
        assert(ret == 0); \
    } while(0)
#define QUEUE_BARRIER(_context) \
    do { \
        int ret; \
        ret = blkqueue_barrier(_context); \
        assert(ret == 0); \
    } while(0)

#define POP_CHECK_WRITE(_bq, _offset, _buf, _size, _pattern, _section) \
    do { \
        BlockQueueRequest *req; \
        memset(_buf, _pattern, _size); \
        req = blkqueue_pop(_bq); \
        CHECK_WRITE(req, _bq, _offset, _size, _buf, _section); \
        blkqueue_free_request(req); \
    } while(0)
#define POP_CHECK_BARRIER(_bq, _section) \
    do { \
        BlockQueueRequest *req; \
        req = blkqueue_pop(_bq); \
        CHECK_BARRIER(req, _bq, _section); \
        blkqueue_free_request(req); \
    } while(0)

static void  __attribute__((used)) dump_queue(BlockQueue *bq)
{
    BlockQueueRequest *req;

    fprintf(stderr, "--- Queue dump ---\n");
    QTAILQ_FOREACH(req, &bq->queue, link) {
        fprintf(stderr, "[%d] ", req->section);
        if (req->type == REQ_TYPE_WRITE) {
            fprintf(stderr, "Write off=%5"PRId64", len=%5"PRId64", buf=%p\n",
                req->offset, req->size, req->buf);
        } else if (req->type == REQ_TYPE_BARRIER) {
            fprintf(stderr, "Barrier\n");
        } else {
            fprintf(stderr, "Unknown type %d\n", req->type);
        }
    }
}

static void test_basic(BlockDriverState *bs)
{
    uint8_t buf[512];
    BlockQueue *bq;
    BlockQueueContext context;

    bq = blkqueue_create(bs);
    blkqueue_init_context(&context, bq);

    /* Queue requests */
    QUEUE_WRITE(&context,   0, buf, 512, 0x12);
    QUEUE_WRITE(&context, 512, buf,  42, 0x34);
    QUEUE_BARRIER(&context);
    QUEUE_WRITE(&context, 678, buf,  42, 0x56);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x12, 0);
    POP_CHECK_WRITE(bq,   512, buf,  42, 0x34, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,   678, buf,  42, 0x56, 1);

    blkqueue_destroy(bq);
}

static void test_merge(BlockDriverState *bs)
{
    uint8_t buf[512];
    BlockQueue *bq;
    BlockQueueContext ctx1, ctx2;

    bq = blkqueue_create(bs);
    blkqueue_init_context(&ctx1, bq);
    blkqueue_init_context(&ctx2, bq);

    /* Queue requests */
    QUEUE_WRITE(&ctx1,    0, buf, 512, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx2,  512, buf,  42, 0x34);
    QUEUE_WRITE(&ctx1, 1024, buf, 512, 0x12);
    QUEUE_BARRIER(&ctx2);
    QUEUE_WRITE(&ctx2, 1512, buf,  42, 0x34);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x12, 0);
    POP_CHECK_WRITE(bq,   512, buf,  42, 0x34, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,  1024, buf, 512, 0x12, 1);
    POP_CHECK_WRITE(bq,  1512, buf,  42, 0x34, 1);

    /* Same queue, new contexts */
    blkqueue_init_context(&ctx1, bq);
    blkqueue_init_context(&ctx2, bq);

    /* Queue requests */
    QUEUE_BARRIER(&ctx2);
    QUEUE_WRITE(&ctx2,  512, buf,  42, 0x34);
    QUEUE_WRITE(&ctx2,   12, buf,  20, 0x45);
    QUEUE_BARRIER(&ctx2);
    QUEUE_WRITE(&ctx2,  892, buf, 142, 0x56);

    QUEUE_WRITE(&ctx1,    0, buf,   8, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx1, 1024, buf, 512, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx1, 1512, buf,  42, 0x34);
    QUEUE_BARRIER(&ctx1);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf,   8, 0x12, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,   512, buf,  42, 0x34, 1);
    POP_CHECK_WRITE(bq,    12, buf,  20, 0x45, 1);
    POP_CHECK_WRITE(bq,  1024, buf, 512, 0x12, 1);
    POP_CHECK_BARRIER(bq, 1);
    POP_CHECK_WRITE(bq,   892, buf, 142, 0x56, 2);
    POP_CHECK_WRITE(bq,  1512, buf,  42, 0x34, 2);
    POP_CHECK_BARRIER(bq, 2);

    blkqueue_destroy(bq);
}

static void test_read(BlockDriverState *bs)
{
    uint8_t buf[512], buf2[512];
    BlockQueue *bq;
    BlockQueueContext ctx1;

    bq = blkqueue_create(bs);
    blkqueue_init_context(&ctx1, bq);

    /* Queue requests and do some test reads */
    memset(buf2, 0xa5, 512);
    CHECK_READ(&ctx1, 0, buf, 32, buf2);

    QUEUE_WRITE(&ctx1, 5, buf, 5, 0x12);
    memset(buf2, 0x12, 5);
    CHECK_READ(&ctx1,  5, buf, 5, buf2);
    CHECK_READ(&ctx1,  7, buf, 2, buf2);
    memset(buf2, 0xa5, 512);
    memset(buf2 + 5, 0x12, 5);
    CHECK_READ(&ctx1,  0, buf, 8, buf2);
    CHECK_READ(&ctx1,  0, buf, 10, buf2);
    CHECK_READ(&ctx1,  0, buf, 32, buf2);
    memset(buf2, 0xa5, 512);
    memset(buf2, 0x12, 5);
    CHECK_READ(&ctx1,  5, buf, 16, buf2);
    memset(buf2, 0xa5, 512);
    CHECK_READ(&ctx1,  0, buf,  2, buf2);
    CHECK_READ(&ctx1, 10, buf, 16, buf2);

    QUEUE_WRITE(&ctx1, 0, buf, 2, 0x12);
    memset(&buf2[5], 0x12, 5);
    memset(buf2, 0x12, 2);
    CHECK_READ(&ctx1,  0, buf, 32, buf2);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     5, buf,   5, 0x12, 0);
    POP_CHECK_WRITE(bq,     0, buf,   2, 0x12, 0);

    blkqueue_destroy(bq);
}

static void test_read_order(BlockDriverState *bs)
{
    uint8_t buf[512], buf2[512];
    BlockQueue *bq;
    BlockQueueContext ctx1, ctx2;

    bq = blkqueue_create(bs);
    blkqueue_init_context(&ctx1, bq);
    blkqueue_init_context(&ctx2, bq);

    /* Queue requests and do some test reads */
    QUEUE_WRITE(&ctx1, 25, buf, 5, 0x44);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx1, 5, buf, 5, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx2, 10, buf, 5, 0x34);

    memset(buf2, 0xa5, 512);
    memset(buf2 + 5, 0x12, 5);
    memset(buf2 + 10, 0x34, 5);
    CHECK_READ(&ctx2, 0, buf, 20, buf2);
    QUEUE_WRITE(&ctx2,  0, buf, 10, 0x34);
    QUEUE_BARRIER(&ctx2);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,    25, buf,   5, 0x44, 0);
    POP_CHECK_WRITE(bq,    10, buf,   5, 0x34, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,     5, buf,   5, 0x12, 1);
    POP_CHECK_WRITE(bq,     0, buf,  10, 0x34, 1);
    POP_CHECK_BARRIER(bq, 1);

    blkqueue_destroy(bq);
}

static void test_process_request(BlockDriverState *bs)
{
    uint8_t buf[512], buf2[512];
    BlockQueue *bq;
    BlockQueueContext ctx1;

    bq = blkqueue_create(bs);
    blkqueue_init_context(&ctx1, bq);

    /* Queue requests and do a test read */
    QUEUE_WRITE(&ctx1, 25, buf, 5, 0x44);
    QUEUE_BARRIER(&ctx1);

    memset(buf2, 0xa5, 512);
    memset(buf2 + 25, 0x44, 5);
    CHECK_READ(&ctx1, 0, buf, 64, buf2);

    /* Process the requests */
    blkqueue_process_request(bq);
    assert(bq->in_flight_num == 1);
    assert(bq->in_flight_type == REQ_TYPE_WRITE);

    /* Check if we still read the same */
    CHECK_READ(&ctx1, 0, buf, 64, buf2);

    /* Process the AIO requests and check again */
    qemu_aio_flush();
    assert(bq->barriers_submitted == 1);
    assert(bq->in_flight_num == 0);
    CHECK_READ(&ctx1, 0, buf, 64, buf2);

    /* Verify the queue is empty */
    assert(blkqueue_pop(bq) == NULL);

    /* Check that processing an empty queue works */
    blkqueue_process_request(bq);

    blkqueue_destroy(bq);
}

static void run_test(void (*testfn)(BlockDriverState*), BlockDriverState *bs)
{
    void* buf;
    int ret;

    buf = qemu_malloc(1024 * 1024);
    memset(buf, 0xa5, 1024 * 1024);
    ret = bdrv_write(bs, 0, buf, 2048);
    assert(ret >= 0);
    qemu_free(buf);

    testfn(bs);
}

int main(void)
{
    BlockDriverState *bs;
    int ret;

    bdrv_init();
    bs = bdrv_new("");
    ret = bdrv_open(bs, "block-queue.img", BDRV_O_RDWR, NULL);
    if (ret < 0) {
        fprintf(stderr, "Couldn't open block-queue.img: %s\n",
            strerror(-ret));
        exit(1);
    }

    run_test(&test_basic, bs);
    run_test(&test_merge, bs);
    run_test(&test_read, bs);
    run_test(&test_read_order, bs);
    run_test(&test_process_request, bs);

    bdrv_delete(bs);

    return 0;
}
#endif
