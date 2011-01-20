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
