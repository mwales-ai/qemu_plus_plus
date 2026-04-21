/*
 * QEMU PREP PCI host
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2011-2013 Andreas Färber
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

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/qdev-properties.h"
#include "hw/intc/i8259.h"
#include "hw/irq.h"
#include "qom/object.h"
extern "C" {
#include "qemu/units.h"
#include "hw/pci/pci_bus.h"
#include "hw/or-irq.h"
}

#define TYPE_RAVEN_PCI_DEVICE "raven"
#define TYPE_RAVEN_PCI_HOST_BRIDGE "raven-pcihost"

OBJECT_DECLARE_SIMPLE_TYPE(PREPPCIState, RAVEN_PCI_HOST_BRIDGE)

struct PREPPCIState {
    PCIHostState parent_obj;

    OrIRQState *or_irq;
    qemu_irq pci_irqs[PCI_NUM_PINS];
    AddressSpace pci_io_as;
    MemoryRegion pci_io;
    MemoryRegion pci_io_non_contiguous;
    MemoryRegion pci_memory;
    MemoryRegion pci_intack;
    MemoryRegion bm;
    MemoryRegion bm_ram_alias;
    MemoryRegion bm_pci_memory_alias;
    AddressSpace bm_as;

    int contiguous_map;

    static inline uint32_t idselToAddr(hwaddr addr);
    static void mmcfgWrite(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size);
    static uint64_t mmcfgRead(void *opaque, hwaddr addr, unsigned int size);
    static uint64_t intackRead(void *opaque, hwaddr addr, unsigned int size);
    static void intackWrite(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size);
    inline hwaddr ioAddress(hwaddr addr);
    static uint64_t ioRead(void *opaque, hwaddr addr, unsigned int size);
    static void ioWrite(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size);
    static int mapIrq(PCIDevice *pci_dev, int irq_num);
    static void setIrq(void *opaque, int irq_num, int level);
    static AddressSpace *setIommu(PCIBus *bus, void *opaque, int devfn);
    static void changeGpio(void *opaque, int n, int level);

    void realize(Error **errp);
    void init();
    static void classInit(DeviceClass *dc);
    static void ravenPciClassInit(ObjectClass *klass, const void *data);
};

#define PCI_IO_BASE_ADDR    0x80000000  /* Physical address on main bus */

inline uint32_t PREPPCIState::idselToAddr(hwaddr addr)
{
    return (ctz16(addr >> 11) << 11) | (addr & 0x7ff);
}

void PREPPCIState::mmcfgWrite(void *opaque, hwaddr addr, uint64_t val,
                               unsigned int size)
{
    PCIBus *hbus = static_cast<PCIBus *>(opaque);

    pci_data_write(hbus, idselToAddr(addr), val, size);
}

uint64_t PREPPCIState::mmcfgRead(void *opaque, hwaddr addr, unsigned int size)
{
    PCIBus *hbus = static_cast<PCIBus *>(opaque);

    return pci_data_read(hbus, idselToAddr(addr), size);
}

