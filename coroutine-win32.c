/*
 * Win32 coroutine initialization code
 *
 * Copyright (c) 2011 Kevin Wolf <kwolf@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "continuation.h"

static void __attribute__((used)) trampoline(struct continuation *cc)
{
    if (!setjmp(cc->env)) {
        return;
    }

    while (true) {
        cc->entry(cc);
        if (!setjmp(cc->env)) {
            longjmp(*cc->last_env, 2);
        }
    }
}

int cc_init(struct continuation *cc)
{
    /* FIXME This belongs in common code */
    if (cc->initialized) {
        return 0;
    } else {
        cc->initialized = true;
    }

#ifdef __i386__
    asm volatile(
        "mov %%esp, %%ebx;"
        "mov %0, %%esp;"
        "pushl %1;"
        "call _trampoline;"
        "mov %%ebx, %%esp;"
        : : "r" (cc->stack + cc->stack_size), "r" (cc) : "ebx"
    );
#else
    #error This host architecture is not supported for win32
#endif

    return 0;
}
