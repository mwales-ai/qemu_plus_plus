/*
 * QEMU Macintosh Nubus
 *
 * Copyright (c) 2013-2018 Laurent Vivier <laurent@vivier.eu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/nubus/mac-nubus-bridge.h"


void MacNubusBridge::init()
{
    NubusBridge *nb = NUBUS_BRIDGE(this);
    NubusBus *bus = &nb->bus;

    /* Macintosh only has slots 0x9 to 0xe available */
    bus->slot_available_mask = MAKE_64BIT_MASK(MAC_NUBUS_FIRST_SLOT,
                                               MAC_NUBUS_SLOT_NB);

    /* Aliases for slots 0x9 to 0xe */
    memory_region_init_alias(&super_slot_alias, OBJECT(this), "super-slot-alias",
                             &bus->nubus_mr,
                             MAC_NUBUS_FIRST_SLOT * NUBUS_SUPER_SLOT_SIZE,
                             MAC_NUBUS_SLOT_NB * NUBUS_SUPER_SLOT_SIZE);

    memory_region_init_alias(&slot_alias, OBJECT(this), "slot-alias",
                             &bus->nubus_mr,
                             NUBUS_SLOT_BASE +
                             MAC_NUBUS_FIRST_SLOT * NUBUS_SLOT_SIZE,
                             MAC_NUBUS_SLOT_NB * NUBUS_SLOT_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(this), &super_slot_alias);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &slot_alias);
}

void MacNubusBridge::classInit(DeviceClass *dc)
{
    dc->desc = "Nubus bridge";
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(MacNubusBridge, TYPE_MAC_NUBUS_BRIDGE, TYPE_NUBUS_BRIDGE)
