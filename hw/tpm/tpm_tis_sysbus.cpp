/*
 * tpm_tis_sysbus.c - QEMU's TPM TIS SYSBUS Device
 *
 * Copyright (C) 2006,2010-2013 IBM Corporation
 *
 * Authors:
 *  Stefan Berger <stefanb@us.ibm.com>
 *  David Safford <safford@us.ibm.com>
 *
 * Xen 4 support: Andrease Niederl <andreas.niederl@iaik.tugraz.at>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * Implementation of the TIS interface according to specs found at
 * http://www.trustedcomputinggroup.org. This implementation currently
 * supports version 1.3, 21 March 2013
 * In the developers menu choose the PC Client section then find the TIS
 * specification.
 *
 * TPM TIS for TPM 2 implementation following TCG PC Client Platform
 * TPM Profile (PTP) Specification, Family 2.0, Revision 00.43
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/acpi/tpm.h"
#include "tpm_prop.h"
#include "hw/sysbus.h"
#include "tpm_tis.h"
#include "qom/object.h"

struct TPMStateSysBus {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    TPMState state; /* not a QOM object */

    void init();
    static void classInit(DeviceClass *dc);
};

OBJECT_DECLARE_SIMPLE_TYPE(TPMStateSysBus, TPM_TIS_SYSBUS)

static int tpm_tis_pre_save_sysbus(void *opaque)
{
    TPMStateSysBus *sbdev = static_cast<TPMStateSysBus *>(opaque);

    return tpm_tis_pre_save(&sbdev->state);
}

static const VMStateField vmstate_tpm_tis_sysbus_fields[] = {
    VMSTATE_BUFFER(state.buffer, TPMStateSysBus),
    VMSTATE_UINT16(state.rw_offset, TPMStateSysBus),
    VMSTATE_UINT8(state.active_locty, TPMStateSysBus),
    VMSTATE_UINT8(state.aborting_locty, TPMStateSysBus),
    VMSTATE_UINT8(state.next_locty, TPMStateSysBus),

    {
        .name         = stringify(state.loc),
        .offset       = vmstate_offset_array(TPMStateSysBus, state.loc, TPMLocality, TPM_TIS_NUM_LOCALITIES),
        .size         = sizeof(TPMLocality),
        .num          = TPM_TIS_NUM_LOCALITIES,
        .flags        = static_cast<VMStateFlags>(VMS_STRUCT|VMS_ARRAY),
        .vmsd         = &vmstate_locty,
        .version_id   = 0,
    },

    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_tpm_tis_sysbus = {
    .name = "tpm-tis",
    .version_id = 0,
    .pre_save  = tpm_tis_pre_save_sysbus,
    .fields = vmstate_tpm_tis_sysbus_fields,
};

static void tpm_tis_sysbus_request_completed(TPMIf *ti, int ret)
{
    TPMStateSysBus *sbdev = TPM_TIS_SYSBUS(ti);
    TPMState *s = &sbdev->state;

    tpm_tis_request_completed(s, ret);
}

static enum TPMVersion tpm_tis_sysbus_get_tpm_version(TPMIf *ti)
{
    TPMStateSysBus *sbdev = TPM_TIS_SYSBUS(ti);
    TPMState *s = &sbdev->state;

    return tpm_tis_get_tpm_version(s);
}

static void tpm_tis_sysbus_reset(DeviceState *dev)
{
    TPMStateSysBus *sbdev = TPM_TIS_SYSBUS(dev);
    TPMState *s = &sbdev->state;

    return tpm_tis_reset(s);
}

static const Property tpm_tis_sysbus_properties[] = {
    DEFINE_PROP_UINT32("irq", TPMStateSysBus, state.irq_num, TPM_TIS_IRQ),
    DEFINE_PROP_TPMBE("tpmdev", TPMStateSysBus, state.be_driver),
};

void TPMStateSysBus::init()
{
    TPMState *s = &this->state;

    memory_region_init_io(&s->mmio, OBJECT(this), &tpm_tis_memory_ops,
                          s, "tpm-tis-mmio",
                          TPM_TIS_NUM_LOCALITIES << TPM_TIS_LOCALITY_SHIFT);

    sysbus_init_mmio(SYS_BUS_DEVICE(this), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(this), &s->irq);
}

static void tpm_tis_sysbus_realizefn(DeviceState *dev, Error **errp)
{
    TPMStateSysBus *sbdev = TPM_TIS_SYSBUS(dev);
    TPMState *s = &sbdev->state;

    if (!tpm_find()) {
        error_setg(errp, "at most one TPM device is permitted");
        return;
    }

    if (!s->be_driver) {
        error_setg(errp, "'tpmdev' property is required");
        return;
    }
}

void TPMStateSysBus::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    TPMIfClass *tc = TPM_IF_CLASS(klass);

    device_class_set_props(dc, tpm_tis_sysbus_properties);
    dc->vmsd  = &vmstate_tpm_tis_sysbus;
    tc->model = TPM_MODEL_TPM_TIS;
    dc->realize = tpm_tis_sysbus_realizefn;
    device_class_set_legacy_reset(dc, tpm_tis_sysbus_reset);
    tc->request_completed = tpm_tis_sysbus_request_completed;
    tc->get_version = tpm_tis_sysbus_get_tpm_version;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const InterfaceInfo tpm_tis_sysbus_interfaces[] = {
    { TYPE_TPM_IF },
    { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(TPMStateSysBus, TYPE_TPM_TIS_SYSBUS,
                             TYPE_DYNAMIC_SYS_BUS_DEVICE, tpm_tis_sysbus_interfaces)
