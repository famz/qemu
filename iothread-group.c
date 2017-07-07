/*
 * Event loop thread group
 *
 * Copyright Red Hat Inc., 2013, 2017
 *
 * Authors:
 *  Stefan Hajnoczi   <stefanha@redhat.com>
 *  Fam Zheng         <famz@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "qemu/module.h"
#include "block/aio.h"
#include "block/block.h"
#include "sysemu/iothread.h"
#include "qmp-commands.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"

static int iothread_group_stop(Object *object, void *opaque)
{
    /* XXX: stop each iothread */
    return 0;
}

static void iothread_group_instance_init(Object *obj)
{
    IOThreadGroup *group = IOTHREAD_GROUP(obj);

    group->poll_params.max_ns = IOTHREAD_POLL_MAX_NS_DEFAULT;
    group->size = 1;
}

static void iothread_group_instance_finalize(Object *obj)
{
    IOThreadGroup *group = IOTHREAD_GROUP(obj);

    iothread_group_stop(obj, NULL);
    if (!group->ctx) {
        return;
    }
    aio_context_unref(group->ctx);
}

static void iothread_group_complete(UserCreatable *obj, Error **errp)
{
    Error *local_err = NULL;
    IOThreadGroup *group = IOTHREAD_GROUP(obj);
    char *name, *thread_name;
    int i;

    group->ctx = aio_context_new(&local_err);
    if (!group->ctx) {
        error_propagate(errp, local_err);
        return;
    }

    aio_context_set_poll_params(group->ctx, group->poll_params,
                                &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        aio_context_unref(group->ctx);
        group->ctx = NULL;
        return;
    }

    /* This assumes we are called from a thread with useful CPU affinity for us
     * to inherit.
     */
    name = object_get_canonical_path_component(OBJECT(obj));
    group->iothreads = g_new0(IOThread *, group->size);
    for (i = 0; i < group->size; ++i) {
        IOThread *iothread = (IOThread *)object_new(TYPE_IOTHREAD);

        /* We've already set up the shared aio context so this should do
         * nothing, but better be consistent. */
        iothread->poll_params = group->poll_params;
        iothread->ctx = group->ctx;

        thread_name = g_strdup_printf("IO %s[%d]", name, i);
        iothread_start(iothread, thread_name, &local_err);
        g_free(thread_name);
        if (local_err) {
            object_unref(OBJECT(iothread));
            break;
        }
        group->iothreads[i] = iothread;
    }
    g_free(name);
    if (local_err) {
        error_propagate(errp, local_err);
    }
}

typedef struct {
    const char *name;
    ptrdiff_t offset; /* field's byte offset in IOThreadGroup struct */
    int64_t max;
} PropInfo;

static PropInfo size_info = {
    "size", offsetof(IOThreadGroup, size), INT_MAX,
};
static PropInfo poll_max_ns_info = {
    "poll-max-ns", offsetof(IOThreadGroup, poll_params.max_ns), INT64_MAX,
};
static PropInfo poll_grow_info = {
    "poll-grow", offsetof(IOThreadGroup, poll_params.grow), INT64_MAX,
};
static PropInfo poll_shrink_info = {
    "poll-shrink", offsetof(IOThreadGroup, poll_params.shrink), INT64_MAX,
};

static void iothread_group_get_prop(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    IOThreadGroup *group = IOTHREAD_GROUP(obj);
    PropInfo *info = opaque;
    int64_t *field = (void *)group + info->offset;

    visit_type_int64(v, name, field, errp);
}

static void iothread_group_set_prop(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    IOThreadGroup *group = IOTHREAD_GROUP(obj);
    PropInfo *info = opaque;
    int64_t *field = (void *)group + info->offset;
    Error *local_err = NULL;
    int64_t value;

    visit_type_int64(v, name, &value, &local_err);
    if (local_err) {
        goto out;
    }

    if (value < 0 || value > info->max) {
        error_setg(&local_err, "%s value must be in range [0, %"PRId64"]",
                   info->name, info->max);
        goto out;
    }

    *field = value;

    if (group->ctx) {
        aio_context_set_poll_params(group->ctx,
                                    group->poll_params,
                                    &local_err);
    }

out:
    error_propagate(errp, local_err);
}

static void iothread_group_class_init(ObjectClass *klass, void *class_data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(klass);
    ucc->complete = iothread_group_complete;

    object_class_property_add(klass, "size", "int",
                              iothread_group_get_prop,
                              iothread_group_set_prop,
                              NULL, &size_info, &error_abort);
    object_class_property_add(klass, "poll-max-ns", "int",
                              iothread_group_get_prop,
                              iothread_group_set_prop,
                              NULL, &poll_max_ns_info, &error_abort);
    object_class_property_add(klass, "poll-grow", "int",
                              iothread_group_get_prop,
                              iothread_group_set_prop,
                              NULL, &poll_grow_info, &error_abort);
    object_class_property_add(klass, "poll-shrink", "int",
                              iothread_group_get_prop,
                              iothread_group_set_prop,
                              NULL, &poll_shrink_info, &error_abort);
}

static const TypeInfo iothread_group_info = {
    .name = TYPE_IOTHREAD_GROUP,
    .parent = TYPE_OBJECT,
    .class_init = iothread_group_class_init,
    .instance_size = sizeof(IOThreadGroup),
    .instance_init = iothread_group_instance_init,
    .instance_finalize = iothread_group_instance_finalize,
    .interfaces = (InterfaceInfo[]) {
        {TYPE_USER_CREATABLE},
        {}
    },
};

static void iothread_group_register_types(void)
{
    type_register_static(&iothread_group_info);
}

type_init(iothread_group_register_types)

AioContext *iothread_group_get_aio_context(IOThreadGroup *group)
{
    return group->ctx;
}
