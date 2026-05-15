/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2024 IBM Corp.
 *
 * IBM Local bus where FSI slaves are connected
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/fsi/lbus.h"
#include "qom/cpp/object.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "trace.h"

#define TO_REG(offset) ((offset) >> 2)

static void fsi_lbus_init(Object *o)
{
    FSILBus *lbus = FSI_LBUS(o);

    memory_region_init(&lbus->mr, OBJECT(lbus), TYPE_FSI_LBUS, 1 * MiB);
}


static uint64_t fsi_scratchpad_read(void *opaque, hwaddr addr, unsigned size)
{
    FSIScratchPad *s = SCRATCHPAD(opaque);
    int reg = TO_REG(addr);

    trace_fsi_scratchpad_read(addr, size);

    if (reg >= FSI_SCRATCHPAD_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out-of-bounds read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    return s->regs[reg];
}

static void fsi_scratchpad_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    FSIScratchPad *s = SCRATCHPAD(opaque);

    trace_fsi_scratchpad_write(addr, size, data);
    int reg = TO_REG(addr);

    if (reg >= FSI_SCRATCHPAD_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out-of-bounds write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    s->regs[reg] = data;
}

static const struct MemoryRegionOps scratchpad_ops = {
    .read = fsi_scratchpad_read,
    .write = fsi_scratchpad_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void fsi_scratchpad_realize(DeviceState *dev, Error **errp)
{
    FSILBusDevice *ldev = FSI_LBUS_DEVICE(dev);

    memory_region_init_io(&ldev->iomem, OBJECT(ldev), &scratchpad_ops,
                          ldev, TYPE_FSI_SCRATCHPAD, 0x400);
}

static void fsi_scratchpad_reset(DeviceState *dev)
{
    FSIScratchPad *s = SCRATCHPAD(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

void FSIScratchPad::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    (void)klass;

    dc->bus_type = TYPE_FSI_LBUS;
    dc->realize = fsi_scratchpad_realize;
    device_class_set_legacy_reset(dc, fsi_scratchpad_reset);
}

REGISTER_QEMU_OBJECT_INIT_ONLY_SIZED(fsi_lbus, FSILBus,
                                      TYPE_FSI_LBUS, TYPE_BUS,
                                      fsi_lbus_init)

REGISTER_QEMU_OBJECT_ABSTRACT_SIZED(FSILBusDevice, TYPE_FSI_LBUS_DEVICE,
                                     TYPE_DEVICE)

REGISTER_QEMU_DEVICE(FSIScratchPad, TYPE_FSI_SCRATCHPAD, TYPE_FSI_LBUS_DEVICE)
