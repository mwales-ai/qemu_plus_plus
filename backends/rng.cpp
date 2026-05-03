/*
 * QEMU Random Number Generator Backend
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
#include "system/rng.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
}

void rng_backend_request_entropy(RngBackend *s, size_t size,
                                 EntropyReceiveFunc *receive_entropy,
                                 void *opaque)
{
    RngBackendClass *k = RNG_BACKEND_GET_CLASS(s);
    RngRequest *req;

    if (k->request_entropy) {
        req = static_cast<RngRequest *>(g_malloc(sizeof(*req)));

        req->offset = 0;
        req->size = size;
        req->receive_entropy = receive_entropy;
        req->opaque = opaque;
        req->data = static_cast<uint8_t *>(g_malloc(req->size));

        k->request_entropy(s, req);

        QSIMPLEQ_INSERT_TAIL(&s->requests, req, next);
    }
}

static bool rng_backend_prop_get_opened(Object *obj, Error **errp)
{
    RngBackend *s = RNG_BACKEND(obj);

    return s->opened;
}

static void rng_backend_complete(UserCreatable *uc, Error **errp)
{
    RngBackend *s = RNG_BACKEND(uc);
    RngBackendClass *k = RNG_BACKEND_GET_CLASS(s);
    Error *local_err = NULL;

    if (k->opened) {
        k->opened(s, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }

    s->opened = true;
}

static void rng_backend_free_request(RngRequest *req)
{
    g_free(req->data);
    g_free(req);
}

static void rng_backend_free_requests(RngBackend *s)
{
    RngRequest *req, *next;

    QSIMPLEQ_FOREACH_SAFE(req, &s->requests, next, next) {
        rng_backend_free_request(req);
    }

    QSIMPLEQ_INIT(&s->requests);
}

void rng_backend_finalize_request(RngBackend *s, RngRequest *req)
{
    QSIMPLEQ_REMOVE(&s->requests, req, RngRequest, next);
    rng_backend_free_request(req);
}

void RngBackend::init()
{
    QSIMPLEQ_INIT(&requests);
}

void RngBackend::finalize()
{
    rng_backend_free_requests(this);
}

static void rng_backend_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = rng_backend_complete;

    object_class_property_add_bool(oc, "opened",
                                   rng_backend_prop_get_opened,
                                   NULL);
}

static const InterfaceInfo rng_backend_interfaces[] = {
    { TYPE_USER_CREATABLE },
    { }
};

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_ABSTRACT_CI_CS_IFACES(RngBackend, RngBackendClass,
                                            TYPE_RNG_BACKEND, TYPE_OBJECT,
                                            rng_backend_class_init,
                                            rng_backend_interfaces)
