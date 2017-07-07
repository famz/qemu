/*
 * Event loop thread
 *
 * Copyright Red Hat Inc., 2013
 *
 * Authors:
 *  Stefan Hajnoczi   <stefanha@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef IOTHREAD_H
#define IOTHREAD_H

#include "block/aio.h"
#include "qemu/thread.h"

#define TYPE_IOTHREAD "iothread"

typedef struct {
    Object parent_obj;

    QemuThread thread;
    AioContext *ctx;
    QemuMutex init_done_lock;
    QemuCond init_done_cond;    /* is thread initialization done? */
    bool stopping;
    int thread_id;

    AioContextPollParams poll_params;
} IOThread;

#define TYPE_IOTHREAD_GROUP "iothread-group"

typedef struct {
    Object parent_obj;

    int running_threads;
    AioContext *ctx;
    AioContextPollParams poll_params;
    /* Number of iothreads */
    int64_t size;
    IOThread **iothreads;
} IOThreadGroup;

#define IOTHREAD(obj) \
   OBJECT_CHECK(IOThread, obj, TYPE_IOTHREAD)

#define IOTHREAD_GROUP(obj) \
   OBJECT_CHECK(IOThreadGroup, obj, TYPE_IOTHREAD_GROUP)

char *iothread_get_id(IOThread *iothread);
void iothread_start(IOThread *iothread, const char *thread_name, Error **errp);
AioContext *iothread_get_aio_context(IOThread *iothread);
AioContext *iothread_group_get_aio_context(IOThreadGroup *group);
void iothread_stop_all(void);

#ifndef _WIN32
/* Benchmark results from 2016 on NVMe SSD drives show max polling times around
 * 16-32 microseconds yield IOPS improvements for both iodepth=1 and iodepth=32
 * workloads.
 */
#define IOTHREAD_POLL_MAX_NS_DEFAULT 32768ULL
#else
/* Our aio implementation on Windows doesn't support polling, don't enable it
 * by default. */
#define IOTHREAD_POLL_MAX_NS_DEFAULT 0
#endif

#endif /* IOTHREAD_H */
