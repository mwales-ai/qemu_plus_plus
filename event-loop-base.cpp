/*
 * QEMU event-loop base
 *
 * Copyright (C) 2022 Red Hat Inc
 *
 * Authors:
 *  Stefan Hajnoczi <stefanha@redhat.com>
 *  Nicolas Saenz Julienne <nsaenzju@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "qom/object_interfaces.h"
#include "qapi/error.h"
#include "block/thread-pool.h"
#include "system/event-loop-base.h"

typedef struct {
    const char *name;
    ptrdiff_t offset; /* field's byte offset in EventLoopBase struct */
} EventLoopBaseParamInfo;

void EventLoopBase::init()
{
    thread_pool_max = THREAD_POOL_MAX_THREADS_DEFAULT;
}

static EventLoopBaseParamInfo aio_max_batch_info = {
    "aio-max-batch", offsetof(EventLoopBase, aio_max_batch),
};
static EventLoopBaseParamInfo thread_pool_min_info = {
    "thread-pool-min", offsetof(EventLoopBase, thread_pool_min),
};
static EventLoopBaseParamInfo thread_pool_max_info = {
    "thread-pool-max", offsetof(EventLoopBase, thread_pool_max),
};

static void event_loop_base_get_param(Object *obj, Visitor *v,
        const char *name, void *opaque, Error **errp)
{
    EventLoopBase *event_loop_base = EVENT_LOOP_BASE(obj);
    EventLoopBaseParamInfo *info = static_cast<EventLoopBaseParamInfo *>(opaque);
    int64_t *field = (int64_t *)((char *)event_loop_base + info->offset);

    visit_type_int64(v, name, field, errp);
}

static void event_loop_base_set_param(Object *obj, Visitor *v,
        const char *name, void *opaque, Error **errp)
{
    EventLoopBaseClass *bc = EVENT_LOOP_BASE_GET_CLASS(obj);
    EventLoopBase *base = EVENT_LOOP_BASE(obj);
    EventLoopBaseParamInfo *info = static_cast<EventLoopBaseParamInfo *>(opaque);
    int64_t *field = (int64_t *)((char *)base + info->offset);
    int64_t value;

    if (!visit_type_int64(v, name, &value, errp)) {
        return;
    }

    if (value < 0) {
        error_setg(errp, "%s value must be in range [0, %" PRId64 "]",
                   info->name, INT64_MAX);
        return;
    }

    *field = value;

    if (bc->update_params) {
        bc->update_params(base, errp);
    }
}

static void event_loop_base_complete(UserCreatable *uc, Error **errp)
{
    EventLoopBaseClass *bc = EVENT_LOOP_BASE_GET_CLASS(uc);
    EventLoopBase *base = EVENT_LOOP_BASE(uc);

    if (bc->init) {
        bc->init(base, errp);
    }
}

static bool event_loop_base_can_be_deleted(UserCreatable *uc)
{
    EventLoopBaseClass *bc = EVENT_LOOP_BASE_GET_CLASS(uc);
    EventLoopBase *backend = EVENT_LOOP_BASE(uc);

    if (bc->can_be_deleted) {
        return bc->can_be_deleted(backend);
    }

    return true;
}

static void event_loop_base_class_init(ObjectClass *klass,
                                       const void *class_data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(klass);
    ucc->complete = event_loop_base_complete;
    ucc->can_be_deleted = event_loop_base_can_be_deleted;

    object_class_property_add(klass, "aio-max-batch", "int",
                              event_loop_base_get_param,
                              event_loop_base_set_param,
                              NULL, &aio_max_batch_info);
    object_class_property_add(klass, "thread-pool-min", "int",
                              event_loop_base_get_param,
                              event_loop_base_set_param,
                              NULL, &thread_pool_min_info);
    object_class_property_add(klass, "thread-pool-max", "int",
                              event_loop_base_get_param,
                              event_loop_base_set_param,
                              NULL, &thread_pool_max_info);
}

static const InterfaceInfo event_loop_base_interfaces[] = {
    { TYPE_USER_CREATABLE },
    { }
};

} /* extern "C" */

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_ABSTRACT_CI_CS_IFACES(EventLoopBase, EventLoopBaseClass,
                                            TYPE_EVENT_LOOP_BASE, TYPE_OBJECT,
                                            event_loop_base_class_init,
                                            event_loop_base_interfaces)
