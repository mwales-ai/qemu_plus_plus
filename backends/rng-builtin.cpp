/*
 * QEMU Builtin Random Number Generator Backend
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
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "qom/object.h"
#include "system/replay.h"
}

OBJECT_DECLARE_SIMPLE_TYPE(RngBuiltin, RNG_BUILTIN)

struct RngBuiltin {
    RngBackend parent;
    QEMUBH *bh;

    void init();
    void finalize();
};

static void rng_builtin_receive_entropy_bh(void *opaque)
{
    RngBuiltin *s = static_cast<RngBuiltin *>(opaque);

    while (!QSIMPLEQ_EMPTY(&s->parent.requests)) {
        RngRequest *req = QSIMPLEQ_FIRST(&s->parent.requests);

        qemu_guest_getrandom_nofail(req->data, req->size);

        req->receive_entropy(req->opaque, req->data, req->size);

        rng_backend_finalize_request(&s->parent, req);
    }
}

static void rng_builtin_request_entropy(RngBackend *b, RngRequest *req)
{
    RngBuiltin *s = RNG_BUILTIN(b);

    replay_bh_schedule_event(s->bh);
}

void RngBuiltin::init()
{
    bh = qemu_bh_new(rng_builtin_receive_entropy_bh, this);
}

void RngBuiltin::finalize()
{
    qemu_bh_delete(bh);
}

static void rng_builtin_class_init(ObjectClass *klass, const void *data)
{
    RngBackendClass *rbc = RNG_BACKEND_CLASS(klass);

    rbc->request_entropy = rng_builtin_request_entropy;
}

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_CI(RngBuiltin, TYPE_RNG_BUILTIN, TYPE_RNG_BACKEND,
                         rng_builtin_class_init)
