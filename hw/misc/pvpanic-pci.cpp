/*
 * QEMU simulated PCI pvpanic device.
 *
 * Copyright (C) 2020 Oracle
 *
 * Authors:
 *     Mihai Carabas <mihai.carabas@oracle.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "system/runstate.h"

#include "hw/nvram/fw_cfg.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/pvpanic.h"
#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "standard-headers/misc/pvpanic.h"

OBJECT_DECLARE_SIMPLE_TYPE(PVPanicPCIState, PVPANIC_PCI_DEVICE)

/*
 * PVPanicPCIState for PCI device
 */
struct PVPanicPCIState {
    PCIDevice dev;
    PVPanicState pvpanic;

    static void classInit(DeviceClass *dc);
};

static const VMStateDescription vmstate_pvpanic_pci = {
    .name = "pvpanic-pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PVPanicPCIState),
        VMSTATE_END_OF_LIST()
    }
};

static void pvpanic_pci_realizefn(PCIDevice *dev, Error **errp)
{
    PVPanicPCIState *s = PVPANIC_PCI_DEVICE(dev);
    PVPanicState *ps = &s->pvpanic;

    pvpanic_setup_io(ps, DEVICE(s), 2);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &ps->mr);
}

static const Property pvpanic_pci_properties[] = {
    DEFINE_PROP_UINT8("events", PVPanicPCIState, pvpanic.events,
                      PVPANIC_EVENTS),
};

void PVPanicPCIState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    device_class_set_props(dc, pvpanic_pci_properties);

    pc->realize = pvpanic_pci_realizefn;
    pc->vendor_id = PCI_VENDOR_ID_REDHAT;
    pc->device_id = PCI_DEVICE_ID_REDHAT_PVPANIC;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_SYSTEM_OTHER;
    dc->vmsd = &vmstate_pvpanic_pci;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const InterfaceInfo pvpanic_pci_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(PVPanicPCIState, TYPE_PVPANIC_PCI_DEVICE,
                             TYPE_PCI_DEVICE, pvpanic_pci_interfaces)
