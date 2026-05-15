/*
 * Mai Logic Articia S emulation
 *
 * Copyright (c) 2023 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/irq.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/intc/i8259.h"
extern "C" {
#include "hw/pci-host/articia.h"
}

/*
 * This is a minimal emulation of this chip as used in AmigaOne board.
 * Most features are missing but those are not needed by firmware and guests.
 */

OBJECT_DECLARE_SIMPLE_TYPE(ArticiaState, ARTICIA)

OBJECT_DECLARE_SIMPLE_TYPE(ArticiaHostState, ARTICIA_PCI_HOST)
struct ArticiaHostState {
    PCIDevice parent_obj;

    ArticiaState *as;

    static void pciHostClassInit(ObjectClass *klass, const void *data);
    static void pciBridgeClassInit(ObjectClass *klass, const void *data);
};

/* TYPE_ARTICIA */

struct ArticiaState {
    PCIHostState parent_obj;

    qemu_irq irq[PCI_NUM_PINS];
    MemoryRegion io;
    MemoryRegion mem;
    MemoryRegion reg;

    bitbang_i2c_interface smbus;
    uint32_t gpio; /* bits 0-7 in, 8-15 out, 16-23 direction (0 in, 1 out) */
    hwaddr gpio_base;
    MemoryRegion gpio_reg;

    static uint64_t gpioRead(void *opaque, hwaddr addr, unsigned int size);
    static void gpioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned int size);
    static uint64_t regRead(void *opaque, hwaddr addr, unsigned int size);
    static void regWrite(void *opaque, hwaddr addr, uint64_t val,
                         unsigned int size);
    static void setIrq(void *opaque, int n, int level);
    static int bus0MapIrq(PCIDevice *pdev, int pin);

    void realize(Error **errp);
    static void classInit(DeviceClass *dc);
};

uint64_t ArticiaState::gpioRead(void *opaque, hwaddr addr, unsigned int size)
{
    ArticiaState *s = static_cast<ArticiaState *>(opaque);

    return (s->gpio >> (addr * 8)) & 0xff;
}

void ArticiaState::gpioWrite(void *opaque, hwaddr addr, uint64_t val,
                              unsigned int size)
{
    ArticiaState *s = static_cast<ArticiaState *>(opaque);
    uint32_t sh = addr * 8;

    if (addr == 0) {
        /* in bits read only? */
        return;
    }

    if ((s->gpio & (0xff << sh)) != (val & 0xff) << sh) {
        s->gpio &= ~(0xff << sh | 0xff);
        s->gpio |= (val & 0xff) << sh;
        s->gpio |= bitbang_i2c_set(&s->smbus, BITBANG_I2C_SDA,
                                   s->gpio & BIT(16) ?
                                   !!(s->gpio & BIT(8)) : 1);
        if ((s->gpio & BIT(17))) {
            s->gpio &= ~BIT(0);
            s->gpio |= bitbang_i2c_set(&s->smbus, BITBANG_I2C_SCL,
                                       !!(s->gpio & BIT(9)));
        }
    }
}

static MemoryRegionOps articia_gpio_ops = {
    .read = ArticiaState::gpioRead,
    .write = ArticiaState::gpioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void __attribute__((constructor)) init_articia_gpio_ops(void)
{
    articia_gpio_ops.valid.min_access_size = 1;
    articia_gpio_ops.valid.max_access_size = 1;
}

uint64_t ArticiaState::regRead(void *opaque, hwaddr addr, unsigned int size)
{
    ArticiaState *s = static_cast<ArticiaState *>(opaque);
    uint64_t ret = UINT_MAX;

    switch (addr) {
    case 0xc00cf8:
        ret = pci_host_conf_le_ops.read(reinterpret_cast<PCIHostState *>(s), 0, size);
        break;
    case 0xe00cfc ... 0xe00cff:
        ret = pci_host_data_le_ops.read(reinterpret_cast<PCIHostState *>(s), addr - 0xe00cfc, size);
        break;
    case 0xf00000:
        ret = pic_read_irq(isa_pic);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Unimplemented register read 0x%"
                      HWADDR_PRIx " %d\n", __func__, addr, size);
        break;
    }
    return ret;
}

void ArticiaState::regWrite(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    ArticiaState *s = static_cast<ArticiaState *>(opaque);

    switch (addr) {
    case 0xc00cf8:
        pci_host_conf_le_ops.write(reinterpret_cast<PCIHostState *>(s), 0, val, size);
        break;
    case 0xe00cfc ... 0xe00cff:
        pci_host_data_le_ops.write(reinterpret_cast<PCIHostState *>(s), addr, val, size);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Unimplemented register write 0x%"
                      HWADDR_PRIx " %d <- %" PRIx64 "\n", __func__, addr, size, val);
        break;
    }
}

