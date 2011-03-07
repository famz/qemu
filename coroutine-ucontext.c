/*
 * ucontext coroutine initialization code
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2011  Kevin Wolf <kwolf@redhat.com>
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
#include "qemu-coroutine-int.h"

#ifndef _WIN32
#include <ucontext.h>
#endif

static Coroutine* current;
static ucontext_t caller;

static void continuation_trampoline(void)
{
    Coroutine *co = current;

    /* Initialize longjmp environment and switch back to cc_init */
    if (!setjmp(co->env)) {
        return;
    }

    while (true) {
        co->entry(co->data);
        if (!setjmp(co->env)) {
            longjmp(*co->last_env, 2);
        }
    }
}

int qemu_coroutine_init_env(Coroutine *co)
{
    ucontext_t uc;

    /* Create a new ucontext for switching to the coroutine stack and setting
     * up a longjmp environment. */
    if (getcontext(&uc) == -1) {
        return -1;
    }

    uc.uc_link = &caller;
    uc.uc_stack.ss_sp = co->stack;
    uc.uc_stack.ss_size = co->stack_size;
    uc.uc_stack.ss_flags = 0;

    current = co;
    makecontext(&uc, (void *)continuation_trampoline, 0);

    /* Initialize the longjmp environment */
    swapcontext(&caller, &uc);

    return 0;
}
