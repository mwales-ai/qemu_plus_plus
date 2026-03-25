/*
 * QEMU IDE Emulation: ISA Bus support.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
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

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/dma.h"

#include "hw/ide/isa.h"
#include "qom/object.h"
#include "ide-internal.h"

/***********************************************************/
/* ISA IDE definitions */

struct ISAIDEState {
    ISADevice parent_obj;

    IDEBus    bus;
    uint32_t  iobase;
    uint32_t  iobase2;
    uint32_t  irqnum;

    void deviceReset()
    {
        ide_bus_reset(&bus);
    }

    void realize(Error **errp)
    {
        ISADevice *isadev = ISA_DEVICE(DEVICE(this));

        ide_bus_init(&bus, sizeof(bus), DEVICE(this), 0, 2);
        ide_init_ioport(&bus, isadev, iobase, iobase2);
        ide_bus_init_output_irq(&bus, isa_get_irq(isadev, irqnum));
        vmstate_register_any(VMSTATE_IF(DEVICE(this)), &vmstate_ide_isa, this);
        ide_bus_register_restart_cb(&bus);
    }

    static void deviceReset_static(DeviceState *d)
    {
        ISAIDEState *s = ISA_IDE(d);
        s->deviceReset();
    }

    static void deviceRealize(DeviceState *dev, Error **errp)
    {
        ISAIDEState *s = ISA_IDE(dev);
        s->realize(errp);
    }

    static void classInit(ObjectClass *klass, const void *data);

    static const VMStateDescription vmstate_ide_isa;
    static const Property isa_ide_properties[];
};

static const VMStateField vmstate_ide_isa_fields[] = {
    VMSTATE_IDE_BUS(bus, ISAIDEState),
    VMSTATE_IDE_DRIVES(bus.ifs, ISAIDEState),
    VMSTATE_END_OF_LIST()
};

const VMStateDescription ISAIDEState::vmstate_ide_isa = {
    .name = "isa-ide",
    .version_id = 3,
    .minimum_version_id = 0,
    .fields = vmstate_ide_isa_fields
};

const Property ISAIDEState::isa_ide_properties[] = {
    DEFINE_PROP_UINT32("iobase",  ISAIDEState, iobase,  0x1f0),
    DEFINE_PROP_UINT32("iobase2", ISAIDEState, iobase2, 0x3f6),
    DEFINE_PROP_UINT32("irq",     ISAIDEState, irqnum,  14),
};

void ISAIDEState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deviceRealize;
    dc->fw_name = "ide";
    device_class_set_legacy_reset(dc, deviceReset_static);
    device_class_set_props(dc, isa_ide_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

ISADevice *isa_ide_init(ISABus *bus, int iobase, int iobase2, int irqnum,
                        DriveInfo *hd0, DriveInfo *hd1)
{
    DeviceState *dev;
    ISADevice *isadev;
    ISAIDEState *s;

    isadev = isa_new(TYPE_ISA_IDE);
    dev = DEVICE(isadev);
    qdev_prop_set_uint32(dev, "iobase",  iobase);
    qdev_prop_set_uint32(dev, "iobase2", iobase2);
    qdev_prop_set_uint32(dev, "irq",     irqnum);
    isa_realize_and_unref(isadev, bus, &error_fatal);

    s = ISA_IDE(dev);
    if (hd0) {
        ide_bus_create_drive(&s->bus, 0, hd0);
    }
    if (hd1) {
        ide_bus_create_drive(&s->bus, 1, hd1);
    }
    return isadev;
}

static const TypeInfo isa_ide_info = {
    .name          = TYPE_ISA_IDE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ISAIDEState),
    .class_init    = ISAIDEState::classInit,
};

static void isa_ide_register_types(void)
{
    type_register_static(&isa_ide_info);
}

type_init(isa_ide_register_types)
