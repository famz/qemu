/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "coroutine.h"

#include "qemu-common.h"
#include "qemu-coroutine.h"

struct Coroutine {
    struct coroutine co;
    QTAILQ_ENTRY(Coroutine) co_queue_next;
    QLIST_ENTRY(Coroutine) pool_next;
};

static QLIST_HEAD(, Coroutine) pool;
static QTAILQ_HEAD(, Coroutine) unlock_bh_queue;
static QEMUBH* unlock_bh;

static void qemu_co_queue_next_bh(void *opaque)
{
    Coroutine* next;

    while ((next = QTAILQ_FIRST(&unlock_bh_queue))) {
        QTAILQ_REMOVE(&unlock_bh_queue, next, co_queue_next);
        qemu_coroutine_enter(next, NULL);
        assert(qemu_in_coroutine());
    }
}

static int qemu_coroutine_done(struct coroutine *co)
{
	Coroutine *coroutine = container_of(co, Coroutine, co);

    QLIST_INSERT_HEAD(&pool, coroutine, pool_next);
    return 1;
}

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *coroutine;

    if (!unlock_bh) {
        unlock_bh = qemu_bh_new(qemu_co_queue_next_bh, NULL);
    }

    coroutine = QLIST_FIRST(&pool);

    if (coroutine) {
        QLIST_REMOVE(coroutine, pool_next);
        coroutine_reinit(&coroutine->co);
    } else {
        coroutine = qemu_mallocz(sizeof(*coroutine));
        coroutine_init(&coroutine->co);
    }

    coroutine->co.entry = entry;
    coroutine->co.release = qemu_coroutine_done;

    return coroutine;
}

void *qemu_coroutine_enter(Coroutine *coroutine, void *opaque)
{
    return coroutine_yieldto(&coroutine->co, opaque);
}

void * coroutine_fn qemu_coroutine_yield(void *opaque)
{
    return coroutine_yield(opaque);
}

Coroutine * coroutine_fn qemu_coroutine_self(void)
{
    return (Coroutine*)coroutine_self();
}

bool qemu_in_coroutine(void)
{
    return !coroutine_is_leader(coroutine_self());
}

void qemu_co_queue_init(CoQueue *queue)
{
    QTAILQ_INIT(&queue->entries);
}

void qemu_co_queue_wait(CoQueue *queue)
{
    Coroutine *self = qemu_coroutine_self();
    QTAILQ_INSERT_TAIL(&queue->entries, self, co_queue_next);
    qemu_coroutine_yield(NULL);
    assert(qemu_in_coroutine());
}

bool qemu_co_queue_next(CoQueue *queue)
{
    Coroutine* next;

    next = QTAILQ_FIRST(&queue->entries);
    if (next) {
        QTAILQ_REMOVE(&queue->entries, next, co_queue_next);
        qemu_coroutine_enter(next, NULL);
        assert(qemu_in_coroutine());
    }

    return (next != NULL);
}

bool qemu_co_queue_empty(CoQueue *queue)
{
    return (QTAILQ_FIRST(&queue->entries) == NULL);
}

void qemu_co_mutex_init(CoMutex *mutex)
{
    memset(mutex, 0, sizeof(*mutex));
    qemu_co_queue_init(&mutex->queue);
}

void qemu_co_mutex_lock(CoMutex *mutex)
{
    if (mutex->locked) {
        qemu_co_queue_wait(&mutex->queue);
        assert(mutex->locked == false);
    }

    mutex->locked = true;
}

void qemu_co_mutex_unlock(CoMutex *mutex)
{
    assert(mutex->locked == true);
    assert(qemu_in_coroutine());

    mutex->locked = false;
    qemu_co_queue_next(&mutex->queue);
}
