/*
 * LASI Wrapper for NCR710 SCSI Controller
 *
 * Copyright (c) 2025 Soumyajyotii Ssarkar <soumyajyotisarkar23@gmail.com>
 * This driver was developed during the Google Summer of Code 2025 program.
 * Mentored by Helge Deller <deller@gmx.de>
 *
 * NCR710 SCSI Controller implementation
 * Based on the NCR53C710 Technical Manual Version 3.2, December 2000
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

extern "C" {
#include "hw/scsi/lasi_ncr710.h"
#include "hw/scsi/ncr53c710.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "system/blockdev.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/dma.h"
}

#include "trace.h"

#define LASI_710_SVERSION    0x00082
#define SCNR                 0xBEEFBABE
#define LASI_710_HVERSION    0x3D
#define HPHW_FIO             5        /* Fixed I/O module */

static uint64_t lasi_ncr710_reg_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    LasiNCR710State *s = LASI_NCR710(opaque);
    uint64_t val = 0;

    trace_lasi_ncr710_reg_read(addr, 0, size);

    if (addr == 0x00) {  /* Device ID */
        val = (HPHW_FIO << 24) | LASI_710_SVERSION;
        trace_lasi_ncr710_reg_read_id(HPHW_FIO, LASI_710_SVERSION, val);
        return val;
    }

    if (addr == 0x08) {  /* HVersion */
        val = LASI_710_HVERSION;
        trace_lasi_ncr710_reg_read_hversion(val);
        return val;
    }

    if (addr >= 0x100) {
        hwaddr ncr_addr = addr - 0x100;
        if (size == 1) {
            ncr_addr ^= 3;
            NCR710_DPRINTF("Reading value to LASI WRAPPER == 0x%lx%s, "
                           "val=0x%lx, size=%u\n",
                           addr - 0x100, size == 1 ? " (XORed)" : "",
                           val, size);
            val = ncr710_reg_read(&s->ncr710, ncr_addr, size);
        } else {
            val = 0;
            for (unsigned i = 0; i < size; i++) {
                uint8_t byte_val = ncr710_reg_read(&s->ncr710, ncr_addr + i, 1);
                val |= ((uint64_t)byte_val) << (i * 8);
                NCR710_DPRINTF("  Read byte %u from NCR addr 0x%lx: "
                               "0x%02x\n", i, ncr_addr + i, byte_val);
            }
            NCR710_DPRINTF("  Reconstructed %u-byte value: 0x%lx\n",
                           size, val);
        }

        trace_lasi_ncr710_reg_forward_read(addr, val);
    } else {
        val = 0;
        trace_lasi_ncr710_reg_read(addr, val, size);
    }
    return val;
}

static void lasi_ncr710_reg_write(void *opaque, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    LasiNCR710State *s = LASI_NCR710(opaque);

    trace_lasi_ncr710_reg_write(addr, val, size);

    if (addr <= 0x0F) {
        return;
    }

    if (addr >= 0x100) {
        hwaddr ncr_addr = addr - 0x100;

        if (size == 1) {
            ncr_addr ^= 3;
            NCR710_DPRINTF("Writing value to LASI WRAPPER == 0x%lx%s, "
                           "val=0x%lx, size=%u\n",
                           addr - 0x100, size == 1 ? " (XORed)" : "",
                           val, size);
            ncr710_reg_write(&s->ncr710, ncr_addr, val, size);
        } else {
            for (unsigned i = 0; i < size; i++) {
                uint8_t byte_val = (val >> (i * 8)) & 0xff;
                 NCR710_DPRINTF("  Writing byte %u to NCR addr 0x%lx: 0x%02x\n",
                       i, ncr_addr + i, byte_val);
                ncr710_reg_write(&s->ncr710, ncr_addr + i, byte_val, 1);
            }
        }

        trace_lasi_ncr710_reg_forward_write(addr, val);
    } else {
        trace_lasi_ncr710_reg_write(addr, val, size);
    }
}

/*
 * req_cancelled, command_complete, transfer_data forwards
 * commands to its core counterparts.
 */
static void lasi_ncr710_request_cancelled(SCSIRequest *req)
{
    trace_lasi_ncr710_request_cancelled(req);
    ncr710_request_cancelled(req);
}

