/*
 * QEMU SiI3112A PCI to Serial ATA Controller Emulation
 *
 * Copyright (C) 2017 BALATON Zoltan <balaton@eik.bme.hu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* For documentation on this and similar cards see:
 * http://wiki.osdev.org/User:Quok/Silicon_Image_Datasheets
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "hw/ide/pci.h"
#include "qemu/module.h"
#include "trace.h"
#include "qom/object.h"
#include "ide-internal.h"

#define TYPE_SII3112_PCI "sii3112"
OBJECT_DECLARE_SIMPLE_TYPE(SiI3112PCIState, SII3112_PCI)

typedef struct SiI3112Regs {
    uint32_t confstat;
    uint32_t scontrol;
    uint16_t sien;
    uint8_t swdata;
} SiI3112Regs;

struct SiI3112PCIState {
    PCIIDEState i;
    MemoryRegion mmio;
    SiI3112Regs regs[2];

    static uint64_t regRead(void *opaque, hwaddr addr, unsigned int size);
    static void regWrite(void *opaque, hwaddr addr, uint64_t val,
                         unsigned int size);
    void updateIrq();
    static void setIrq(void *opaque, int channel, int level);

    void reset();
    static void resetWrapper(DeviceState *dev);

    void realize(Error **errp);
    static void realizeWrapper(PCIDevice *dev, Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
};

/* The sii3112_reg_read and sii3112_reg_write functions implement the
 * Internal Register Space - BAR5 (section 6.7 of the data sheet).
 */

uint64_t SiI3112PCIState::regRead(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    SiI3112PCIState *d = static_cast<SiI3112PCIState *>(opaque);
    uint64_t val;

    switch (addr) {
    case 0x00:
        val = d->i.bmdma[0].cmd;
        break;
    case 0x01:
        val = d->regs[0].swdata;
        break;
    case 0x02:
        val = d->i.bmdma[0].status;
        break;
    case 0x03:
        val = 0;
        break;
    case 0x04 ... 0x07:
        val = bmdma_addr_ioport_ops.read(&d->i.bmdma[0], addr - 4, size);
        break;
    case 0x08:
        val = d->i.bmdma[1].cmd;
        break;
    case 0x09:
        val = d->regs[1].swdata;
        break;
    case 0x0a:
        val = d->i.bmdma[1].status;
        break;
    case 0x0b:
        val = 0;
        break;
    case 0x0c ... 0x0f:
        val = bmdma_addr_ioport_ops.read(&d->i.bmdma[1], addr - 12, size);
        break;
    case 0x10:
        val = d->i.bmdma[0].cmd;
        val |= (d->regs[0].confstat & (1UL << 11) ? (1 << 4) : 0); /*SATAINT0*/
        val |= (d->regs[1].confstat & (1UL << 11) ? (1 << 6) : 0); /*SATAINT1*/
        val |= (d->i.bmdma[1].status & BM_STATUS_INT ? (1 << 14) : 0);
        val |= (uint32_t)d->i.bmdma[0].status << 16;
        val |= (uint32_t)d->i.bmdma[1].status << 24;
        break;
    case 0x18:
        val = d->i.bmdma[1].cmd;
        val |= (d->regs[1].confstat & (1UL << 11) ? (1 << 4) : 0);
        val |= (uint32_t)d->i.bmdma[1].status << 16;
        break;
    case 0x80 ... 0x87:
        val = pci_ide_data_le_ops.read(&d->i.bus[0], addr - 0x80, size);
        break;
    case 0x8a:
        val = pci_ide_cmd_le_ops.read(&d->i.bus[0], 2, size);
        break;
    case 0xa0:
        val = d->regs[0].confstat;
        break;
    case 0xc0 ... 0xc7:
        val = pci_ide_data_le_ops.read(&d->i.bus[1], addr - 0xc0, size);
        break;
    case 0xca:
        val = pci_ide_cmd_le_ops.read(&d->i.bus[1], 2, size);
        break;
    case 0xe0:
        val = d->regs[1].confstat;
        break;
    case 0x100:
        val = d->regs[0].scontrol;
        break;
    case 0x104:
        val = (d->i.bus[0].ifs[0].blk) ? 0x113 : 0;
        break;
    case 0x148:
        val = (uint32_t)d->regs[0].sien << 16;
        break;
    case 0x180:
        val = d->regs[1].scontrol;
        break;
    case 0x184:
        val = (d->i.bus[1].ifs[0].blk) ? 0x113 : 0;
        break;
    case 0x1c8:
        val = (uint32_t)d->regs[1].sien << 16;
        break;
    default:
        val = 0;
        break;
    }
    trace_sii3112_read(size, addr, val);
    return val;
}

