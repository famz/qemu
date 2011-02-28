/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/* XXX Is there a nicer way to disable glibc's stack check for longjmp? */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
#include <setjmp.h>

#include <stdio.h>
#include <stdlib.h>

#include "qemu-common.h"
#include "coroutine.h"
#include "osdep.h"

int coroutine_release(struct coroutine *co)
{
	return cc_release(&co->cc);
}

static int _coroutine_release(struct continuation *cc)
{
	struct coroutine *co = container_of(cc, struct coroutine, cc);

	if (co->release) {
		int ret = co->release(co);
		if (ret < 0) {
			return ret;
        } else if (ret > 0) {
            goto out;
        }
	}

    qemu_free(co->cc.stack);

out:
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
	co->cc.release = _coroutine_release;
	co->exited = 0;

	return cc_init(&co->cc);
}

int coroutine_init(struct coroutine *co)
{
	if (co->stack_size == 0)
		co->stack_size = 16 << 20;

	co->cc.stack_size = co->stack_size;
	co->cc.stack = qemu_malloc(co->stack_size);

    return coroutine_reinit(co);
}

static __thread struct coroutine leader;
static __thread struct coroutine *current;

struct coroutine *coroutine_self(void)
{
	if (current == NULL)
		current = &leader;
	return current;
}

int coroutine_is_leader(struct coroutine *co)
{
    return co == &leader;
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
        coroutine_release(to);
        to->exited = 1;
        return to->data;
    }

    /* Switch to called coroutine */
    longjmp(to->cc.env, 1);

	return NULL;
}

void *coroutine_yieldto(struct coroutine *to, void *arg)
{
	if (to->caller) {
		fprintf(stderr, "Co-routine is re-entering itself\n");
		abort();
	}
	to->caller = coroutine_self();
	return coroutine_swap(coroutine_self(), to, arg, 1);
}

void *coroutine_yield(void *arg)
{
	struct coroutine *to = coroutine_self()->caller;
	if (!to) {
		fprintf(stderr, "Co-routine is yielding to no one\n");
		abort();
	}
	coroutine_self()->caller = NULL;
	return coroutine_swap(coroutine_self(), to, arg, 0);
}
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
