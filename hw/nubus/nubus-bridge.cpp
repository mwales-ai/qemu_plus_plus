/*
 * QEMU Nubus
 *
 * Copyright (c) 2013-2018 Laurent Vivier <laurent@vivier.eu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/nubus/nubus.h"


void NubusBridge::init()
{
    NubusBus *nbus = &bus;

    qbus_init(nbus, sizeof(bus), TYPE_NUBUS_BUS, DEVICE(this), NULL);

    qdev_init_gpio_out(DEVICE(this), nbus->irqs, NUBUS_IRQS);
}

static const Property nubus_bridge_properties[] = {
    DEFINE_PROP_UINT16("slot-available-mask", NubusBridge,
                       bus.slot_available_mask, 0xffff),
};

void NubusBridge::classInit(DeviceClass *dc)
{
    dc->fw_name = "nubus";
    device_class_set_props(dc, nubus_bridge_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(NubusBridge, TYPE_NUBUS_BRIDGE, TYPE_SYS_BUS_DEVICE)
