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

Object *object_property_add_new_container(Object *obj, const char *name)
{
    Object *child = object_new(TYPE_CONTAINER);

    object_property_add_child(obj, name, child);
    object_unref(child);

    return child;
}

} /* extern "C" */

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT(Object, TYPE_CONTAINER, TYPE_OBJECT)