void SiI3112PCIState::regWrite(void *opaque, hwaddr addr,
                                uint64_t val, unsigned int size)
{
    SiI3112PCIState *d = static_cast<SiI3112PCIState *>(opaque);

    trace_sii3112_write(size, addr, val);
    switch (addr) {
    case 0x00:
    case 0x10:
        bmdma_cmd_writeb(&d->i.bmdma[0], val);
        break;
    case 0x01:
    case 0x11:
        d->regs[0].swdata = val & 0x3f;
        break;
    case 0x02:
    case 0x12:
        bmdma_status_writeb(&d->i.bmdma[0], val);
        break;
    case 0x04 ... 0x07:
        bmdma_addr_ioport_ops.write(&d->i.bmdma[0], addr - 4, val, size);
        break;
    case 0x08:
    case 0x18:
        bmdma_cmd_writeb(&d->i.bmdma[1], val);
        break;
    case 0x09:
    case 0x19:
        d->regs[1].swdata = val & 0x3f;
        break;
    case 0x0a:
    case 0x1a:
        bmdma_status_writeb(&d->i.bmdma[1], val);
        break;
    case 0x0c ... 0x0f:
        bmdma_addr_ioport_ops.write(&d->i.bmdma[1], addr - 12, val, size);
        break;
    case 0x80 ... 0x87:
        pci_ide_data_le_ops.write(&d->i.bus[0], addr - 0x80, val, size);
        break;
    case 0x8a:
        pci_ide_cmd_le_ops.write(&d->i.bus[0], 2, val, size);
        break;
    case 0xc0 ... 0xc7:
        pci_ide_data_le_ops.write(&d->i.bus[1], addr - 0xc0, val, size);
        break;
    case 0xca:
        pci_ide_cmd_le_ops.write(&d->i.bus[1], 2, val, size);
        break;
    case 0x100:
        d->regs[0].scontrol = val & 0xfff;
        if (val & 1) {
            ide_bus_reset(&d->i.bus[0]);
        }
        break;
    case 0x148:
        d->regs[0].sien = (val >> 16) & 0x3eed;
        break;
    case 0x180:
        d->regs[1].scontrol = val & 0xfff;
        if (val & 1) {
            ide_bus_reset(&d->i.bus[1]);
        }
        break;
    case 0x1c8:
        d->regs[1].sien = (val >> 16) & 0x3eed;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sii3112_reg_ops = {
    .read = SiI3112PCIState::regRead,
    .write = SiI3112PCIState::regWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* the PCI irq level is the logical OR of the two channels */
void SiI3112PCIState::updateIrq()
{
    int i, set = 0;

    for (i = 0; i < 2; i++) {
        set |= regs[i].confstat & (1UL << 11);
    }
    pci_set_irq(reinterpret_cast<PCIDevice *>(this), (set ? 1 : 0));
}

void SiI3112PCIState::setIrq(void *opaque, int channel, int level)
{
    SiI3112PCIState *s = static_cast<SiI3112PCIState *>(opaque);

    trace_sii3112_set_irq(channel, level);
    if (level) {
        s->regs[channel].confstat |= (1UL << 11);
    } else {
        s->regs[channel].confstat &= ~(1UL << 11);
    }

    s->updateIrq();
}

void SiI3112PCIState::reset()
{
    int i;

    for (i = 0; i < 2; i++) {
        regs[i].confstat = 0x6515 << 16;
        ide_bus_reset(&this->i.bus[i]);
    }
}

void SiI3112PCIState::resetWrapper(DeviceState *dev)
{
    SiI3112PCIState *s = reinterpret_cast<SiI3112PCIState *>(dev);
    s->reset();
}

void SiI3112PCIState::realize(Error **errp)
{
    PCIDevice *dev = reinterpret_cast<PCIDevice *>(this);
    PCIIDEState *s = reinterpret_cast<PCIIDEState *>(dev);
    DeviceState *ds = reinterpret_cast<DeviceState *>(dev);
    MemoryRegion *mr;
    int idx;

    pci_config_set_interrupt_pin(dev->config, 1);
    pci_set_byte(dev->config + PCI_CACHE_LINE_SIZE, 8);

    /* BAR5 is in PCI memory space */
    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this), &sii3112_reg_ops, this,
                         "sii3112.bar5", 0x200);
    pci_register_bar(dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &mmio);

    /* BAR0-BAR4 are PCI I/O space aliases into BAR5 */
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, reinterpret_cast<Object *>(this), "sii3112.bar0", &mmio, 0x80, 8);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, reinterpret_cast<Object *>(this), "sii3112.bar1", &mmio, 0x88, 4);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, reinterpret_cast<Object *>(this), "sii3112.bar2", &mmio, 0xc0, 8);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, reinterpret_cast<Object *>(this), "sii3112.bar3", &mmio, 0xc8, 4);
    pci_register_bar(dev, 3, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, reinterpret_cast<Object *>(this), "sii3112.bar4", &mmio, 0, 16);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, mr);

    qdev_init_gpio_in(ds, setIrq, 2);
    for (idx = 0; idx < 2; idx++) {
        ide_bus_init(&s->bus[idx], sizeof(s->bus[idx]), ds, idx, 1);
        ide_bus_init_output_irq(&s->bus[idx], qdev_get_gpio_in(ds, idx));

        bmdma_init(&s->bus[idx], &s->bmdma[idx], s);
        ide_bus_register_restart_cb(&s->bus[idx]);
    }
}

void SiI3112PCIState::realizeWrapper(PCIDevice *dev, Error **errp)
{
    SiI3112PCIState *s = reinterpret_cast<SiI3112PCIState *>(dev);
    s->realize(errp);
}

void SiI3112PCIState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *pd = reinterpret_cast<PCIDeviceClass *>(klass);

    pd->vendor_id = 0x1095;
    pd->device_id = 0x3112;
    pd->class_id = PCI_CLASS_STORAGE_RAID;
    pd->revision = 1;
    pd->realize = realizeWrapper;
    device_class_set_legacy_reset(dc, resetWrapper);
    dc->desc = "SiI3112A SATA controller";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo sii3112_pci_info = {
    .name = TYPE_SII3112_PCI,
    .parent = TYPE_PCI_IDE,
    .instance_size = sizeof(SiI3112PCIState),
    .class_init = SiI3112PCIState::classInit,
};

static void sii3112_register_types(void)
{
    type_register_static(&sii3112_pci_info);
}

type_init(sii3112_register_types)
