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

BlockQueue *blkqueue_create(void);
void blkqueue_destroy(BlockQueue *bq);
int blkqueue_pwrite(BlockQueue *bq, uint64_t offset, void *buf, uint64_t size);

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

void blkqueue_destroy(BlockQueue *bq)
{
    qemu_free(bq);
}

int blkqueue_pwrite(BlockQueue *bq, uint64_t offset, void *buf, uint64_t size)
{
    BlockQueueRequest *req = qemu_malloc(sizeof(*req));
    req->offset = 0;
    req->size = 512;
    req->buf = buf;

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

#ifdef RUN_TESTS
#include <assert.h>

int main(void)
{
    int ret;
    uint8_t buf[512], buf2[512];
    BlockQueue *bq = blkqueue_create();
    BlockQueueRequest *req;

    memset(buf, 0x12, 512);
    ret = blkqueue_pwrite(bq, 0, buf, 512);
    assert(ret == 0);

    memset(buf2, 0x12, 512);
    req = blkqueue_pop(bq);
    assert(req != NULL);
    assert(req->offset == 0);
    assert(req->size == 512);
    assert(!memcmp(req->buf, buf2, 512));

    blkqueue_destroy(bq);

    return 0;
}
#endif
