/*
 * QEMU coroutine implementation
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

#ifndef QEMU_COROUTINE_H
#define QEMU_COROUTINE_H

#include <stdbool.h>

#include "qemu-queue.h"

/**
 * Mark a function that executes in coroutine context
 *
 * Functions that execute in coroutine context cannot be called directly from
 * normal functions.  In the future it would be nice to enable compiler or
 * static checker support for catching such errors.  This annotation might make
 * it possible and in the meantime it serves as documentation.
 *
 * For example:
 *
 *   static void coroutine_fn foo(void) {
 *       ....
 *   }
 */
#define coroutine_fn

typedef struct Coroutine Coroutine;

/**
 * Coroutine entry point
 *
 * When the coroutine is entered for the first time, opaque is passed in as an
 * argument.
 *
 * When this function returns, the coroutine is destroyed automatically and the
 * return value is passed back to the caller who last entered the coroutine.
 */
typedef void * coroutine_fn CoroutineEntry(void *opaque);

/**
 * Create a new coroutine
 *
 * Use qemu_coroutine_enter() to actually transfer control to the coroutine.
 */
Coroutine *qemu_coroutine_create(CoroutineEntry *entry);

/**
 * Transfer control to a coroutine
 *
 * The opaque argument is made available to the coroutine either as the entry
 * function argument if this is the first time a new coroutine is entered, or
 * as the return value from qemu_coroutine_yield().
 *
 * The return value from this function is either an opaque value yielded by the
 * coroutine or the coroutine entry function return value when the coroutine
 * terminates.
 */
void *qemu_coroutine_enter(Coroutine *coroutine, void *opaque);

/**
 * Transfer control back to a coroutine's caller
 *
 * The opaque argument is returned from the calling qemu_coroutine_enter().
 *
 * The return value is the argument passed back in from the next
 * qemu_coroutine_enter().
 */
void * coroutine_fn qemu_coroutine_yield(void *opaque);

/**
 * Get the currently executing coroutine
 */
Coroutine * coroutine_fn qemu_coroutine_self(void);

/**
 * Return whether or not currently inside a coroutine
 */
bool qemu_in_coroutine(void);

/* TODO Documentation */
typedef struct CoQueue {
    QTAILQ_HEAD(, Coroutine) entries;
} CoQueue;

void qemu_co_queue_init(CoQueue *queue);
void qemu_co_queue_wait(CoQueue *queue);
void qemu_co_queue_next(CoQueue *queue);


typedef struct CoMutex {
    bool locked;
    CoQueue queue;
} CoMutex;

void qemu_co_mutex_init(CoMutex *mutex);
void qemu_co_mutex_lock(CoMutex *mutex);
void qemu_co_mutex_unlock(CoMutex *mutex);

#endif /* QEMU_COROUTINE_H */
