/*
 * QEMU AMD PC-Net II (Am79C970A) PCI emulation
 *
 * Copyright (c) 2004 Antony T Curtis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* This software was written to be compatible with the specification:
 * AMD Am79C970A PCnet-PCI II Ethernet Controller Data-Sheet
 * AMD Publication# 19436  Rev:E  Amendment/0  Issue Date: June 2000
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/dma.h"
#include "system/system.h"
#include "trace.h"

#include "pcnet.h"
#include "qom/object.h"

//#define PCNET_DEBUG
//#define PCNET_DEBUG_IO
//#define PCNET_DEBUG_BCR
//#define PCNET_DEBUG_CSR
//#define PCNET_DEBUG_RMD
//#define PCNET_DEBUG_TMD
//#define PCNET_DEBUG_MATCH

#define TYPE_PCI_PCNET "pcnet"

OBJECT_DECLARE_SIMPLE_TYPE(PCIPCNetState, PCI_PCNET)

struct PCIPCNetState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    PCNetState state;
    MemoryRegion io_bar;

    /* methods */
    static void apromWriteb(void *opaque, uint32_t addr, uint32_t val);
    static uint32_t apromReadb(void *opaque, uint32_t addr);
    static uint64_t ioportRead(void *opaque, hwaddr addr, unsigned size);
    static void ioportWrite(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size);
    static void pciPhysicalMemoryWrite(void *dma_opaque, hwaddr addr,
                                       uint8_t *buf, int len, int do_bswap);
    static void pciPhysicalMemoryRead(void *dma_opaque, hwaddr addr,
                                      uint8_t *buf, int len, int do_bswap);
    void pciUninit();
    void pciRealize(Error **errp);
    void reset();
    void instanceInit();
    static void classInit(ObjectClass *klass, const void *data);
};

void PCIPCNetState::apromWriteb(void *opaque, uint32_t addr, uint32_t val)
{
    PCNetState *s = static_cast<PCNetState *>(opaque);

    trace_pcnet_aprom_writeb(opaque, addr, val);
    if (BCR_APROMWE(s)) {
        s->prom[addr & 15] = val;
    }
}

uint32_t PCIPCNetState::apromReadb(void *opaque, uint32_t addr)
{
    PCNetState *s = static_cast<PCNetState *>(opaque);
    uint32_t val = s->prom[addr & 15];

    trace_pcnet_aprom_readb(opaque, addr, val);
    return val;
}

uint64_t PCIPCNetState::ioportRead(void *opaque, hwaddr addr,
                                   unsigned size)
{
    PCNetState *d = static_cast<PCNetState *>(opaque);

    trace_pcnet_ioport_read(opaque, addr, size);
    if (addr < 0x10) {
        if (!BCR_DWIO(d) && size == 1) {
            return apromReadb(d, addr);
        } else if (!BCR_DWIO(d) && (addr & 1) == 0 && size == 2) {
            return apromReadb(d, addr) |
                   (apromReadb(d, addr + 1) << 8);
        } else if (BCR_DWIO(d) && (addr & 3) == 0 && size == 4) {
            return apromReadb(d, addr) |
                   (apromReadb(d, addr + 1) << 8) |
                   (apromReadb(d, addr + 2) << 16) |
                   (apromReadb(d, addr + 3) << 24);
        }
    } else {
        if (size == 2) {
            return pcnet_ioport_readw(d, addr);
        } else if (size == 4) {
            return pcnet_ioport_readl(d, addr);
        }
    }
    return ((uint64_t)1 << (size * 8)) - 1;
}

void PCIPCNetState::ioportWrite(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    PCNetState *d = static_cast<PCNetState *>(opaque);

    trace_pcnet_ioport_write(opaque, addr, data, size);
    if (addr < 0x10) {
        if (!BCR_DWIO(d) && size == 1) {
            apromWriteb(d, addr, data);
        } else if (!BCR_DWIO(d) && (addr & 1) == 0 && size == 2) {
            apromWriteb(d, addr, data & 0xff);
            apromWriteb(d, addr + 1, data >> 8);
        } else if (BCR_DWIO(d) && (addr & 3) == 0 && size == 4) {
            apromWriteb(d, addr, data & 0xff);
            apromWriteb(d, addr + 1, (data >> 8) & 0xff);
            apromWriteb(d, addr + 2, (data >> 16) & 0xff);
            apromWriteb(d, addr + 3, data >> 24);
        }
    } else {
        if (size == 2) {
            pcnet_ioport_writew(d, addr, data);
        } else if (size == 4) {
            pcnet_ioport_writel(d, addr, data);
        }
    }
}

