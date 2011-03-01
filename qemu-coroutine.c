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

#include "coroutine.h"

#include "trace.h"
#include "qemu-common.h"
#include "qemu-coroutine.h"

/* FIXME This is duplicated in qemu-coroutine-lock.c */
struct Coroutine {
    struct coroutine co;
    QTAILQ_ENTRY(Coroutine) co_queue_next;
    QLIST_ENTRY(Coroutine) pool_next;
};

static QLIST_HEAD(, Coroutine) pool;

static __thread struct coroutine leader;
static __thread struct coroutine *current;

static int qemu_coroutine_done(struct coroutine *co)
{
    Coroutine *coroutine = container_of(co, Coroutine, co);

    trace_qemu_coroutine_done(co);
    QLIST_INSERT_HEAD(&pool, coroutine, pool_next);

    co->caller = NULL;
    return 0;
}

static void coroutine_trampoline(struct continuation *cc)
{
    struct coroutine *co = container_of(cc, struct coroutine, cc);
    co->data = co->entry(co->data);
}

int coroutine_reinit(struct coroutine *co)
{
    co->cc.entry = coroutine_trampoline;
    co->exited = 0;

    return cc_init(&co->cc);
}

int coroutine_init(struct coroutine *co)
{
    co->stack_size = 16 << 20;
    co->cc.stack_size = co->stack_size;
    co->cc.stack = qemu_malloc(co->stack_size);

    return coroutine_reinit(co);
}

struct coroutine *coroutine_self(void)
{
    if (current == NULL) {
        current = &leader;
    }

    return current;
}

void *coroutine_swap(struct coroutine *from, struct coroutine *to, void *arg, int savectx)
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
        to->exited = 1;
        return to->data;
    }

    /* Switch to called coroutine */
    longjmp(to->cc.env, 1);

	return NULL;
}


void *qemu_coroutine_enter(Coroutine *coroutine, void *opaque)
{
    trace_qemu_coroutine_enter(qemu_coroutine_self(), coroutine, opaque);

	if (coroutine->co.caller) {
		fprintf(stderr, "Co-routine re-entered recursively\n");
		abort();
	}

	coroutine->co.caller = coroutine_self();
	return coroutine_swap(coroutine_self(), &coroutine->co, opaque, 1);
}


void * coroutine_fn qemu_coroutine_yield(void *opaque)
{
    Coroutine *self = qemu_coroutine_self();
	struct coroutine *to = self->co.caller;

    trace_qemu_coroutine_yield(self, self->co.caller, opaque);

	if (!to) {
		fprintf(stderr, "Co-routine is yielding to no one\n");
		abort();
	}

	coroutine_self()->caller = NULL;
	return coroutine_swap(coroutine_self(), to, opaque, 0);
}

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *coroutine;

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

Coroutine * coroutine_fn qemu_coroutine_self(void)
{
    return (Coroutine*)coroutine_self();
}

bool qemu_in_coroutine(void)
{
    return (coroutine_self() != &leader);
}
