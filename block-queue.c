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
int blkqueue_barrier(BlockQueueContext *context);

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
    QTAILQ_HEAD(, BlockQueueRequest) queue;
    QSIMPLEQ_HEAD(, BlockQueueRequest) sections;
};


BlockQueue *blkqueue_create(void)
{
    BlockQueue *bq = qemu_mallocz(sizeof(BlockQueue));
    QTAILQ_INIT(&bq->queue);
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
    assert(QTAILQ_FIRST(&bq->queue) == NULL);
    assert(QSIMPLEQ_FIRST(&bq->sections) == NULL);
    qemu_free(bq);
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
    QSIMPLEQ_FOREACH(section_req, &bq->sections, link_section) {
        if (section_req->section >= req->section) {
            req->section = section_req->section;
            context->section = section_req->section;
            QTAILQ_INSERT_BEFORE(section_req, req, link);
            return 0;
        }
    }

    /* If there was no barrier, just put it at the end. */
    QTAILQ_INSERT_TAIL(&bq->queue, req, link);

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
    QSIMPLEQ_FOREACH(section_req, &bq->sections, link_section) {
        if (section_req->section >= req->section) {
            req->section = section_req->section;
            context->section = section_req->section + 1;
            qemu_free(req);
            return 0;
        }
    }

    /*
     * If there was no barrier for the same section, check if the last request
     * in the queue is a barrier. If not, insert a new one at the end.
     */
    section_req = QSIMPLEQ_LAST(&bq->sections, BlockQueueRequest, link_section);
    if (!section_req || section_req->type != REQ_TYPE_BARRIER) {
        QTAILQ_INSERT_TAIL(&bq->queue, req, link);
        QSIMPLEQ_INSERT_TAIL(&bq->sections, req, link_section);
        context->section++;
    } else {
        qemu_free(req);
    }

    return 0;
}

static BlockQueueRequest *blkqueue_pop(BlockQueue *bq)
{
    BlockQueueRequest *req;

    req = QTAILQ_FIRST(&bq->queue);

    QTAILQ_REMOVE(&bq->queue, req, link);
    if (req->type == REQ_TYPE_BARRIER) {
        assert(QSIMPLEQ_FIRST(&bq->sections) == req);
        QSIMPLEQ_REMOVE_HEAD(&bq->sections, link_section);
    }

    return req;
}

static void blkqueue_free_request(BlockQueueRequest *req)
{
    qemu_free(req->buf);
    qemu_free(req);
}

#ifdef RUN_TESTS
#include <assert.h>

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

static void test_basic(void)
{
    uint8_t buf[512];
    BlockQueue *bq;
    BlockQueueContext context;

    bq = blkqueue_create();
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

static void test_merge(void)
{
    uint8_t buf[512];
    BlockQueue *bq;
    BlockQueueContext ctx1, ctx2;

    bq = blkqueue_create();
    blkqueue_init_context(&ctx1, bq);
    blkqueue_init_context(&ctx2, bq);

    /* Queue requests */
    QUEUE_WRITE(&ctx1,    0, buf, 512, 0x12);
    QUEUE_BARRIER(&ctx1);
    QUEUE_WRITE(&ctx2,  512, buf,  42, 0x34);
    QUEUE_WRITE(&ctx1, 1024, buf, 512, 0x12);
    QUEUE_BARRIER(&ctx2);
    QUEUE_BARRIER(&ctx2);
    QUEUE_WRITE(&ctx2, 1512, buf,  42, 0x34);

    /* Verify queue contents */
    POP_CHECK_WRITE(bq,     0, buf, 512, 0x12, 0);
    POP_CHECK_WRITE(bq,   512, buf,  42, 0x34, 0);
    POP_CHECK_BARRIER(bq, 0);
    POP_CHECK_WRITE(bq,  1024, buf, 512, 0x12, 1);
    POP_CHECK_WRITE(bq,  1512, buf,  42, 0x34, 1);

    blkqueue_destroy(bq);
}

int main(void)
{
    test_basic();
    test_merge();

    /* TODO
     * ctx1:  20 20 | 21 21 21 | 22
     * ctx2:   1  | 21 21 |  22 22 22
     */

    return 0;
}
#endif
