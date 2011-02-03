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
    QTAILQ_ENTRY(Coroutine) mutex_queue_next;
};

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *coroutine = qemu_mallocz(sizeof(*coroutine));

    coroutine->co.entry = entry;
    coroutine_init(&coroutine->co);
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

void qemu_co_mutex_init(CoMutex *mutex)
{
    memset(mutex, 0, sizeof(*mutex));
    QTAILQ_INIT(&mutex->queue);
}

void qemu_co_mutex_lock(CoMutex *mutex)
{
    if (mutex->locked) {
        Coroutine *self = qemu_coroutine_self();
        QTAILQ_INSERT_TAIL(&mutex->queue, self, mutex_queue_next);
        qemu_coroutine_yield(NULL);
        assert(qemu_in_coroutine());
        assert(mutex->locked == false);
    }

    mutex->locked = true;
}

void qemu_co_mutex_unlock(CoMutex *mutex)
{
    Coroutine* next;

    assert(mutex->locked == true);
    assert(qemu_in_coroutine());

    mutex->locked = false;
    next = QTAILQ_FIRST(&mutex->queue);
    if (next) {
        QTAILQ_REMOVE(&mutex->queue, next, mutex_queue_next);
        qemu_coroutine_enter(next, NULL);
        assert(qemu_in_coroutine());
    }
}
