/*
 * QEMU 16550A UART emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2008 Citrix Systems, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* see docs/specs/pci-serial.rst */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_PCI_SERIAL "pci-serial"

struct PCISerialState {
    PCIDevice dev;
    SerialState state;

    void realize(Error **errp)
    {
        SerialState *s = &state;

        if (!qdev_realize(DEVICE(s), NULL, errp)) {
            return;
        }

        dev.config[PCI_CLASS_PROG] = 2; /* 16550 compatible */
        dev.config[PCI_INTERRUPT_PIN] = 1;
        s->irq = pci_allocate_irq(&dev);

        memory_region_init_io(&s->io, OBJECT(this), &serial_io_ops, s, "serial", 8);
        pci_register_bar(&dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    }

    void exitDevice()
    {
        SerialState *s = &state;

        qdev_unrealize(DEVICE(s));
        qemu_free_irq(s->irq);
    }

    static void pciRealize(PCIDevice *dev, Error **errp);
    static void pciExit(PCIDevice *dev);
    static void instanceInit(Object *o);
    static void classInit(ObjectClass *klass, const void *data);

    static const VMStateDescription vmstate_pci_serial;
};

OBJECT_DECLARE_SIMPLE_TYPE(PCISerialState, PCI_SERIAL)

void PCISerialState::pciRealize(PCIDevice *dev, Error **errp)
{
    PCISerialState *pci = DO_UPCAST(PCISerialState, dev, dev);
    pci->realize(errp);
}

void PCISerialState::pciExit(PCIDevice *dev)
{
    PCISerialState *pci = DO_UPCAST(PCISerialState, dev, dev);
    pci->exitDevice();
}

void PCISerialState::instanceInit(Object *o)
{
    PCISerialState *ps = PCI_SERIAL(o);

    object_initialize_child(o, "serial", &ps->state, TYPE_SERIAL);

    qdev_alias_all_properties(DEVICE(&ps->state), o);
}

void PCISerialState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = pciRealize;
    pc->exit = pciExit;
    pc->vendor_id = PCI_VENDOR_ID_REDHAT;
    pc->device_id = PCI_DEVICE_ID_REDHAT_SERIAL;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_COMMUNICATION_SERIAL;
    dc->vmsd = &vmstate_pci_serial;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

const VMStateDescription PCISerialState::vmstate_pci_serial = {
    .name = "pci-serial",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PCISerialState),
        VMSTATE_STRUCT(state, PCISerialState, 0, vmstate_serial, SerialState),
        VMSTATE_END_OF_LIST()
    }
};

static const TypeInfo serial_pci_info = {
    .name          = TYPE_PCI_SERIAL,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCISerialState),
    .instance_init = PCISerialState::instanceInit,
    .class_init    = PCISerialState::classInit,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void serial_pci_register_types(void)
{
    type_register_static(&serial_pci_info);
}

type_init(serial_pci_register_types)
