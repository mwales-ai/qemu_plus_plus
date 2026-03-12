/*
 * QEMU authorization framework base class
 *
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "authz/base.h"
#include "qemu/module.h"
#include "trace.h"

bool qauthz_is_allowed(QAuthZ *authz,
                       const char *identity,
                       Error **errp)
{
    QAuthZClass *cls = QAUTHZ_GET_CLASS(authz);
    bool allowed;

    allowed = cls->is_allowed(authz, identity, errp);
    trace_qauthz_is_allowed(authz, identity, allowed);

    return allowed;
}


bool qauthz_is_allowed_by_id(const char *authzid,
                             const char *identity,
                             Error **errp)
{
    QAuthZ *authz;
    Object *obj;
    Object *container;

    container = object_get_objects_root();
    obj = object_resolve_path_component(container,
                                        authzid);
    if (!obj) {
        error_setg(errp, "Cannot find QAuthZ object ID %s",
                   authzid);
        return false;
    }

    if (!object_dynamic_cast(obj, TYPE_QAUTHZ)) {
        error_setg(errp, "Object '%s' is not a QAuthZ subclass",
                   authzid);
        return false;
    }

    authz = QAUTHZ(obj);

    return qauthz_is_allowed(authz, identity, errp);
}


static const TypeInfo authz_info = {
    .name = TYPE_QAUTHZ,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(QAuthZ),
    .abstract = true,
    .class_size = sizeof(QAuthZClass),
};

static void qauthz_register_types(void)
{
    type_register_static(&authz_info);
}

type_init(qauthz_register_types)

} /* extern "C" */