static const MemoryRegionOps pcnet_io_ops = {
    .read = PCIPCNetState::ioportRead,
    .write = PCIPCNetState::ioportWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const VMStateField vmstate_pci_pcnet_fields[] = {
    VMSTATE_PCI_DEVICE(parent_obj, PCIPCNetState),
    VMSTATE_STRUCT(state, PCIPCNetState, 0, vmstate_pcnet, PCNetState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_pci_pcnet = {
    .name = "pcnet",
    .version_id = 3,
    .minimum_version_id = 2,
    .fields = vmstate_pci_pcnet_fields,
};

/* PCI interface */

static const MemoryRegionOps pcnet_mmio_ops = {
    .read = PCIPCNetState::ioportRead,
    .write = PCIPCNetState::ioportWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
    .impl = { .min_access_size = 1, .max_access_size = 4, },
};

void PCIPCNetState::pciPhysicalMemoryWrite(void *dma_opaque, hwaddr addr,
                                           uint8_t *buf, int len, int do_bswap)
{
    pci_dma_write(static_cast<PCIDevice *>(dma_opaque), addr, buf, len);
}

void PCIPCNetState::pciPhysicalMemoryRead(void *dma_opaque, hwaddr addr,
                                          uint8_t *buf, int len, int do_bswap)
{
    pci_dma_read(static_cast<PCIDevice *>(dma_opaque), addr, buf, len);
}

static void pci_pcnet_uninit(PCIDevice *dev)
{
    PCIPCNetState *d = PCI_PCNET(dev);
    d->pciUninit();
}

void PCIPCNetState::pciUninit()
{
    qemu_free_irq(this->state.irq);
    timer_free(this->state.poll_timer);
    qemu_del_nic(this->state.nic);
}

static NetClientInfo net_pci_pcnet_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = pcnet_receive,
    .link_status_changed = pcnet_set_link_status,
};

static void pci_pcnet_realize(PCIDevice *pci_dev, Error **errp)
{
    PCIPCNetState *d = PCI_PCNET(pci_dev);
    d->pciRealize(errp);
}

void PCIPCNetState::pciRealize(Error **errp)
{
    PCIDevice *pci_dev = PCI_DEVICE(DEVICE(this));
    PCNetState *s = &this->state;
    uint8_t *pci_conf;

#if 0
    printf("sizeof(RMD)=%d, sizeof(TMD)=%d\n",
        sizeof(struct pcnet_RMD), sizeof(struct pcnet_TMD));
#endif

    pci_conf = pci_dev->config;

    pci_set_word(pci_conf + PCI_STATUS,
                 PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MEDIUM);

    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, 0x0);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, 0x0);

    pci_conf[PCI_INTERRUPT_PIN] = 1; /* interrupt pin A */
    pci_conf[PCI_MIN_GNT] = 0x06;
    pci_conf[PCI_MAX_LAT] = 0xff;

    /* Handler for memory-mapped I/O */
    memory_region_init_io(&this->state.mmio, OBJECT(this), &pcnet_mmio_ops, s,
                          "pcnet-mmio", PCNET_PNPMMIO_SIZE);

    memory_region_init_io(&this->io_bar, OBJECT(this), &pcnet_io_ops, s, "pcnet-io",
                          PCNET_IOPORT_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &this->io_bar);

    pci_register_bar(pci_dev, 1, 0, &s->mmio);

    s->irq = pci_allocate_irq(pci_dev);
    s->phys_mem_read = PCIPCNetState::pciPhysicalMemoryRead;
    s->phys_mem_write = PCIPCNetState::pciPhysicalMemoryWrite;
    s->dma_opaque = DEVICE(pci_dev);

    pcnet_common_init(DEVICE(pci_dev), s, &net_pci_pcnet_info);
}

static void pci_reset(DeviceState *dev)
{
    PCIPCNetState *d = PCI_PCNET(dev);
    d->reset();
}

void PCIPCNetState::reset()
{
    pcnet_h_reset(&this->state);
}

static void pcnet_instance_init(Object *obj)
{
    PCIPCNetState *d = PCI_PCNET(obj);
    d->instanceInit();
}

void PCIPCNetState::instanceInit()
{
    PCNetState *s = &this->state;

    device_add_bootindex_property(OBJECT(this), &s->conf.bootindex,
                                  "bootindex", "/ethernet-phy@0",
                                  DEVICE(this));
}

static const Property pcnet_properties[] = {
    DEFINE_NIC_PROPERTIES(PCIPCNetState, state.conf),
};

void PCIPCNetState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pci_pcnet_realize;
    k->exit = pci_pcnet_uninit;
    k->romfile = "efi-pcnet.rom",
    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = PCI_DEVICE_ID_AMD_LANCE;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    device_class_set_legacy_reset(dc, pci_reset);
    dc->vmsd = &vmstate_pci_pcnet;
    device_class_set_props(dc, pcnet_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const InterfaceInfo pcnet_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo pcnet_info = {
    .name          = TYPE_PCI_PCNET,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIPCNetState),
    .instance_init = pcnet_instance_init,
    .class_init    = PCIPCNetState::classInit,
    .interfaces    = pcnet_interfaces,
};

static void pci_pcnet_register_types(void)
{
    type_register_static(&pcnet_info);
}

type_init(pci_pcnet_register_types)
