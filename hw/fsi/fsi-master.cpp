/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2024 IBM Corp.
 *
 * IBM Flexible Service Interface master
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#include "hw/fsi/fsi-master.h"

#define TYPE_OP_BUS "opb"

#define TO_REG(x)                               ((x) >> 2)

#define FSI_MENP0                               TO_REG(0x010)
#define FSI_MENP32                              TO_REG(0x014)
#define FSI_MSENP0                              TO_REG(0x018)
#define FSI_MLEVP0                              TO_REG(0x018)
#define FSI_MSENP32                             TO_REG(0x01c)
#define FSI_MLEVP32                             TO_REG(0x01c)
#define FSI_MCENP0                              TO_REG(0x020)
#define FSI_MREFP0                              TO_REG(0x020)
#define FSI_MCENP32                             TO_REG(0x024)
#define FSI_MREFP32                             TO_REG(0x024)

#define FSI_MVER                                TO_REG(0x074)
#define FSI_MRESP0                              TO_REG(0x0d0)

#define FSI_MRESB0                              TO_REG(0x1d0)
#define   FSI_MRESB0_RESET_GENERAL              BIT(31)
#define   FSI_MRESB0_RESET_ERROR                BIT(30)

static uint64_t fsi_master_read(void *opaque, hwaddr addr, unsigned size)
{
    FSIMasterState *s = FSI_MASTER(opaque);
    int reg = TO_REG(addr);

    trace_fsi_master_read(addr, size);

    if (reg >= FSI_MASTER_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out of bounds read: 0x%" HWADDR_PRIx " for %u\n",
                      __func__, addr, size);
        return 0;
    }

    return s->regs[reg];
}

static void fsi_master_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    FSIMasterState *s = FSI_MASTER(opaque);
    int reg = TO_REG(addr);

    trace_fsi_master_write(addr, size, data);

    if (reg >= FSI_MASTER_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out of bounds write: %" HWADDR_PRIx " for %u\n",
                      __func__, addr, size);
        return;
    }

    switch (reg) {
    case FSI_MENP0:
        s->regs[FSI_MENP0] = data;
        break;
    case FSI_MENP32:
        s->regs[FSI_MENP32] = data;
        break;
    case FSI_MSENP0:
        s->regs[FSI_MENP0] |= data;
        break;
    case FSI_MSENP32:
        s->regs[FSI_MENP32] |= data;
        break;
    case FSI_MCENP0:
        s->regs[FSI_MENP0] &= ~data;
        break;
    case FSI_MCENP32:
        s->regs[FSI_MENP32] &= ~data;
        break;
    case FSI_MRESP0:
        /* Perform necessary resets leave register 0 to indicate no errors */
        break;
    case FSI_MRESB0:
        if (data & FSI_MRESB0_RESET_GENERAL) {
            device_cold_reset(DEVICE(opaque));
        }
        if (data & FSI_MRESB0_RESET_ERROR) {
            /* FIXME: this seems dubious */
            device_cold_reset(DEVICE(opaque));
        }
        break;
    default:
        s->regs[reg] = data;
    }
}

static const struct MemoryRegionOps fsi_master_ops = {
    .read = fsi_master_read,
    .write = fsi_master_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

void FSIMasterState::init()
{
    object_initialize_child(OBJECT(this), "cfam", &cfam, TYPE_FSI_CFAM);

    qbus_init(&bus, sizeof(bus), TYPE_FSI_BUS, DEVICE(this), NULL);

    memory_region_init_io(&iomem, OBJECT(this), &fsi_master_ops, this,
                          TYPE_FSI_MASTER, 0x10000000);
    memory_region_init(&opb2fsi, OBJECT(this), "fsi.opb2fsi", 0x10000000);
}

void FSIMasterState::realize(Error **errp)
{
    if (!qdev_realize(DEVICE(&cfam), BUS(&bus), errp)) {
        return;
    }

    memory_region_add_subregion(&opb2fsi, 0, &cfam.mr);
}

void FSIMasterState::reset()
{
    memset(regs, 0, sizeof(regs));
    regs[FSI_MVER] = 0xe0050101;
}

void FSIMasterState::classInit(DeviceClass *dc)
{
    dc->bus_type = TYPE_OP_BUS;
    dc->desc = "FSI Master";
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(FSIMasterState, TYPE_FSI_MASTER, TYPE_DEVICE)
