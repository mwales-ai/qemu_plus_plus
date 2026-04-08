/*
 * QEMU TEWS TPCI200 IndustryPack carrier emulation
 *
 * Copyright (C) 2012 Igalia, S.L.
 * Author: Alberto Garcia <berto@igalia.com>
 *
 * This code is licensed under the GNU GPL v2 or (at your option) any
 * later version.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/units.h"
#include "hw/ipack/ipack.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qom/object.h"

/* #define DEBUG_TPCI */

#ifdef DEBUG_TPCI
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "TPCI200: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define N_MODULES 4

#define IP_ID_SPACE  2
#define IP_INT_SPACE 3
#define IP_IO_SPACE_ADDR_MASK  0x7F
#define IP_ID_SPACE_ADDR_MASK  0x3F
#define IP_INT_SPACE_ADDR_MASK 0x3F

#define STATUS_INT(IP, INTNO) BIT((IP) * 2 + (INTNO))
#define STATUS_TIME(IP)       BIT((IP) + 12)
#define STATUS_ERR_ANY        0xF00

#define CTRL_CLKRATE          BIT(0)
#define CTRL_RECOVER          BIT(1)
#define CTRL_TIME_INT         BIT(2)
#define CTRL_ERR_INT          BIT(3)
#define CTRL_INT_EDGE(INTNO)  BIT(4 + (INTNO))
#define CTRL_INT(INTNO)       BIT(6 + (INTNO))

#define REG_REV_ID    0x00
#define REG_IP_A_CTRL 0x02
#define REG_IP_B_CTRL 0x04
#define REG_IP_C_CTRL 0x06
#define REG_IP_D_CTRL 0x08
#define REG_RESET     0x0A
#define REG_STATUS    0x0C
#define IP_N_FROM_REG(REG) ((REG) / 2 - 1)

struct TPCI200State {
    PCIDevice dev;
    IPackBus bus;
    MemoryRegion mmio;
    MemoryRegion io;
    MemoryRegion las0;
    MemoryRegion las1;
    MemoryRegion las2;
    MemoryRegion las3;
    bool big_endian[3];
    uint8_t ctrl[N_MODULES];
    uint16_t status;
    uint8_t int_set;

