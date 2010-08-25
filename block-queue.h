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

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include "qemu-common.h"

typedef struct BlockQueue BlockQueue;

typedef struct BlockQueueContext {
    BlockQueue* bq;
    unsigned    section;
} BlockQueueContext;

BlockQueue *blkqueue_create(BlockDriverState *bs);
void blkqueue_init_context(BlockQueueContext* context, BlockQueue *bq);
void blkqueue_destroy(BlockQueue *bq);
int blkqueue_pread(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size);
int blkqueue_pwrite(BlockQueueContext *context, uint64_t offset, void *buf,
    uint64_t size);
int blkqueue_barrier(BlockQueueContext *context);
void blkqueue_flush(BlockQueue *bq);

#endif
