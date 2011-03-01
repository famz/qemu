/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "qemu-common.h"
#include "qemu-coroutine.h"
#include "qemu-coroutine-int.h"

static QLIST_HEAD(, Coroutine) pool;

static __thread Coroutine leader;
static __thread Coroutine *current;

static int qemu_coroutine_done(Coroutine *coroutine)
{
    trace_qemu_coroutine_done(coroutine);
    QLIST_INSERT_HEAD(&pool, coroutine, pool_next);
    coroutine->caller = NULL;
    return 0;
}

static void coroutine_trampoline(struct continuation *cc)
{
    Coroutine *co = container_of(cc, Coroutine, cc);
    co->data = co->entry(co->data);
}

static int coroutine_reinit(Coroutine *co)
{
    co->cc.entry = coroutine_trampoline;

    /* FIXME This belongs in common code */
    if (co->initialized) {
        return 0;
    } else {
        co->initialized = true;
    }

    return cc_init(co);
}

static int coroutine_init(Coroutine *co)
{
    co->stack_size = 16 << 20;
    co->cc.stack_size = co->stack_size;
    co->cc.stack = qemu_malloc(co->stack_size);

    return coroutine_reinit(co);
}

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *coroutine;

    coroutine = QLIST_FIRST(&pool);

    if (coroutine) {
        QLIST_REMOVE(coroutine, pool_next);
        coroutine_reinit(coroutine);
    } else {
        coroutine = qemu_mallocz(sizeof(*coroutine));
        coroutine_init(coroutine);
    }

    coroutine->entry = entry;

    return coroutine;
}

Coroutine * coroutine_fn qemu_coroutine_self(void)
{
    if (current == NULL) {
        current = &leader;
    }

    return current;
}

bool qemu_in_coroutine(void)
{
    return (qemu_coroutine_self() != &leader);
}

static void *coroutine_swap(Coroutine *from, Coroutine *to, void *arg,
    bool savectx)
{
	int ret;
	to->data = arg;
	current = to;

    /* Handle termination of called coroutine */
    if (savectx) {
        to->cc.last_env = &from->cc.env;
    }

    /* Handle yield of called coroutine */
    ret = setjmp(from->cc.env);
    if (ret == 1) {
		return from->data;
    } else if (ret == 2) {
        current = current->caller;
        qemu_coroutine_done(to);
        return to->data;
    }

    /* Switch to called coroutine */
    longjmp(to->cc.env, 1);

	return NULL;
}


void *qemu_coroutine_enter(Coroutine *coroutine, void *opaque)
{
    Coroutine *self = qemu_coroutine_self();

    trace_qemu_coroutine_enter(qemu_coroutine_self(), coroutine, opaque);

	if (coroutine->caller) {
		fprintf(stderr, "Co-routine re-entered recursively\n");
		abort();
	}

	coroutine->caller = self;
	return coroutine_swap(self, coroutine, opaque, true);
}


void * coroutine_fn qemu_coroutine_yield(void *opaque)
{
    Coroutine *self = qemu_coroutine_self();
	Coroutine *to = self->caller;

    trace_qemu_coroutine_yield(self, self->caller, opaque);

	if (!to) {
		fprintf(stderr, "Co-routine is yielding to no one\n");
		abort();
	}

	self->caller = NULL;
	return coroutine_swap(self, to, opaque, false);
}

