/*
 * QEMU Host Memory Backend
 *
 * Copyright (C) 2013 Red Hat Inc
 *
 * Authors:
 *   Igor Mammedov <imammedo@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "sysemu/hostmem.h"
#include "qom/object_interfaces.h"

#define TYPE_MEMORY_BACKEND_RAM "memory-ram"


static void
ram_backend_memory_alloc(HostMemoryBackend *backend, Error **errp)
{
    if (!backend->size) {
        error_setg(errp, "can't create backend with size 0");
        return;
    }

    memory_region_init_ram(&backend->mr, OBJECT(backend),
                           object_get_canonical_path(OBJECT(backend)),
                           backend->size);
}

static void
ram_backend_class_init(ObjectClass *oc, void *data)
{
    HostMemoryBackendClass *bc = MEMORY_BACKEND_CLASS(oc);

    bc->alloc = ram_backend_memory_alloc;
}

static const TypeInfo ram_backend_info = {
    .name = TYPE_MEMORY_BACKEND_RAM,
    .parent = TYPE_MEMORY_BACKEND,
    .class_init = ram_backend_class_init,
};

static void register_types(void)
{
    type_register_static(&ram_backend_info);
}

type_init(register_types);
