/*
 * QEMU simulated pvpanic device (MMIO frontend)
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "hw/qdev-properties.h"
#include "hw/misc/pvpanic.h"
#include "hw/sysbus.h"
#include "qom/cpp/object.h"
#include "standard-headers/misc/pvpanic.h"

OBJECT_DECLARE_SIMPLE_TYPE(PVPanicMMIOState, PVPANIC_MMIO_DEVICE)

#define PVPANIC_MMIO_SIZE 0x2

struct PVPanicMMIOState {
    SysBusDevice parent_obj;

    PVPanicState pvpanic;

    void init();
    static void classInit(DeviceClass *dc);
};

void PVPanicMMIOState::init()
{
    pvpanic_setup_io(&pvpanic, reinterpret_cast<DeviceState *>(this),
                     PVPANIC_MMIO_SIZE);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &pvpanic.mr);
}

static const Property pvpanic_mmio_properties[] = {
    DEFINE_PROP_UINT8("events", PVPanicMMIOState, pvpanic.events,
                      PVPANIC_PANICKED | PVPANIC_CRASH_LOADED),
};

void PVPanicMMIOState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, pvpanic_mmio_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

REGISTER_QEMU_DEVICE(PVPanicMMIOState, TYPE_PVPANIC_MMIO_DEVICE,
                     TYPE_SYS_BUS_DEVICE)
