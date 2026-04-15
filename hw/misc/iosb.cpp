/*
 * QEMU IOSB emulation
 *
 * Copyright (c) 2019 Laurent Vivier
 * Copyright (c) 2022 Mark Cave-Ayland
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/sysbus.h"
#include "hw/misc/iosb.h"
#include "qom/cpp/object.h"
#include "trace.h"

#define IOSB_SIZE          0x2000

#define IOSB_CONFIG        0x0
#define IOSB_CONFIG2       0x100
#define IOSB_SONIC_SCSI    0x200
#define IOSB_REVISION      0x300
#define IOSB_SCSI_RESID    0x400
#define IOSB_BRIGHTNESS    0x500
#define IOSB_TIMEOUT       0x600


static uint64_t iosb_read(void *opaque, hwaddr addr,
                          unsigned size)
{
    IOSBState *s = IOSB(opaque);
    uint64_t val = 0;

    switch (addr) {
    case IOSB_CONFIG:
    case IOSB_CONFIG2:
    case IOSB_SONIC_SCSI:
    case IOSB_REVISION:
    case IOSB_SCSI_RESID:
    case IOSB_BRIGHTNESS:
    case IOSB_TIMEOUT:
        val = s->regs[addr >> 8];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "IOSB: unimplemented read addr=0x%"PRIx64
                                 " val=0x%"PRIx64 " size=%d\n",
                                 addr, val, size);
    }

    trace_iosb_read(addr, val, size);
    return val;
}

static void iosb_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    IOSBState *s = IOSB(opaque);

    switch (addr) {
    case IOSB_CONFIG:
    case IOSB_CONFIG2:
    case IOSB_SONIC_SCSI:
    case IOSB_REVISION:
    case IOSB_SCSI_RESID:
    case IOSB_BRIGHTNESS:
    case IOSB_TIMEOUT:
        s->regs[addr >> 8] = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "IOSB: unimplemented write addr=0x%"PRIx64
                                 " val=0x%"PRIx64 " size=%d\n",
                                 addr, val, size);
    }

    trace_iosb_write(addr, val, size);
}

static const MemoryRegionOps iosb_mmio_ops = {
    .read = iosb_read,
    .write = iosb_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void iosb_reset_hold(Object *obj, ResetType type)
{
    IOSBState *s = IOSB(obj);

    memset(s->regs, 0, sizeof(s->regs));

    /* BCLK 33 MHz */
    s->regs[IOSB_CONFIG >> 8] = 1;
}

void IOSBState::init()
{
    memory_region_init_io(&mem_regs, reinterpret_cast<Object *>(this),
                          &iosb_mmio_ops, this, "IOSB", IOSB_SIZE);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &mem_regs);
}

static const VMStateField vmstate_iosb_fields[] = {
    VMSTATE_UINT32_ARRAY(regs, IOSBState, IOSB_REGS),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_iosb = {
    .name = "IOSB",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_iosb_fields,
};

void IOSBState::classInit(DeviceClass *dc)
{
    ResettableClass *rc = reinterpret_cast<ResettableClass *>(dc);

    dc->vmsd = &vmstate_iosb;
    rc->phases.hold = iosb_reset_hold;
}

REGISTER_QEMU_DEVICE(IOSBState, TYPE_IOSB, TYPE_SYS_BUS_DEVICE)
