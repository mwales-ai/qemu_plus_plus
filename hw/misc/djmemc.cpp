/*
 * djMEMC, macintosh memory and interrupt controller
 * (Quadra 610/650/800 & Centris 610/650)
 *
 *    https://mac68k.info/wiki/display/mac68k/djMEMC+Information
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/djmemc.h"
#include "hw/qdev-properties.h"
#include "qom/cpp/object.h"
#include "trace.h"


#define DJMEMC_INTERLEAVECONF   0x0
#define DJMEMC_BANK0CONF        0x4
#define DJMEMC_BANK1CONF        0x8
#define DJMEMC_BANK2CONF        0xc
#define DJMEMC_BANK3CONF        0x10
#define DJMEMC_BANK4CONF        0x14
#define DJMEMC_BANK5CONF        0x18
#define DJMEMC_BANK6CONF        0x1c
#define DJMEMC_BANK7CONF        0x20
#define DJMEMC_BANK8CONF        0x24
#define DJMEMC_BANK9CONF        0x28
#define DJMEMC_MEMTOP           0x2c
#define DJMEMC_CONFIG           0x30
#define DJMEMC_REFRESH          0x34


static uint64_t djmemc_read(void *opaque, hwaddr addr, unsigned size)
{
    DJMEMCState *s = static_cast<DJMEMCState *>(opaque);
    uint64_t val = 0;

    switch (addr) {
    case DJMEMC_INTERLEAVECONF:
    case DJMEMC_BANK0CONF ... DJMEMC_BANK9CONF:
    case DJMEMC_MEMTOP:
    case DJMEMC_CONFIG:
    case DJMEMC_REFRESH:
        val = s->regs[addr >> 2];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "djMEMC: unimplemented read addr=0x%"PRIx64
                                 " val=0x%"PRIx64 " size=%d\n",
                                 addr, val, size);
    }

    trace_djmemc_read(addr, val, size);
    return val;
}

static void djmemc_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    DJMEMCState *s = static_cast<DJMEMCState *>(opaque);

    trace_djmemc_write(addr, val, size);

    switch (addr) {
    case DJMEMC_INTERLEAVECONF:
    case DJMEMC_BANK0CONF ... DJMEMC_BANK9CONF:
    case DJMEMC_MEMTOP:
    case DJMEMC_CONFIG:
    case DJMEMC_REFRESH:
        s->regs[addr >> 2] = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "djMEMC: unimplemented write addr=0x%"PRIx64
                                 " val=0x%"PRIx64 " size=%d\n",
                                 addr, val, size);
    }
}

static const MemoryRegionOps djmemc_mmio_ops = {
    .read = djmemc_read,
    .write = djmemc_write,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_BIG_ENDIAN,
};

void DJMEMCState::init()
{
    memory_region_init_io(&mem_regs, reinterpret_cast<Object *>(this),
                          &djmemc_mmio_ops, this, "djMEMC", DJMEMC_SIZE);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &mem_regs);
}

static void djmemc_reset_hold(Object *obj, ResetType type)
{
    DJMEMCState *s = DJMEMC(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_djmemc = {
    .name = "djMEMC",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DJMEMCState, DJMEMC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    }
};

void DJMEMCState::classInit(DeviceClass *dc)
{
    ResettableClass *rc = reinterpret_cast<ResettableClass *>(dc);

    dc->vmsd = &vmstate_djmemc;
    rc->phases.hold = djmemc_reset_hold;
}

REGISTER_QEMU_DEVICE(DJMEMCState, TYPE_DJMEMC, TYPE_SYS_BUS_DEVICE)
