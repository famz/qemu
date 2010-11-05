/*
 * block-queue.c unit tests
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

/* We want to test some static functions, so just include the source file */
#define RUN_TESTS
#include "block-queue.c"

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
    QUEUE_WRITE(&ctx2, 1536, buf,  42, 0x34);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x12, 0);
    POP_CHECK_WRITE(bq,   512, buf,  42, 0x34, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,  1024, buf, 512, 0x12, 1);
    POP_CHECK_WRITE(bq,  1536, buf,  42, 0x34, 1);

    /* Same queue, new contexts */
    blkqueue_init_context(&ctx1, bq);
    blkqueue_init_context(&ctx2, bq);

    /* Queue requests */
    QUEUE_BARRIER(&ctx2);
    QUEUE_WRITE(&ctx2,  512, buf,  42, 0x34);
    QUEUE_WRITE(&ctx2,   12, buf,  20, 0x45);
    QUEUE_BARRIER(&ctx2);
    QUEUE_WRITE(&ctx2, 2892, buf, 142, 0x56);

    QUEUE_WRITE(&ctx1,    0, buf,   8, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx1, 1024, buf, 512, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx1, 2512, buf,  42, 0x34);
    QUEUE_BARRIER(&ctx1);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf,   8, 0x12, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,   512, buf,  42, 0x34, 1);
    POP_CHECK_WRITE(bq,    12, buf,  20, 0x45, 1);
    POP_CHECK_WRITE(bq,  1024, buf, 512, 0x12, 1);
    POP_CHECK_BARRIER(bq, 1);
    POP_CHECK_WRITE(bq,  2892, buf, 142, 0x56, 2);
    POP_CHECK_WRITE(bq,  2512, buf,  42, 0x34, 2);
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
    POP_CHECK_WRITE(bq,     5, buf,   5, 0x34, 1);
    POP_CHECK_WRITE(bq,     0, buf,   5, 0x34, 1);
    POP_CHECK_BARRIER(bq, 1);

    blkqueue_destroy(bq);
}

static void test_write_order(BlockDriverState *bs)
{
    uint8_t buf[512], buf2[512];
    BlockQueue *bq;
    BlockQueueContext context;

    bq = blkqueue_create(bs);

    /* Merging two writes */
    /* Queue requests */
    blkqueue_init_context(&context, bq);
    QUEUE_WRITE(&context,   0, buf, 512, 0x12);
    QUEUE_BARRIER(&context);
    QUEUE_WRITE(&context, 512, buf, 512, 0x56);

    blkqueue_init_context(&context, bq);
    QUEUE_WRITE(&context, 512, buf, 512, 0x34);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x12, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,   512, buf, 512, 0x34, 1);

    /* Queue requests once again */
    blkqueue_init_context(&context, bq);
    QUEUE_WRITE(&context,   0, buf, 512, 0x12);
    QUEUE_BARRIER(&context);
    QUEUE_WRITE(&context, 512, buf, 512, 0x56);

    blkqueue_init_context(&context, bq);
    QUEUE_WRITE(&context, 512, buf, 512, 0x34);

    /* Check if the right values are read back */
    memset(buf2, 0x34, 512);
    CHECK_READ(&context, 512, buf, 512, buf2);
    blkqueue_process_request(bq);
    qemu_aio_flush();
    memset(buf2, 0x34, 512);
    CHECK_READ(&context, 512, buf, 512, buf2);

    blkqueue_flush(bq);

    /* Must not merge with write in earlier section */
    /* Queue requests */
    blkqueue_init_context(&context, bq);
    QUEUE_WRITE(&context,   0, buf, 512, 0x12);

    blkqueue_init_context(&context, bq);
    QUEUE_WRITE(&context, 512, buf, 512, 0x34);
    QUEUE_BARRIER(&context);
    QUEUE_WRITE(&context,   0, buf, 512, 0x56);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x12, 0);
    POP_CHECK_WRITE(bq,   512, buf, 512, 0x34, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x56, 1);

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

    /* Check if we still read the same */
    CHECK_READ(&ctx1, 0, buf, 64, buf2);

    /* Process the AIO requests and check again */
    qemu_aio_flush();
    assert(bq->barriers_submitted == 1);
    assert(bq->in_flight_num == 0);
    CHECK_READ(&ctx1, 0, buf, 64, buf2);

    /* Run the barrier */
    blkqueue_flush(bq);

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
    ret = bdrv_open(bs, "block-queue.img", BDRV_O_RDWR | BDRV_O_CACHE_WB, NULL);
    if (ret < 0) {
        fprintf(stderr, "Couldn't open block-queue.img: %s\n",
            strerror(-ret));
        exit(1);
    }

    run_test(&test_basic, bs);
    run_test(&test_merge, bs);
    run_test(&test_read, bs);
    run_test(&test_read_order, bs);
    run_test(&test_write_order, bs);
    run_test(&test_process_request, bs);

    bdrv_delete(bs);

    return 0;
}
