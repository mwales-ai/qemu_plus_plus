/*
 * LED, Switch and Debug control registers for ARM Integrator Boards
 *
 * This is currently a stub for this functionality but at least
 * ensures something other than unassigned_mem_read() handles access
 * to this area.
 *
 * The real h/w is described at:
 *  https://developer.arm.com/documentation/dui0159/b/peripherals-and-interfaces/debug-leds-and-dip-switch-interface
 *
 * Copyright (c) 2013 Alex Bennée <alex@bennee.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "hw/sysbus.h"
#include "hw/misc/arm_integrator_debug.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "qom/cpp/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(IntegratorDebugState, INTEGRATOR_DEBUG)

struct IntegratorDebugState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    void init();

    static uint64_t mmioRead(void *opaque, hwaddr offset, unsigned size);
    static void mmioWrite(void *opaque, hwaddr offset,
                          uint64_t value, unsigned size);
};

uint64_t IntegratorDebugState::mmioRead(void *opaque, hwaddr offset,
                                        unsigned size)
{
    switch (offset >> 2) {
    case 0: /* ALPHA */
    case 1: /* LEDS */
    case 2: /* SWITCHES */
        qemu_log_mask(LOG_UNIMP,
                      "%s: returning zero from %" HWADDR_PRIx ":%u\n",
                      __func__, offset, size);
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset %" HWADDR_PRIx,
                      __func__, offset);
        return 0;
    }
}

void IntegratorDebugState::mmioWrite(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    switch (offset >> 2) {
    case 1: /* ALPHA */
    case 2: /* LEDS */
    case 3: /* SWITCHES */
        /* Nothing interesting implemented yet.  */
        qemu_log_mask(LOG_UNIMP,
                      "%s: ignoring write of %" PRIu64
                      " to %" HWADDR_PRIx ":%u\n",
                      __func__, value, offset, size);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write of %" PRIu64
                      " to bad offset %" HWADDR_PRIx "\n",
                      __func__, value, offset);
    }
}

static const MemoryRegionOps intdbg_control_ops = {
    .read = IntegratorDebugState::mmioRead,
    .write = IntegratorDebugState::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void IntegratorDebugState::init()
{
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &intdbg_control_ops, NULL, "dbg-leds", 0x1000000);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

REGISTER_QEMU_DEVICE(IntegratorDebugState, TYPE_INTEGRATOR_DEBUG,
                     TYPE_SYS_BUS_DEVICE)
