/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2024 IBM Corp.
 *
 * IBM Flexible Service Interface
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#include "hw/fsi/fsi.h"
#include "qom/cpp/object.h"

#define TO_REG(x)                               ((x) >> 2)

static const TypeInfo fsi_bus_info = {
    .name = TYPE_FSI_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(FSIBus),
};

static uint64_t fsi_slave_read(void *opaque, hwaddr addr, unsigned size)
{
    FSISlaveState *s = FSI_SLAVE(opaque);
    int reg = TO_REG(addr);

    trace_fsi_slave_read(addr, size);

    if (reg >= FSI_SLAVE_CONTROL_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out of bounds read: 0x%" HWADDR_PRIx " for %u\n",
                      __func__, addr, size);
        return 0;
    }

    return s->regs[reg];
}

static void fsi_slave_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    FSISlaveState *s = FSI_SLAVE(opaque);
    int reg = TO_REG(addr);

    trace_fsi_slave_write(addr, size, data);

    if (reg >= FSI_SLAVE_CONTROL_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out of bounds write: 0x%" HWADDR_PRIx " for %u\n",
                      __func__, addr, size);
        return;
    }

    s->regs[reg] = data;
}

static const struct MemoryRegionOps fsi_slave_ops = {
    .read = fsi_slave_read,
    .write = fsi_slave_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void fsi_slave_reset(DeviceState *dev)
{
    FSISlaveState *s = FSI_SLAVE(dev);

    /* Initialize registers */
    memset(s->regs, 0, sizeof(s->regs));
}

void FSISlaveState::init()
{
    memory_region_init_io(&iomem, OBJECT(this), &fsi_slave_ops,
                          this, TYPE_FSI_SLAVE, 0x400);
}

void FSISlaveState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    (void)klass;

    dc->bus_type = TYPE_FSI_BUS;
    dc->desc = "FSI Slave";
    device_class_set_legacy_reset(dc, fsi_slave_reset);
}

static void __attribute__((constructor)) fsi_bus_register(void)
{
    type_register_static(&fsi_bus_info);
}

REGISTER_QEMU_DEVICE(FSISlaveState, TYPE_FSI_SLAVE, TYPE_DEVICE)