static const MemoryRegionOps raven_mmcfg_ops = {
    .read = PREPPCIState::mmcfgRead,
    .write = PREPPCIState::mmcfgWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

uint64_t PREPPCIState::intackRead(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    return pic_read_irq(isa_pic);
}

void PREPPCIState::intackWrite(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s not implemented\n", __func__);
}

static MemoryRegionOps raven_intack_ops = {
    .read = PREPPCIState::intackRead,
    .write = PREPPCIState::intackWrite,
};

static void raven_intack_ops_init(void) __attribute__((constructor));
static void raven_intack_ops_init(void)
{
    raven_intack_ops.valid.max_access_size = 1;
}

hwaddr PREPPCIState::ioAddress(hwaddr addr)
{
    if (contiguous_map == 0) {
        /* 64 KB contiguous space for IOs */
        addr &= 0xFFFF;
    } else {
        /* 8 MB non-contiguous space for IOs */
        addr = (addr & 0x1F) | ((addr & 0x007FFF000) >> 7);
    }

    /* FIXME: handle endianness switch */

    return addr;
}

uint64_t PREPPCIState::ioRead(void *opaque, hwaddr addr, unsigned int size)
{
    PREPPCIState *s = static_cast<PREPPCIState *>(opaque);
    uint8_t buf[4];

    addr = s->ioAddress(addr);
    address_space_read(&s->pci_io_as, addr + PCI_IO_BASE_ADDR,
                       MEMTXATTRS_UNSPECIFIED, buf, size);

    if (size == 1) {
        return buf[0];
    } else if (size == 2) {
        return lduw_le_p(buf);
    } else if (size == 4) {
        return ldl_le_p(buf);
    } else {
        g_assert_not_reached();
    }
}

void PREPPCIState::ioWrite(void *opaque, hwaddr addr,
                            uint64_t val, unsigned int size)
{
    PREPPCIState *s = static_cast<PREPPCIState *>(opaque);
    uint8_t buf[4];

    addr = s->ioAddress(addr);

    if (size == 1) {
        buf[0] = val;
    } else if (size == 2) {
        stw_le_p(buf, val);
    } else if (size == 4) {
        stl_le_p(buf, val);
    } else {
        g_assert_not_reached();
    }

    address_space_write(&s->pci_io_as, addr + PCI_IO_BASE_ADDR,
                        MEMTXATTRS_UNSPECIFIED, buf, size);
}

static MemoryRegionOps raven_io_ops = {
    .read = PREPPCIState::ioRead,
    .write = PREPPCIState::ioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void raven_io_ops_init(void) __attribute__((constructor));
static void raven_io_ops_init(void)
{
    raven_io_ops.impl.max_access_size = 4;
    raven_io_ops.impl.unaligned = true;
    raven_io_ops.valid.unaligned = true;
}

int PREPPCIState::mapIrq(PCIDevice *pci_dev, int irq_num)
{
    return (irq_num + (pci_dev->devfn >> 3)) & 1;
}

void PREPPCIState::setIrq(void *opaque, int irq_num, int level)
{
    PREPPCIState *s = static_cast<PREPPCIState *>(opaque);

    qemu_set_irq(s->pci_irqs[irq_num], level);
}

AddressSpace *PREPPCIState::setIommu(PCIBus *bus, void *opaque, int devfn)
{
    PREPPCIState *s = static_cast<PREPPCIState *>(opaque);

    return &s->bm_as;
}

static const PCIIOMMUOps raven_iommu_ops = {
    .get_address_space = PREPPCIState::setIommu,
};

void PREPPCIState::changeGpio(void *opaque, int n, int level)
{
    PREPPCIState *s = static_cast<PREPPCIState *>(opaque);

    s->contiguous_map = level;
}

void PREPPCIState::realize(Error **errp)
{
    DeviceState *d = reinterpret_cast<DeviceState *>(this);
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(d);
    PCIHostState *h = reinterpret_cast<PCIHostState *>(dev);
    MemoryRegion *address_space_mem = get_system_memory();
    int i;

    /*
     * According to PReP specification section 6.1.6 "System Interrupt
     * Assignments", all PCI interrupts are routed via IRQ 15
     */
    or_irq = reinterpret_cast<OrIRQState *>(object_new(TYPE_OR_IRQ));
    object_property_set_int(reinterpret_cast<Object *>(or_irq), "num-lines", PCI_NUM_PINS,
                            &error_fatal);
    qdev_realize(reinterpret_cast<DeviceState *>(or_irq), NULL, &error_fatal);
    sysbus_init_irq(dev, &or_irq->out_irq);

    for (i = 0; i < PCI_NUM_PINS; i++) {
        pci_irqs[i] = qdev_get_gpio_in(reinterpret_cast<DeviceState *>(or_irq), i);
    }

    qdev_init_gpio_in(d, changeGpio, 1);

    h->bus = pci_register_root_bus(d, NULL, setIrq, mapIrq,
                                   this, &pci_memory, &pci_io, 0, 4,
                                   TYPE_PCI_BUS);

    memory_region_init_io(&h->conf_mem, reinterpret_cast<Object *>(h), &pci_host_conf_le_ops, this,
                          "pci-conf-idx", 4);
    memory_region_add_subregion(&pci_io, 0xcf8, &h->conf_mem);

    memory_region_init_io(&h->data_mem, reinterpret_cast<Object *>(h), &pci_host_data_le_ops, this,
                          "pci-conf-data", 4);
    memory_region_add_subregion(&pci_io, 0xcfc, &h->data_mem);

    memory_region_init_io(&h->mmcfg, reinterpret_cast<Object *>(h), &raven_mmcfg_ops, h->bus,
                          "pci-mmcfg", 0x00400000);
    memory_region_add_subregion(address_space_mem, 0x80800000, &h->mmcfg);

    memory_region_init_io(&pci_intack, reinterpret_cast<Object *>(this), &raven_intack_ops, this,
                          "pci-intack", 1);
    memory_region_add_subregion(address_space_mem, 0xbffffff0, &pci_intack);

    pci_create_simple(h->bus, PCI_DEVFN(0, 0), TYPE_RAVEN_PCI_DEVICE);

    address_space_init(&bm_as, &bm, "raven-bm");
    pci_setup_iommu(h->bus, &raven_iommu_ops, this);
}

void PREPPCIState::init()
{
    Object *obj = reinterpret_cast<Object *>(this);
    MemoryRegion *address_space_mem = get_system_memory();

    memory_region_init(&pci_io, obj, "pci-io", 0x3f800000);
    memory_region_init_io(&pci_io_non_contiguous, obj, &raven_io_ops, this,
                          "pci-io-non-contiguous", 0x00800000);
    memory_region_init(&pci_memory, obj, "pci-memory", 0x3f000000);
    address_space_init(&pci_io_as, &pci_io, "raven-io");

    /*
     * Raven's raven_io_ops use the address-space API to access pci-conf-idx
     * (which is also owned by the raven device). As such, mark the
     * pci_io_non_contiguous as re-entrancy safe.
     */
    pci_io_non_contiguous.disable_reentrancy_guard = true;

    /* CPU address space */
    memory_region_add_subregion(address_space_mem, PCI_IO_BASE_ADDR,
                                &pci_io);
    memory_region_add_subregion_overlap(address_space_mem, PCI_IO_BASE_ADDR,
                                        &pci_io_non_contiguous, 1);
    memory_region_add_subregion(address_space_mem, 0xc0000000, &pci_memory);

    /* Bus master address space */
    memory_region_init(&bm, obj, "bm-raven", 4 * GiB);
    memory_region_init_alias(&bm_pci_memory_alias, obj, "bm-pci-memory",
                             &pci_memory, 0,
                             memory_region_size(&pci_memory));
    memory_region_init_alias(&bm_ram_alias, obj, "bm-system",
                             get_system_memory(), 0, 0x80000000);
    memory_region_add_subregion(&bm, 0         , &bm_pci_memory_alias);
    memory_region_add_subregion(&bm, 0x80000000, &bm_ram_alias);
}

void PREPPCIState::classInit(DeviceClass *dc)
{
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";
}

static void raven_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    d->config[PCI_CAPABILITY_LIST] = 0x00;
}

void PREPPCIState::ravenPciClassInit(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    k->realize = raven_realize;
    k->vendor_id = PCI_VENDOR_ID_MOTOROLA;
    k->device_id = PCI_DEVICE_ID_MOTOROLA_RAVEN;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "PReP Host Bridge - Motorola Raven";
    /*
     * Reason: PCI-facing part of the host bridge, not usable without
     * the host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const InterfaceInfo raven_pci_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static void __attribute__((constructor)) register_raven_pci_type(void)
{
    static TypeInfo raven_pci_info = {
        .name = TYPE_RAVEN_PCI_DEVICE,
        .parent = TYPE_PCI_DEVICE,
        .class_init = PREPPCIState::ravenPciClassInit,
        .interfaces = raven_pci_interfaces,
    };
    type_register_static(&raven_pci_info);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(PREPPCIState, TYPE_RAVEN_PCI_HOST_BRIDGE, TYPE_PCI_HOST_BRIDGE)
