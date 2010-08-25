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

enum blkqueue_req_type {
    REQ_TYPE_WRITE,
    REQ_TYPE_BARRIER,
};

typedef struct BlockQueueRequest {
    enum blkqueue_req_type type;

    uint64_t    offset;
    void*       buf;
    uint64_t    size;
    unsigned    section;

    QTAILQ_ENTRY(BlockQueueRequest) link;
    QSIMPLEQ_ENTRY(BlockQueueRequest) link_section;
} BlockQueueRequest;

struct BlockQueue {
    BlockDriverState*   bs;

    QemuThread          thread;
    bool                thread_done;
    QemuMutex           lock;
    QemuMutex           flush_lock;
    QemuCond            cond;

    QTAILQ_HEAD(bq_queue_head, BlockQueueRequest) queue;
    QSIMPLEQ_HEAD(, BlockQueueRequest) sections;
};

static void *blkqueue_thread(void *bq);

BlockQueue *blkqueue_create(BlockDriverState *bs)
{
    BlockQueue *bq = qemu_mallocz(sizeof(BlockQueue));
    bq->bs = bs;

    QTAILQ_INIT(&bq->queue);
    QSIMPLEQ_INIT(&bq->sections);

    qemu_mutex_init(&bq->lock);
    qemu_mutex_init(&bq->flush_lock);
    qemu_cond_init(&bq->cond);

    bq->thread_done = false;
    qemu_thread_create(&bq->thread, blkqueue_thread, bq);

    return bq;
}

void blkqueue_init_context(BlockQueueContext* context, BlockQueue *bq)
{
    context->bq = bq;
    context->section = 0;
}

void blkqueue_destroy(BlockQueue *bq)
{
    bq->thread_done = true;
    qemu_cond_signal(&bq->cond);
    qemu_thread_join(&bq->thread);

    blkqueue_flush(bq);

    qemu_mutex_destroy(&bq->lock);
    qemu_mutex_destroy(&bq->flush_lock);
    qemu_cond_destroy(&bq->cond);

    assert(QTAILQ_FIRST(&bq->queue) == NULL);
    assert(QSIMPLEQ_FIRST(&bq->sections) == NULL);
    qemu_free(bq);
}

int blkqueue_pread(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *req;
    int ret;

    /*
     * First check if there are any pending writes for the same data. Reverse
     * order to return data written by the latest write.
     */
    QTAILQ_FOREACH_REVERSE(req, &bq->queue, bq_queue_head, link) {
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
            return 0;
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
    req->offset     = offset;
    req->size       = size;
    req->buf        = qemu_malloc(size);
    req->section    = context->section;
    memcpy(req->buf, buf, size);

    /*
     * Find the right place to insert it into the queue:
     * Right before the barrier that closes the current section.
     */
    qemu_mutex_lock(&bq->lock);
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
    qemu_cond_signal(&bq->cond);

out:
    qemu_mutex_unlock(&bq->lock);
    return 0;
}

int blkqueue_barrier(BlockQueueContext *context)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *section_req;

    /* Create request structure */
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
    req->type       = REQ_TYPE_BARRIER;
    req->section    = context->section;
    req->buf        = NULL;

    /* Find another barrier to merge with. */
    qemu_mutex_lock(&bq->lock);
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
    context->section++;
    qemu_cond_signal(&bq->cond);

out:
    qemu_mutex_unlock(&bq->lock);
    return 0;
}

static BlockQueueRequest *blkqueue_pop(BlockQueue *bq)
{
    BlockQueueRequest *req;

    qemu_mutex_lock(&bq->lock);
    req = QTAILQ_FIRST(&bq->queue);
    if (req == NULL) {
        goto out;
    }

    QTAILQ_REMOVE(&bq->queue, req, link);
    if (req->type == REQ_TYPE_BARRIER) {
        assert(QSIMPLEQ_FIRST(&bq->sections) == req);
        QSIMPLEQ_REMOVE_HEAD(&bq->sections, link_section);
    }

out:
    qemu_mutex_unlock(&bq->lock);
    return req;
}

static void blkqueue_free_request(BlockQueueRequest *req)
{
    qemu_free(req->buf);
    qemu_free(req);
}

static void blkqueue_process_request(BlockQueue *bq)
{
    BlockQueueRequest *req;
    BlockQueueRequest *req2;
    int ret;

    /*
     * Note that we leave the request in the queue while we process it. No
     * other request will be queued before this one and we have only one thread
     * that processes the queue, so afterwards it will still be the first
     * request. (XXX Not true for barriers in the first position)
     */
    req = QTAILQ_FIRST(&bq->queue);
    if (req == NULL) {
        return;
    }

    switch (req->type) {
        case REQ_TYPE_WRITE:
            ret = bdrv_pwrite(bq->bs, req->offset, req->buf, req->size);
            if (ret < 0) {
                /* TODO Error reporting! */
                return;
            }
            break;
        case REQ_TYPE_BARRIER:
            bdrv_flush(bq->bs);
            break;
    }

    /*
     * Only remove the request from the queue when it's written, so that reads
     * always access the right data.
     */
    req2 = blkqueue_pop(bq);
    assert(req == req2);
    blkqueue_free_request(req);
}

void blkqueue_flush(BlockQueue *bq)
{
    qemu_mutex_lock(&bq->flush_lock);

    /* The thread only gives up the lock when the queue is empty */
    assert(QTAILQ_FIRST(&bq->queue) == NULL);

    qemu_mutex_unlock(&bq->flush_lock);
}

static void *blkqueue_thread(void *_bq)
{
    BlockQueue *bq = _bq;

    qemu_mutex_lock(&bq->flush_lock);
    while (!bq->thread_done) {
        barrier();
#ifndef RUN_TESTS
        blkqueue_process_request(bq);
        if (QTAILQ_FIRST(&bq->queue) == NULL) {
            qemu_cond_wait(&bq->cond, &bq->flush_lock);
        }
#else
        qemu_cond_wait(&bq->cond, &bq->flush_lock);
#endif
    }
    qemu_mutex_unlock(&bq->flush_lock);

    return NULL;
}

#ifdef RUN_TESTS

#define CHECK_WRITE(req, _offset, _size, _buf, _section) \
    do { \
        assert(req != NULL); \
        assert(req->type == REQ_TYPE_WRITE); \
        assert(req->offset == _offset); \
        assert(req->size == _size); \
        assert(req->section == _section); \
        assert(!memcmp(req->buf, _buf, _size)); \
    } while(0)

#define CHECK_BARRIER(req, _section) \
    do { \
        assert(req != NULL); \
        assert(req->type == REQ_TYPE_BARRIER); \
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
        CHECK_WRITE(req, _offset, _size, _buf, _section); \
        blkqueue_free_request(req); \
    } while(0)
#define POP_CHECK_BARRIER(_bq, _section) \
    do { \
        BlockQueueRequest *req; \
        req = blkqueue_pop(_bq); \
        CHECK_BARRIER(req, _section); \
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

    /* Process the queue (plus one call to test a NULL condition) */
    blkqueue_process_request(bq);
    blkqueue_process_request(bq);
    blkqueue_process_request(bq);

    /* Verify the queue is empty */
    assert(blkqueue_pop(bq) == NULL);

    /* Check if we still read the same */
    CHECK_READ(&ctx1, 0, buf, 64, buf2);

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