static void lasi_ncr710_command_complete(SCSIRequest *req, size_t resid)
{
    trace_lasi_ncr710_command_complete(req->status, resid);
    ncr710_command_complete(req, resid);
}

 static void lasi_ncr710_transfer_data(SCSIRequest *req, uint32_t len)
{
    trace_lasi_ncr710_transfer_data(len);
    ncr710_transfer_data(req, len);
}

static const struct SCSIBusInfo lasi_ncr710_scsi_info = {
    .tcq = true,
    .max_target = 8,
    .max_lun = 8,  /* full LUN support */

    .transfer_data = lasi_ncr710_transfer_data,
    .complete = lasi_ncr710_command_complete,
    .cancel = lasi_ncr710_request_cancelled,
};

static MemoryRegionOps lasi_ncr710_mmio_ops = {
    .read = lasi_ncr710_reg_read,
    .write = lasi_ncr710_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void __attribute__((constructor)) init_lasi_ncr710_mmio_ops(void)
{
    lasi_ncr710_mmio_ops.valid.min_access_size = 1;
    lasi_ncr710_mmio_ops.valid.max_access_size = 4;
}

static const VMStateField vmstate_lasi_ncr710_fields[] = {
    VMSTATE_UINT32(hw_type, LasiNCR710State),
    VMSTATE_UINT32(sversion, LasiNCR710State),
    VMSTATE_UINT32(hversion, LasiNCR710State),
    VMSTATE_STRUCT(ncr710, LasiNCR710State, 1, vmstate_ncr710, NCR710State),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_lasi_ncr710 = {
    .name = "lasi-ncr710",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_lasi_ncr710_fields,
};

void LasiNCR710State::realize(Error **errp)
{
    DeviceState *dev = DEVICE(this);
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);

    trace_lasi_ncr710_device_realize();

    scsi_bus_init(&ncr710.bus, sizeof(ncr710.bus), dev,
                  &lasi_ncr710_scsi_info);
    ncr710.as = &address_space_memory;
    ncr710.irq = lasi_irq;

    ncr710.reselection_retry_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL,
                     ncr710_reselection_retry_callback,
                     &ncr710);

    ncr710_soft_reset(&ncr710);

    trace_lasi_ncr710_timers_initialized(
        (uint64_t)ncr710.reselection_retry_timer);

    /* Initialize memory region */
    memory_region_init_io(&mmio, OBJECT(this), &lasi_ncr710_mmio_ops, this,
                          "lasi-ncr710", 0x200);
    sysbus_init_mmio(sbd, &mmio);
}

extern "C"
void lasi_ncr710_handle_legacy_cmdline(DeviceState *lasi_dev)
{
    LasiNCR710State *s = LASI_NCR710(lasi_dev);
    SCSIBus *bus = &s->ncr710.bus;
    int found_drives = 0;

    if (!bus) {
        return;
    }

    for (int unit = 0; unit <= 7; unit++) {
        DriveInfo *dinfo = drive_get(IF_SCSI, bus->busnr, unit);
        if (dinfo) {
            trace_lasi_ncr710_legacy_drive_found(bus->busnr, unit);
            found_drives++;
        }
    }

    trace_lasi_ncr710_handle_legacy_cmdline(bus->busnr, found_drives);

    scsi_bus_legacy_handle_cmdline(bus);
    BusChild *kid;
    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        trace_lasi_ncr710_scsi_device_created(
            object_get_typename(OBJECT(kid->child)));
    }
}

extern "C"
DeviceState *lasi_ncr710_init(MemoryRegion *addr_space, hwaddr hpa,
                               qemu_irq irq)
{
    DeviceState *dev;
    LasiNCR710State *s;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_LASI_NCR710);
    s = LASI_NCR710(dev);
    sbd = SYS_BUS_DEVICE(dev);
    s->lasi_irq = irq;
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(addr_space, hpa,
                               sysbus_mmio_get_region(sbd, 0));
    return dev;
}

void LasiNCR710State::reset()
{
    trace_lasi_ncr710_device_reset();
    ncr710_soft_reset(&ncr710);
}

void LasiNCR710State::init()
{
    hw_type = HPHW_FIO;
    sversion = LASI_710_SVERSION;
    hversion = LASI_710_HVERSION;
}

void LasiNCR710State::classInit(DeviceClass *dc)
{
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->fw_name = "scsi";
    dc->desc = "HP-PARISC LASI NCR710 SCSI adapter";
    dc->vmsd = &vmstate_lasi_ncr710;
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(LasiNCR710State, TYPE_LASI_NCR710, TYPE_SYS_BUS_DEVICE)