    /* methods */
    static void setIrq(void *opaque, int intno, int level);
    static uint64_t readCfg(void *opaque, hwaddr addr, unsigned size);
    static void writeCfg(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t readLas0(void *opaque, hwaddr addr, unsigned size);
    static void writeLas0(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t readLas1(void *opaque, hwaddr addr, unsigned size);
    static void writeLas1(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t readLas2(void *opaque, hwaddr addr, unsigned size);
    static void writeLas2(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t readLas3(void *opaque, hwaddr addr, unsigned size);
    static void writeLas3(void *opaque, hwaddr addr, uint64_t val, unsigned size);

    void realize(Error **errp);
    static void realizeWrapper(PCIDevice *pci_dev, Error **errp);

    static void classInit(ObjectClass *klass, const void *data);
};

#define TYPE_TPCI200 "tpci200"

OBJECT_DECLARE_SIMPLE_TYPE(TPCI200State, TPCI200)

static const uint8_t local_config_regs[] = {
    0x00, 0xFF, 0xFF, 0x0F, 0x00, 0xFC, 0xFF, 0x0F, 0x00, 0x00, 0x00,
    0x0E, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x08, 0x01, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x60, 0x41, 0xD4,
    0xA2, 0x20, 0x41, 0x14, 0xA2, 0x20, 0x41, 0x14, 0xA2, 0x20, 0x01,
    0x14, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0x08, 0x01, 0x02,
    0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x80, 0x02, 0x41,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x7A, 0x00, 0x52, 0x92, 0x24, 0x02
};

static void adjust_addr(bool big_endian, hwaddr *addr, unsigned size)
{
    if (big_endian && size == 1) {
        *addr ^= 1;
    }
}

static uint64_t adjust_value(bool big_endian, uint64_t *val, unsigned size)
{
    if (big_endian && size == 2) {
        *val = bswap16(*val);
    }
    return *val;
}

void TPCI200State::setIrq(void *opaque, int intno, int level)
{
    IPackDevice *ip = static_cast<IPackDevice *>(opaque);
    IPackBus *bus = reinterpret_cast<IPackBus *>(qdev_get_parent_bus(reinterpret_cast<DeviceState *>(ip)));
    PCIDevice *pcidev = reinterpret_cast<PCIDevice *>(reinterpret_cast<BusState *>(bus)->parent);
    TPCI200State *dev = reinterpret_cast<TPCI200State *>(pcidev);
    unsigned ip_n = ip->slot;
    uint16_t prev_status = dev->status;

    assert(ip->slot >= 0 && ip->slot < N_MODULES);

    if (!(dev->ctrl[ip_n] & CTRL_INT(intno))) {
        return;
    }

    if (level) {
        dev->status |=  STATUS_INT(ip_n, intno);
    } else {
        dev->status &= ~STATUS_INT(ip_n, intno);
    }

    if (dev->status == prev_status) {
        return;
    }

    DPRINTF("IP %u INT%u#: %u\n", ip_n, intno, level);

    if (dev->ctrl[ip_n] & CTRL_INT_EDGE(intno)) {
        if (level) {
            pci_set_irq(&dev->dev, !dev->int_set);
            pci_set_irq(&dev->dev,  dev->int_set);
        }
    } else {
        unsigned i, j;
        uint16_t level_status = dev->status;

        for (i = 0; i < N_MODULES; i++) {
            for (j = 0; j < 2; j++) {
                if (dev->ctrl[i] & CTRL_INT_EDGE(j)) {
                    level_status &= ~STATUS_INT(i, j);
                }
            }
        }

        if (level_status && !dev->int_set) {
            pci_irq_assert(&dev->dev);
            dev->int_set = 1;
        } else if (!level_status && dev->int_set) {
            pci_irq_deassert(&dev->dev);
            dev->int_set = 0;
        }
    }
}

uint64_t TPCI200State::readCfg(void *opaque, hwaddr addr, unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    uint8_t ret = 0;
    if (addr < ARRAY_SIZE(local_config_regs)) {
        ret = local_config_regs[addr];
    }
    if ((addr == 0x2b && s->big_endian[0]) ||
        (addr == 0x2f && s->big_endian[1]) ||
        (addr == 0x33 && s->big_endian[2])) {
        ret |= 1;
    }
    DPRINTF("Read from LCR 0x%x: 0x%x\n", (unsigned) addr, (unsigned) ret);
    return ret;
}

void TPCI200State::writeCfg(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    if (addr == 0x2b || addr == 0x2f || addr == 0x33) {
        unsigned las = (addr - 0x2b) / 4;
        s->big_endian[las] = val & 1;
        DPRINTF("LAS%u big endian mode: %u\n", las, (unsigned) val & 1);
    } else {
        DPRINTF("Write to LCR 0x%x: 0x%x\n", (unsigned) addr, (unsigned) val);
    }
}

uint64_t TPCI200State::readLas0(void *opaque, hwaddr addr, unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    uint64_t ret = 0;

    switch (addr) {
    case REG_REV_ID:
        DPRINTF("Read REVISION ID\n");
        break;
    case REG_IP_A_CTRL:
    case REG_IP_B_CTRL:
    case REG_IP_C_CTRL:
    case REG_IP_D_CTRL:
        {
            unsigned ip_n = IP_N_FROM_REG(addr);
            ret = s->ctrl[ip_n];
            DPRINTF("Read IP %c CONTROL: 0x%x\n", 'A' + ip_n, (unsigned) ret);
        }
        break;
    case REG_RESET:
        DPRINTF("Read RESET\n");
        break;
    case REG_STATUS:
        ret = s->status;
        DPRINTF("Read STATUS: 0x%x\n", (unsigned) ret);
        break;
    default:
        DPRINTF("Unsupported read from LAS0 0x%x\n", (unsigned) addr);
        break;
    }

    return adjust_value(s->big_endian[0], &ret, size);
}

void TPCI200State::writeLas0(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);

    adjust_value(s->big_endian[0], &val, size);

    switch (addr) {
    case REG_REV_ID:
        DPRINTF("Write Revision ID: 0x%x\n", (unsigned) val);
        break;
    case REG_IP_A_CTRL:
    case REG_IP_B_CTRL:
    case REG_IP_C_CTRL:
    case REG_IP_D_CTRL:
        {
            unsigned ip_n = IP_N_FROM_REG(addr);
            s->ctrl[ip_n] = val;
            DPRINTF("Write IP %c CONTROL: 0x%x\n", 'A' + ip_n, (unsigned) val);
        }
        break;
    case REG_RESET:
        DPRINTF("Write RESET: 0x%x\n", (unsigned) val);
        break;
    case REG_STATUS:
        {
            unsigned i;
            for (i = 0; i < N_MODULES; i++) {
                IPackDevice *ip = ipack_device_find(&s->bus, i);
                if (ip != NULL) {
                    if (val & STATUS_INT(i, 0)) {
                        DPRINTF("Clear IP %c INT0# status\n", 'A' + i);
                        qemu_irq_lower(&ip->irq[0]);
                    }
                    if (val & STATUS_INT(i, 1)) {
                        DPRINTF("Clear IP %c INT1# status\n", 'A' + i);
                        qemu_irq_lower(&ip->irq[1]);
                    }
                }
                if (val & STATUS_TIME(i)) {
                    DPRINTF("Clear IP %c timeout\n", 'A' + i);
                    s->status &= ~STATUS_TIME(i);
                }
            }
            if (val & STATUS_ERR_ANY) {
                DPRINTF("Unexpected write to STATUS register: 0x%x\n",
                        (unsigned) val);
            }
        }
        break;
    default:
        DPRINTF("Unsupported write to LAS0 0x%x: 0x%x\n",
                (unsigned) addr, (unsigned) val);
        break;
    }
}

uint64_t TPCI200State::readLas1(void *opaque, hwaddr addr, unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    IPackDevice *ip;
    uint64_t ret = 0;
    unsigned ip_n, space;
    uint8_t offset;

    adjust_addr(s->big_endian[1], &addr, size);

    ip_n = addr >> 8;
    space = (addr >> 6) & 3;
    ip = ipack_device_find(&s->bus, ip_n);

    if (ip == NULL) {
        DPRINTF("Read LAS1: IP module %u not installed\n", ip_n);
    } else {
        IPackDeviceClass *k = IPACK_DEVICE_GET_CLASS(ip);
        switch (space) {
        case IP_ID_SPACE:
            offset = addr & IP_ID_SPACE_ADDR_MASK;
            if (k->id_read) {
                ret = k->id_read(ip, offset);
            }
            break;
        case IP_INT_SPACE:
            offset = addr & IP_INT_SPACE_ADDR_MASK;
            if (offset == 0 || offset == 2) {
                unsigned intno = offset / 2;
                bool int_set = s->status & STATUS_INT(ip_n, intno);
                bool int_edge_sensitive = s->ctrl[ip_n] & CTRL_INT_EDGE(intno);
                if (int_set && !int_edge_sensitive) {
                    qemu_irq_lower(&ip->irq[intno]);
                }
            }
            if (k->int_read) {
                ret = k->int_read(ip, offset);
            }
            break;
        default:
            offset = addr & IP_IO_SPACE_ADDR_MASK;
            if (k->io_read) {
                ret = k->io_read(ip, offset);
            }
            break;
        }
    }

    return adjust_value(s->big_endian[1], &ret, size);
}

void TPCI200State::writeLas1(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    IPackDevice *ip;
    unsigned ip_n, space;
    uint8_t offset;

    adjust_addr(s->big_endian[1], &addr, size);
    adjust_value(s->big_endian[1], &val, size);

    ip_n = addr >> 8;
    space = (addr >> 6) & 3;
    ip = ipack_device_find(&s->bus, ip_n);

    if (ip == NULL) {
        DPRINTF("Write LAS1: IP module %u not installed\n", ip_n);
    } else {
        IPackDeviceClass *k = IPACK_DEVICE_GET_CLASS(ip);
        switch (space) {
        case IP_ID_SPACE:
            offset = addr & IP_ID_SPACE_ADDR_MASK;
            if (k->id_write) {
                k->id_write(ip, offset, val);
            }
            break;
        case IP_INT_SPACE:
            offset = addr & IP_INT_SPACE_ADDR_MASK;
            if (k->int_write) {
                k->int_write(ip, offset, val);
            }
            break;
        default:
            offset = addr & IP_IO_SPACE_ADDR_MASK;
            if (k->io_write) {
                k->io_write(ip, offset, val);
            }
            break;
        }
    }
}

uint64_t TPCI200State::readLas2(void *opaque, hwaddr addr, unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    IPackDevice *ip;
    uint64_t ret = 0;
    unsigned ip_n;
    uint32_t offset;

    adjust_addr(s->big_endian[2], &addr, size);

    ip_n = addr >> 23;
    offset = addr & 0x7fffff;
    ip = ipack_device_find(&s->bus, ip_n);

    if (ip == NULL) {
        DPRINTF("Read LAS2: IP module %u not installed\n", ip_n);
    } else {
        IPackDeviceClass *k = IPACK_DEVICE_GET_CLASS(ip);
        if (k->mem_read16) {
            ret = k->mem_read16(ip, offset);
        }
    }

    return adjust_value(s->big_endian[2], &ret, size);
}

void TPCI200State::writeLas2(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    IPackDevice *ip;
    unsigned ip_n;
    uint32_t offset;

    adjust_addr(s->big_endian[2], &addr, size);
    adjust_value(s->big_endian[2], &val, size);

    ip_n = addr >> 23;
    offset = addr & 0x7fffff;
    ip = ipack_device_find(&s->bus, ip_n);

    if (ip == NULL) {
        DPRINTF("Write LAS2: IP module %u not installed\n", ip_n);
    } else {
        IPackDeviceClass *k = IPACK_DEVICE_GET_CLASS(ip);
        if (k->mem_write16) {
            k->mem_write16(ip, offset, val);
        }
    }
}

uint64_t TPCI200State::readLas3(void *opaque, hwaddr addr, unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    IPackDevice *ip;
    uint64_t ret = 0;
    unsigned ip_n = addr >> 22;
    uint32_t offset = addr & 0x3fffff;

    ip = ipack_device_find(&s->bus, ip_n);

    if (ip == NULL) {
        DPRINTF("Read LAS3: IP module %u not installed\n", ip_n);
    } else {
        IPackDeviceClass *k = IPACK_DEVICE_GET_CLASS(ip);
        if (k->mem_read8) {
            ret = k->mem_read8(ip, offset);
        }
    }

    return ret;
}

void TPCI200State::writeLas3(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    TPCI200State *s = static_cast<TPCI200State *>(opaque);
    IPackDevice *ip;
    unsigned ip_n = addr >> 22;
    uint32_t offset = addr & 0x3fffff;

    ip = ipack_device_find(&s->bus, ip_n);

    if (ip == NULL) {
        DPRINTF("Write LAS3: IP module %u not installed\n", ip_n);
    } else {
        IPackDeviceClass *k = IPACK_DEVICE_GET_CLASS(ip);
        if (k->mem_write8) {
            k->mem_write8(ip, offset, val);
        }
    }
}

static const MemoryRegionOps tpci200_cfg_ops = {
    .read = TPCI200State::readCfg,
    .write = TPCI200State::writeCfg,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid =  {
        .min_access_size = 1,
        .max_access_size = 4
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1
    }
};

static const MemoryRegionOps tpci200_las0_ops = {
    .read = TPCI200State::readLas0,
    .write = TPCI200State::writeLas0,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid =  {
        .min_access_size = 2,
        .max_access_size = 2
    }
};

static const MemoryRegionOps tpci200_las1_ops = {
    .read = TPCI200State::readLas1,
    .write = TPCI200State::writeLas1,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid =  {
        .min_access_size = 1,
        .max_access_size = 2
    }
};

static const MemoryRegionOps tpci200_las2_ops = {
    .read = TPCI200State::readLas2,
    .write = TPCI200State::writeLas2,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid =  {
        .min_access_size = 1,
        .max_access_size = 2
    }
};

static const MemoryRegionOps tpci200_las3_ops = {
    .read = TPCI200State::readLas3,
    .write = TPCI200State::writeLas3,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid =  {
        .min_access_size = 1,
        .max_access_size = 1
    }
};

void TPCI200State::realizeWrapper(PCIDevice *pci_dev, Error **errp)
{
    TPCI200State *s = reinterpret_cast<TPCI200State *>(pci_dev);
    s->realize(errp);
}

void TPCI200State::realize(Error **errp)
{
    uint8_t *c = dev.config;

    pci_set_word(c + PCI_COMMAND, 0x0003);
    pci_set_word(c + PCI_STATUS,  0x0280);

    pci_set_byte(c + PCI_INTERRUPT_PIN, 0x01);

    pci_set_byte(c + PCI_CAPABILITY_LIST, 0x40);
    pci_set_long(c + 0x40, 0x48014801);
    pci_set_long(c + 0x48, 0x00024C06);
    pci_set_long(c + 0x4C, 0x00000003);

    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this), &tpci200_cfg_ops,
                          this, "tpci200_mmio", 128);
    memory_region_init_io(&io, reinterpret_cast<Object *>(this),   &tpci200_cfg_ops,
                          this, "tpci200_io",   128);
    memory_region_init_io(&las0, reinterpret_cast<Object *>(this), &tpci200_las0_ops,
                          this, "tpci200_las0", 256);
    memory_region_init_io(&las1, reinterpret_cast<Object *>(this), &tpci200_las1_ops,
                          this, "tpci200_las1", 1024);
    memory_region_init_io(&las2, reinterpret_cast<Object *>(this), &tpci200_las2_ops,
                          this, "tpci200_las2", 32 * MiB);
    memory_region_init_io(&las3, reinterpret_cast<Object *>(this), &tpci200_las3_ops,
                          this, "tpci200_las3", 16 * MiB);
    pci_register_bar(&dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &mmio);
    pci_register_bar(&dev, 1, PCI_BASE_ADDRESS_SPACE_IO,     &io);
    pci_register_bar(&dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &las0);
    pci_register_bar(&dev, 3, PCI_BASE_ADDRESS_SPACE_MEMORY, &las1);
    pci_register_bar(&dev, 4, PCI_BASE_ADDRESS_SPACE_MEMORY, &las2);
    pci_register_bar(&dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &las3);

    ipack_bus_init(&bus, sizeof(bus), reinterpret_cast<DeviceState *>(&dev),
                   N_MODULES, TPCI200State::setIrq);
}

static const VMStateDescription vmstate_tpci200 = {
    .name = "tpci200",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, TPCI200State),
        VMSTATE_BOOL_ARRAY(big_endian, TPCI200State, 3),
        VMSTATE_UINT8_ARRAY(ctrl, TPCI200State, N_MODULES),
        VMSTATE_UINT16(status, TPCI200State),
        VMSTATE_UINT8(int_set, TPCI200State),
        VMSTATE_END_OF_LIST()
    }
};

void TPCI200State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->realize = TPCI200State::realizeWrapper;
    k->vendor_id = PCI_VENDOR_ID_TEWS;
    k->device_id = PCI_DEVICE_ID_TEWS_TPCI200;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
    k->subsystem_vendor_id = PCI_VENDOR_ID_TEWS;
    k->subsystem_id = 0x300A;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "TEWS TPCI200 IndustryPack carrier";
    dc->vmsd = &vmstate_tpci200;
}

static const InterfaceInfo tpci200_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo tpci200_info = {
    .name          = TYPE_TPCI200,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(TPCI200State),
    .class_init    = TPCI200State::classInit,
    .interfaces    = tpci200_interfaces,
};

static void tpci200_register_types(void)
{
    type_register_static(&tpci200_info);
}

type_init(tpci200_register_types)
