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

#include <stdint.h>
#include "continuation.h"

/*
 * va_args to makecontext() must be type 'int', so passing
 * the pointer we need may require several int args. This
 * union is a quick hack to let us do that
 */
union cc_arg {
	void *p;
	int i[2];
};

static ucontext_t caller;

static void continuation_trampoline(int i0, int i1)
{
	union cc_arg arg;
	struct continuation *cc;
	arg.i[0] = i0;
	arg.i[1] = i1;
	cc = arg.p;

    /* Initialize longjmp environment and switch back to cc_init */
    if (!setjmp(cc->env)) {
	    swapcontext(&cc->uc, &caller);
    }

	cc->entry(cc);

    longjmp(cc->last_env, 1);
}

int cc_init(struct continuation *cc)
{
	volatile union cc_arg arg;
	arg.p = cc;
	if (getcontext(&cc->uc) == -1)
		return -1;

	cc->uc.uc_stack.ss_sp = cc->stack;
	cc->uc.uc_stack.ss_size = cc->stack_size;
	cc->uc.uc_stack.ss_flags = 0;

	makecontext(&cc->uc, (void *)continuation_trampoline, 2, arg.i[0], arg.i[1]);

    /* Initialize the longjmp environment */
	swapcontext(&caller, &cc->uc);

	return 0;
}

int cc_release(struct continuation *cc)
{
	if (cc->release)
		return cc->release(cc);

	return 0;
}

int cc_swap(struct continuation *from, struct continuation *to, int savectx)
{
    int ret;

    /* Handle termination of called coroutine */
    if (savectx) {
        ret = setjmp(to->last_env);
        if (ret) {
            return 1;
        }
    }

    /* Handle yield of called coroutine */
    ret = setjmp(from->env);
    if (ret) {
        return 0;
    }

    /* Switch to called coroutine */
    longjmp(to->env, 1);
}
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
