/*
 * BCM2835 Power Management emulation
 *
 * Copyright (C) 2017 Marcin Chojnacki <marcinch7@gmail.com>
 * Copyright (C) 2021 Nolan Leake <nolan@sigbus.net>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bcm2835_powermgt.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "qom/cpp/object.h"

#define PASSWORD 0x5a000000
#define PASSWORD_MASK 0xff000000

#define R_RSTC 0x1c
#define V_RSTC_RESET 0x20
#define R_RSTS 0x20
#define V_RSTS_POWEROFF 0x555 /* Linux uses partition 63 to indicate halt. */
#define R_WDOG 0x24

static uint64_t bcm2835_powermgt_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BCM2835PowerMgtState *s = (BCM2835PowerMgtState *)opaque;
    uint32_t res = 0;

    switch (offset) {
    case R_RSTC:
        res = s->rstc;
        break;
    case R_RSTS:
        res = s->rsts;
        break;
    case R_WDOG:
        res = s->wdog;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_read: Unknown offset 0x%08" HWADDR_PRIx
                      "\n", offset);
        res = 0;
        break;
    }

    return res;
}

static void bcm2835_powermgt_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BCM2835PowerMgtState *s = (BCM2835PowerMgtState *)opaque;

    if ((value & PASSWORD_MASK) != PASSWORD) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_powermgt_write: Bad password 0x%" PRIx64
                      " at offset 0x%08" HWADDR_PRIx"\n",
                      value, offset);
        return;
    }

    value = value & ~PASSWORD_MASK;

    switch (offset) {
    case R_RSTC:
        s->rstc = value;
        if (value & V_RSTC_RESET) {
            if ((s->rsts & 0xfff) == V_RSTS_POWEROFF) {
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            } else {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
        }
        break;
    case R_RSTS:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_write: RSTS\n");
        s->rsts = value;
        break;
    case R_WDOG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_write: WDOG\n");
        s->wdog = value;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_write: Unknown offset 0x%08" HWADDR_PRIx
                      "\n", offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_powermgt_ops = {
    .read = bcm2835_powermgt_read,
    .write = bcm2835_powermgt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
};

static const VMStateDescription vmstate_bcm2835_powermgt = {
    .name = TYPE_BCM2835_POWERMGT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rstc, BCM2835PowerMgtState),
        VMSTATE_UINT32(rsts, BCM2835PowerMgtState),
        VMSTATE_UINT32(wdog, BCM2835PowerMgtState),
        VMSTATE_END_OF_LIST()
    }
};

void BCM2835PowerMgtState::init()
{
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &bcm2835_powermgt_ops, this,
                          TYPE_BCM2835_POWERMGT, 0x200);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

void BCM2835PowerMgtState::reset()
{
    /* https://elinux.org/BCM2835_registers#PM */
    rstc = 0x00000102;
    rsts = 0x00001000;
    wdog = 0x00000000;
}

void BCM2835PowerMgtState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_bcm2835_powermgt;
}

REGISTER_QEMU_DEVICE(BCM2835PowerMgtState, TYPE_BCM2835_POWERMGT,
                     TYPE_SYS_BUS_DEVICE)
