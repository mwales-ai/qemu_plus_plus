/*
 * Device Container
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "qom/object.h"
#include "qemu/module.h"

static const TypeInfo container_info = {
    .name          = TYPE_CONTAINER,
    .parent        = TYPE_OBJECT,
};

static void container_register_types(void)
{
    type_register_static(&container_info);
}

Object *object_property_add_new_container(Object *obj, const char *name)
{
    Object *child = object_new(TYPE_CONTAINER);

    object_property_add_child(obj, name, child);
    object_unref(child);

    return child;
}

type_init(container_register_types)

} /* extern "C" */
