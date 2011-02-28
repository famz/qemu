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

#ifndef _CONTINUATION_H_
#define _CONTINUATION_H_

#ifndef _WIN32
#include <ucontext.h>
#endif
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>

struct continuation
{
	char *stack;
	size_t stack_size;
	void (*entry)(struct continuation *cc);

	/* private */
#ifndef _WIN32
	ucontext_t uc;
#endif
    jmp_buf env;
    jmp_buf *last_env;
    bool initialized;
};

int cc_init(struct continuation *cc);

#define offset_of(type, member) ((unsigned long)(&((type *)0)->member))
#ifndef container_of
#define container_of(obj, type, member) \
        (type *)(((char *)obj) - offset_of(type, member))
#endif

#endif
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
