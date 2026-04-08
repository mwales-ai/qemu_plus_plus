/*
 * debug exit port emulation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/runstate.h"

#define TYPE_ISA_DEBUG_EXIT_DEVICE "isa-debug-exit"

struct ISADebugExitState {
    ISADevice parent_obj;

    uint32_t iobase;
    uint32_t iosize;
    MemoryRegion io;

    /* C++ methods */
    void realize(Error **errp);

    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned width);
    static void classInit(ObjectClass *klass, const void *data);
};

DECLARE_INSTANCE_CHECKER(ISADebugExitState, ISA_DEBUG_EXIT_DEVICE,
                         TYPE_ISA_DEBUG_EXIT_DEVICE)

uint64_t ISADebugExitState::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

void ISADebugExitState::mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned width)
{
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                           (val << 1) | 1);
}

static const MemoryRegionOps debug_exit_ops = {
    .read = ISADebugExitState::mmioRead,
    .write = ISADebugExitState::mmioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
};

void ISADebugExitState::realize(Error **errp)
{
    ISADevice *dev = reinterpret_cast<ISADevice *>(this);

    memory_region_init_io(&io, reinterpret_cast<Object *>(dev), &debug_exit_ops, this,
                          TYPE_ISA_DEBUG_EXIT_DEVICE, iosize);
    memory_region_add_subregion(isa_address_space_io(dev), iobase, &io);
}

static const Property debug_exit_properties[] = {
    DEFINE_PROP_UINT32("iobase", ISADebugExitState, iobase, 0x501),
    DEFINE_PROP_UINT32("iosize", ISADebugExitState, iosize, 0x02),
};

static void debug_exit_realize(DeviceState *d, Error **errp)
{
    ISA_DEBUG_EXIT_DEVICE(d)->realize(errp);
}

void ISADebugExitState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    dc->realize = debug_exit_realize;
    device_class_set_props(dc, debug_exit_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo debug_exit_info = {
    .name          = TYPE_ISA_DEBUG_EXIT_DEVICE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ISADebugExitState),
    .class_init    = ISADebugExitState::classInit,
};

static void debug_exit_register_types(void)
{
    type_register_static(&debug_exit_info);
}

type_init(debug_exit_register_types)
