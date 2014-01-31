/*
 * QEMU Module Infrastructure
 *
 * Copyright IBM, Corp. 2009
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_MODULE_H
#define QEMU_MODULE_H

/* Using glue() causes the preprocessor to choke on values of CONFIG_STAMP
 * that start with a digit, because they are split at the first
 * letter.  For example:
 *
 *      config-host.h:33:22: error: invalid suffix "c2a05f88b8817d3a0a961f0d8296751d21e8774" on integer constant
 *      #define CONFIG_STAMP 1c2a05f88b8817d3a0a961f0d8296751d21e8774
 *                           ^
 *      include/qemu/module.h:20:48: note: in expansion of macro 'CONFIG_STAMP'
 *      #define DSO_STAMP_FUN         glue(qemu_stamp_,CONFIG_STAMP)
 *                                                     ^
 */
#define DSO_STAMP_FUN2(x)     qemu_stamp_##x
#define DSO_STAMP_FUN1(x)     DSO_STAMP_FUN2(x)
#define DSO_STAMP_FUN         DSO_STAMP_FUN1(CONFIG_STAMP)
#define DSO_STAMP_FUN_STR     stringify(DSO_STAMP_FUN)

#ifdef BUILD_DSO
void DSO_STAMP_FUN(void);
/* This is a dummy symbol to identify a loaded DSO as a QEMU module, so we can
 * distinguish "version mismatch" from "not a QEMU module", when the stamp
 * check fails during module loading */
void qemu_module_dummy(void);

#define module_init(function, type)                                         \
static void __attribute__((constructor)) do_qemu_init_ ## function(void)    \
{                                                                           \
    register_dso_module_init(function, type);                               \
}
#else
/* This should not be used directly.  Use block_init etc. instead.  */
#define module_init(function, type)                                         \
static void __attribute__((constructor)) do_qemu_init_ ## function(void)    \
{                                                                           \
    register_module_init(function, type);                                   \
}
#endif

typedef enum {
    MODULE_INIT_BLOCK,
    MODULE_INIT_MACHINE,
    MODULE_INIT_QAPI,
    MODULE_INIT_QOM,
    MODULE_INIT_MAX
} module_init_type;

#define block_init(function) module_init(function, MODULE_INIT_BLOCK)
#define machine_init(function) module_init(function, MODULE_INIT_MACHINE)
#define qapi_init(function) module_init(function, MODULE_INIT_QAPI)
#define type_init(function) module_init(function, MODULE_INIT_QOM)

void register_module_init(void (*fn)(void), module_init_type type);
void register_dso_module_init(void (*fn)(void), module_init_type type);

void module_call_init(module_init_type type);

#endif
