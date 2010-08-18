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

#include "qemu-common.h"
#include "qemu-queue.h"

typedef struct BlockQueue BlockQueue;

typedef struct BlockQueueContext {
    BlockQueue* bq;
    unsigned    section;
} BlockQueueContext;

BlockQueue *blkqueue_create(void);
void blkqueue_init_context(BlockQueueContext* context, BlockQueue *bq);
void blkqueue_destroy(BlockQueue *bq);
int blkqueue_pwrite(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size);

typedef struct BlockQueueRequest {
    uint64_t    offset;
    void*       buf;
    uint64_t    size;

    QSIMPLEQ_ENTRY(BlockQueueRequest) link;
} BlockQueueRequest;

struct BlockQueue {
    QSIMPLEQ_HEAD(, BlockQueueRequest) queue;
};


BlockQueue *blkqueue_create(void)
{
    BlockQueue *bq = qemu_mallocz(sizeof(BlockQueue));
    QSIMPLEQ_INIT(&bq->queue);

    return bq;
}

void blkqueue_init_context(BlockQueueContext* context, BlockQueue *bq)
{
    context->bq = bq;
    context->section = 0;
}

void blkqueue_destroy(BlockQueue *bq)
{
    qemu_free(bq);
}

int blkqueue_pwrite(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size)
{
    BlockQueue *bq = context->bq;
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
    req->offset = offset;
    req->size = size;
    req->buf = qemu_malloc(size);
    memcpy(req->buf, buf, size);

    QSIMPLEQ_INSERT_TAIL(&bq->queue, req, link);

    return 0;
}

static BlockQueueRequest *blkqueue_pop(BlockQueue *bq)
{
    BlockQueueRequest *req;

    req = QSIMPLEQ_FIRST(&bq->queue);
    QSIMPLEQ_REMOVE_HEAD(&bq->queue, link);

    return req;
}

static void blkqueue_free_request(BlockQueueRequest *req)
{
    qemu_free(req->buf);
    qemu_free(req);
}

#ifdef RUN_TESTS
#include <assert.h>

static void test_basic(void)
{
    int ret;
    uint8_t buf[512], buf2[512];
    BlockQueue *bq;
    BlockQueueContext context;
    BlockQueueRequest *req;

    bq = blkqueue_create();
    blkqueue_init_context(&context, bq);

    memset(buf, 0x12, 512);
    ret = blkqueue_pwrite(&context, 0, buf, 512);
    assert(ret == 0);

    memset(buf, 0x34, 512);
    ret = blkqueue_pwrite(&context, 512, buf, 42);
    assert(ret == 0);

    memset(buf, 0, 512);
    memset(buf2, 0x12, 512);
    req = blkqueue_pop(bq);
    assert(req != NULL);
    assert(req->offset == 0);
    assert(req->size == 512);
    assert(!memcmp(req->buf, buf2, 512));
    blkqueue_free_request(req);

    memset(buf2, 0x34, 512);
    req = blkqueue_pop(bq);
    assert(req != NULL);
    assert(req->offset == 512);
    assert(req->size == 42);
    assert(!memcmp(req->buf, buf2, 42));
    blkqueue_free_request(req);

    blkqueue_destroy(bq);
}

int main(void)
{
    test_basic();
    return 0;
}
#endif