static MemoryRegionOps articia_reg_ops = {
    .read = ArticiaState::regRead,
    .write = ArticiaState::regWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void __attribute__((constructor)) init_articia_reg_ops(void)
{
    articia_reg_ops.valid.min_access_size = 1;
    articia_reg_ops.valid.max_access_size = 4;
}

void ArticiaState::setIrq(void *opaque, int n, int level)
{
    ArticiaState *s = static_cast<ArticiaState *>(opaque);
    qemu_set_irq(s->irq[n], level);
}

/*
 * AmigaOne SE PCI slot to IRQ routing
 *
 * repository: https://source.denx.de/u-boot/custodians/u-boot-avr32.git
 * refspec: v2010.06
 * file: board/MAI/AmigaOneG3SE/articiaS_pci.c
 */
int ArticiaState::bus0MapIrq(PCIDevice *pdev, int pin)
{
    int devfn_slot = PCI_SLOT(pdev->devfn);

    switch (devfn_slot) {
    case 6:  /* On board ethernet */
        return 3;
    case 7:  /* South bridge */
        return pin;
    default: /* PCI Slot 1 Devfn slot 8, Slot 2 Devfn 9, Slot 3 Devfn 10 */
        return pci_swizzle(devfn_slot, pin);
    }

}

void ArticiaState::realize(Error **errp)
{
    DeviceState *dev = reinterpret_cast<DeviceState *>(this);
    PCIHostState *h = reinterpret_cast<PCIHostState *>(dev);
    PCIDevice *pdev;

    bitbang_i2c_init(&smbus, i2c_init_bus(dev, "smbus"));
    memory_region_init_io(&gpio_reg, reinterpret_cast<Object *>(this), &articia_gpio_ops, this,
                          TYPE_ARTICIA, 4);

    memory_region_init(&mem, reinterpret_cast<Object *>(dev), "pci-mem", UINT64_MAX);
    memory_region_init(&io, reinterpret_cast<Object *>(dev), "pci-io", 0xc00000);
    memory_region_init_io(&reg, reinterpret_cast<Object *>(this), &articia_reg_ops, this,
                          TYPE_ARTICIA, 0x1000000);
    memory_region_add_subregion_overlap(&reg, 0, &io, 1);

    /* devfn_min is 8 that matches first PCI slot in AmigaOne */
    h->bus = pci_register_root_bus(dev, NULL, setIrq,
                                   bus0MapIrq, dev, &mem,
                                   &io, PCI_DEVFN(8, 0), 4, TYPE_PCI_BUS);
    pdev = pci_create_simple_multifunction(h->bus, PCI_DEVFN(0, 0),
                                           TYPE_ARTICIA_PCI_HOST);
    reinterpret_cast<ArticiaHostState *>(pdev)->as = this;
    pci_create_simple(h->bus, PCI_DEVFN(0, 1), TYPE_ARTICIA_PCI_BRIDGE);

    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(dev), &reg);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(dev), &mem);
    qdev_init_gpio_out(dev, irq, ARRAY_SIZE(irq));
}

void ArticiaState::classInit(DeviceClass *dc)
{
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

/* TYPE_ARTICIA_PCI_HOST */

static void articia_pci_host_cfg_write(PCIDevice *d, uint32_t addr,
                                       uint32_t val, int len)
{
    ArticiaState *s = reinterpret_cast<ArticiaHostState *>(d)->as;

    pci_default_write_config(d, addr, val, len);
    switch (addr) {
    case 0x40:
        s->gpio_base = val;
        break;
    case 0x44:
        if (val != 0x11) {
            /* FIXME what do the bits actually mean? */
            break;
        }
        if (memory_region_is_mapped(&s->gpio_reg)) {
            memory_region_del_subregion(&s->io, &s->gpio_reg);
        }
        memory_region_add_subregion(&s->io, s->gpio_base + 0x38, &s->gpio_reg);
        break;
    }
}

void ArticiaHostState::pciHostClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->config_write = articia_pci_host_cfg_write;
    k->vendor_id = 0x10cc;
    k->device_id = 0x0660;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge,
     * not usable without the host-facing part
     */
    dc->user_creatable = false;
}

/* TYPE_ARTICIA_PCI_BRIDGE */

void ArticiaHostState::pciBridgeClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->vendor_id = 0x10cc;
    k->device_id = 0x0661;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge,
     * not usable without the host-facing part
     */
    dc->user_creatable = false;
}

static const InterfaceInfo articia_pci_host_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const InterfaceInfo articia_pci_bridge_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED_IFACES(articia_pci_host, ArticiaHostState,
                                              TYPE_ARTICIA_PCI_HOST,
                                              TYPE_PCI_DEVICE,
                                              ArticiaHostState::pciHostClassInit,
                                              articia_pci_host_interfaces)

REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED_IFACES(articia_pci_bridge, PCIDevice,
                                              TYPE_ARTICIA_PCI_BRIDGE,
                                              TYPE_PCI_DEVICE,
                                              ArticiaHostState::pciBridgeClassInit,
                                              articia_pci_bridge_interfaces)

REGISTER_QEMU_DEVICE(ArticiaState, TYPE_ARTICIA, TYPE_PCI_HOST_BRIDGE)
