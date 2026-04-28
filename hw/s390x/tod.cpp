/*
 * TOD (Time Of Day) clock
 *
 * Copyright 2018 Red Hat, Inc.
 * Author(s): David Hildenbrand <david@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/s390x/tod.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "system/kvm.h"
#include "system/tcg.h"
#include "system/qtest.h"
#include "migration/qemu-file-types.h"
#include "migration/register.h"
#include "qom/cpp/object.h"

void s390_init_tod(void)
{
    Object *obj;

    if (kvm_enabled()) {
        obj = object_new(TYPE_KVM_S390_TOD);
    } else if (tcg_enabled()) {
        obj = object_new(TYPE_QEMU_S390_TOD);
    } else if (qtest_enabled()) {
        return;
    } else {
        error_report("current accelerator not handled in s390_init_tod!");
        abort();
    }
    object_property_add_child(qdev_get_machine(), TYPE_S390_TOD, obj);
    object_unref(obj);

    qdev_realize(DEVICE(obj), NULL, &error_fatal);
}

S390TODState *s390_get_todstate(void)
{
    static S390TODState *ts;

    if (!ts) {
        ts = S390_TOD(object_resolve_path_type("", TYPE_S390_TOD, NULL));
    }

    return ts;
}

#define S390_TOD_CLOCK_VALUE_MISSING    0x00
#define S390_TOD_CLOCK_VALUE_PRESENT    0x01

static void s390_tod_save(QEMUFile *f, void *opaque)
{
    S390TODState *td = static_cast<S390TODState *>(opaque);
    S390TODClass *tdc = S390_TOD_GET_CLASS(td);
    Error *err = NULL;
    S390TOD tod;

    tdc->get(td, &tod, &err);
    if (err) {
        warn_report_err(err);
        error_printf("Guest clock will not be migrated "
                     "which could cause the guest to hang.");
        qemu_put_byte(f, S390_TOD_CLOCK_VALUE_MISSING);
        return;
    }

    qemu_put_byte(f, S390_TOD_CLOCK_VALUE_PRESENT);
    qemu_put_byte(f, tod.high);
    qemu_put_be64(f, tod.low);
}

static int s390_tod_load(QEMUFile *f, void *opaque, int version_id)
{
    S390TODState *td = static_cast<S390TODState *>(opaque);
    S390TODClass *tdc = S390_TOD_GET_CLASS(td);
    Error *err = NULL;
    S390TOD tod;

    if (qemu_get_byte(f) == S390_TOD_CLOCK_VALUE_MISSING) {
        warn_report("Guest clock was not migrated. This could "
                    "cause the guest to hang.");
        return 0;
    }

    tod.high = qemu_get_byte(f);
    tod.low = qemu_get_be64(f);

    tdc->set(td, &tod, &err);
    if (err) {
        error_report_err(err);
        return -1;
    }
    return 0;
}

static SaveVMHandlers savevm_tod = {
    .save_state = s390_tod_save,
    .load_state = s390_tod_load,
};

static void s390_tod_realize(DeviceState *dev, Error **errp)
{
    S390TODState *td = S390_TOD(dev);

    /* Legacy migration interface */
    register_savevm_live("todclock", 0, 1, &savevm_tod, td);
}

void S390TODState::classInit(DeviceClass *dc)
{
    dc->desc = "TOD (Time Of Day) Clock";
    dc->realize = s390_tod_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    /* We only have one TOD clock in the system attached to the machine */
    dc->user_creatable = false;
}

REGISTER_QEMU_DEVICE_ABSTRACT(S390TODState, S390TODClass,
                              TYPE_S390_TOD, TYPE_DEVICE)
