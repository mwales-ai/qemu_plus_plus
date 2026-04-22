/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright IBM Corp. 2008
 *
 * Authors: Hollis Blanchard <hollisb@us.ibm.com>
 */

/*
 * This file implements emulation of the 32-bit PCI controller found in some
 * 4xx SoCs, such as the 440EP.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/pci-host/ppc4xx.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "trace.h"
#include "qom/object.h"
#include "qom/cpp/object.h"

struct PCIMasterMap {
    uint32_t la;
    uint32_t ma;
    uint32_t pcila;
    uint32_t pciha;
};

struct PCITargetMap {
    uint32_t ms;
    uint32_t la;
};

OBJECT_DECLARE_SIMPLE_TYPE(PPC4xxPCIState, PPC4xx_PCI_HOST)

#define PPC4xx_PCI_NR_PMMS 3
#define PPC4xx_PCI_NR_PTMS 2

#define PPC4xx_PCI_NUM_DEVS 5

struct PPC4xxPCIState {
    PCIHostState parent_obj;

    struct PCIMasterMap pmm[PPC4xx_PCI_NR_PMMS];
    struct PCITargetMap ptm[PPC4xx_PCI_NR_PTMS];
    qemu_irq irq[PPC4xx_PCI_NUM_DEVS];

    MemoryRegion container;
    MemoryRegion iomem;

    /* methods */
    static uint64_t regRead4(void *opaque, hwaddr offset, unsigned size);
    static void regWrite4(void *opaque, hwaddr offset, uint64_t value, unsigned size);
    static void pciReset(void *opaque);
    static int mapIrq(PCIDevice *pci_dev, int irq_num);
    static void setIrq(void *opaque, int irq_num, int level);

    void realize(Error **errp);
    static void realizeWrapper(DeviceState *dev, Error **errp);

    static void classInit(DeviceClass *dc);
    static void hostBridgeClassInit(ObjectClass *klass, const void *data);
};

#define PCIC0_CFGADDR       0x0
#define PCIC0_CFGDATA       0x4

#define PCIL0_PMM0LA        0x0
#define PCIL0_PMM0MA        0x4
#define PCIL0_PMM0PCILA     0x8
#define PCIL0_PMM0PCIHA     0xc
#define PCIL0_PMM1LA        0x10
#define PCIL0_PMM1MA        0x14
#define PCIL0_PMM1PCILA     0x18
#define PCIL0_PMM1PCIHA     0x1c
#define PCIL0_PMM2LA        0x20
#define PCIL0_PMM2MA        0x24
#define PCIL0_PMM2PCILA     0x28
#define PCIL0_PMM2PCIHA     0x2c

#define PCIL0_PTM1MS        0x30
#define PCIL0_PTM1LA        0x34
#define PCIL0_PTM2MS        0x38
#define PCIL0_PTM2LA        0x3c
#define PCI_REG_BASE        0x800000
#define PCI_REG_SIZE        0x40

#define PCI_ALL_SIZE        (PCI_REG_BASE + PCI_REG_SIZE)

void PPC4xxPCIState::regWrite4(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    struct PPC4xxPCIState *pci = static_cast<struct PPC4xxPCIState *>(opaque);

    switch (offset) {
    case PCIL0_PMM0LA:    pci->pmm[0].la = value; break;
    case PCIL0_PMM0MA:    pci->pmm[0].ma = value; break;
    case PCIL0_PMM0PCIHA: pci->pmm[0].pciha = value; break;
    case PCIL0_PMM0PCILA: pci->pmm[0].pcila = value; break;
    case PCIL0_PMM1LA:    pci->pmm[1].la = value; break;
    case PCIL0_PMM1MA:    pci->pmm[1].ma = value; break;
    case PCIL0_PMM1PCIHA: pci->pmm[1].pciha = value; break;
    case PCIL0_PMM1PCILA: pci->pmm[1].pcila = value; break;
    case PCIL0_PMM2LA:    pci->pmm[2].la = value; break;
    case PCIL0_PMM2MA:    pci->pmm[2].ma = value; break;
    case PCIL0_PMM2PCIHA: pci->pmm[2].pciha = value; break;
    case PCIL0_PMM2PCILA: pci->pmm[2].pcila = value; break;
    case PCIL0_PTM1MS:    pci->ptm[0].ms = value; break;
    case PCIL0_PTM1LA:    pci->ptm[0].la = value; break;
    case PCIL0_PTM2MS:    pci->ptm[1].ms = value; break;
    case PCIL0_PTM2LA:    pci->ptm[1].la = value; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                     "%s: unhandled PCI internal register 0x%" HWADDR_PRIx "\n",
                     __func__, offset);
        break;
    }
}

uint64_t PPC4xxPCIState::regRead4(void *opaque, hwaddr offset,
                                   unsigned size)
{
    struct PPC4xxPCIState *pci = static_cast<struct PPC4xxPCIState *>(opaque);
    uint32_t value;

    switch (offset) {
    case PCIL0_PMM0LA:    value = pci->pmm[0].la; break;
    case PCIL0_PMM0MA:    value = pci->pmm[0].ma; break;
    case PCIL0_PMM0PCIHA: value = pci->pmm[0].pciha; break;
    case PCIL0_PMM0PCILA: value = pci->pmm[0].pcila; break;
    case PCIL0_PMM1LA:    value = pci->pmm[1].la; break;
    case PCIL0_PMM1MA:    value = pci->pmm[1].ma; break;
    case PCIL0_PMM1PCIHA: value = pci->pmm[1].pciha; break;
    case PCIL0_PMM1PCILA: value = pci->pmm[1].pcila; break;
    case PCIL0_PMM2LA:    value = pci->pmm[2].la; break;
    case PCIL0_PMM2MA:    value = pci->pmm[2].ma; break;
    case PCIL0_PMM2PCIHA: value = pci->pmm[2].pciha; break;
    case PCIL0_PMM2PCILA: value = pci->pmm[2].pcila; break;
    case PCIL0_PTM1MS:    value = pci->ptm[0].ms; break;
    case PCIL0_PTM1LA:    value = pci->ptm[0].la; break;
    case PCIL0_PTM2MS:    value = pci->ptm[1].ms; break;
    case PCIL0_PTM2LA:    value = pci->ptm[1].la; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid PCI internal register 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        value = 0;
    }

    return value;
}

static const MemoryRegionOps pci_reg_ops = {
    .read = PPC4xxPCIState::regRead4,
    .write = PPC4xxPCIState::regWrite4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void PPC4xxPCIState::pciReset(void *opaque)
{
    struct PPC4xxPCIState *pci = static_cast<struct PPC4xxPCIState *>(opaque);

    memset(pci->pmm, 0, sizeof(pci->pmm));
    memset(pci->ptm, 0, sizeof(pci->ptm));
}

int PPC4xxPCIState::mapIrq(PCIDevice *pci_dev, int irq_num)
{
    int slot = PCI_SLOT(pci_dev->devfn);

    trace_ppc4xx_pci_map_irq(pci_dev->devfn, irq_num, slot);

    return slot > 0 ? slot - 1 : PPC4xx_PCI_NUM_DEVS - 1;
}

void PPC4xxPCIState::setIrq(void *opaque, int irq_num, int level)
{
    qemu_irq *pci_irqs = static_cast<qemu_irq *>(opaque);

    trace_ppc4xx_pci_set_irq(irq_num);
    assert(irq_num >= 0 && irq_num < PPC4xx_PCI_NUM_DEVS);
    qemu_set_irq(pci_irqs[irq_num], level);
}

static const VMStateField vmstate_pci_master_map_fields[] = {
    VMSTATE_UINT32(la, struct PCIMasterMap),
    VMSTATE_UINT32(ma, struct PCIMasterMap),
    VMSTATE_UINT32(pcila, struct PCIMasterMap),
    VMSTATE_UINT32(pciha, struct PCIMasterMap),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_pci_master_map = {
    .name = "pci_master_map",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = vmstate_pci_master_map_fields,
};

static const VMStateField vmstate_pci_target_map_fields[] = {
    VMSTATE_UINT32(ms, struct PCITargetMap),
    VMSTATE_UINT32(la, struct PCITargetMap),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_pci_target_map = {
    .name = "pci_target_map",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = vmstate_pci_target_map_fields,
};

static const VMStateField vmstate_ppc4xx_pci_fields[] = {
    VMSTATE_STRUCT_ARRAY(pmm, PPC4xxPCIState, PPC4xx_PCI_NR_PMMS, 1,
                         vmstate_pci_master_map,
                         struct PCIMasterMap),
    VMSTATE_STRUCT_ARRAY(ptm, PPC4xxPCIState, PPC4xx_PCI_NR_PTMS, 1,
                         vmstate_pci_target_map,
                         struct PCITargetMap),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_ppc4xx_pci = {
    .name = "ppc4xx_pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_ppc4xx_pci_fields,
};

/* XXX Interrupt acknowledge cycles not supported. */
void PPC4xxPCIState::realizeWrapper(DeviceState *dev, Error **errp)
{
    PPC4xxPCIState *s = PPC4xx_PCI_HOST(dev);
    s->realize(errp);
}

void PPC4xxPCIState::realize(Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);
    PCIHostState *h;
    PCIBus *b;
    int i;

    h = PCI_HOST_BRIDGE(this);

    for (i = 0; i < ARRAY_SIZE(irq); i++) {
        sysbus_init_irq(sbd, &irq[i]);
    }

    b = pci_register_root_bus(DEVICE(this), NULL, PPC4xxPCIState::setIrq,
                              PPC4xxPCIState::mapIrq, irq, get_system_memory(),
                              get_system_io(), 0, ARRAY_SIZE(irq),
                              TYPE_PCI_BUS);
    h->bus = b;

    pci_create_simple(b, 0, TYPE_PPC4xx_HOST_BRIDGE);

    /* XXX split into 2 memory regions, one for config space, one for regs */
    memory_region_init(&container, OBJECT(this), "pci-container", PCI_ALL_SIZE);
    memory_region_init_io(&h->conf_mem, OBJECT(this), &pci_host_conf_le_ops, h,
                          "pci-conf-idx", 4);
    memory_region_init_io(&h->data_mem, OBJECT(this), &pci_host_data_le_ops, h,
                          "pci-conf-data", 4);
    memory_region_init_io(&iomem, OBJECT(this), &pci_reg_ops, this,
                          "pci.reg", PCI_REG_SIZE);
    memory_region_add_subregion(&container, PCIC0_CFGADDR, &h->conf_mem);
    memory_region_add_subregion(&container, PCIC0_CFGDATA, &h->data_mem);
    memory_region_add_subregion(&container, PCI_REG_BASE, &iomem);
    sysbus_init_mmio(sbd, &container);
    qemu_register_reset(PPC4xxPCIState::pciReset, this);
}

void PPC4xxPCIState::hostBridgeClassInit(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc        = "Host bridge";
    k->vendor_id    = PCI_VENDOR_ID_IBM;
    k->device_id    = PCI_DEVICE_ID_IBM_440GX;
    k->class_id     = PCI_CLASS_BRIDGE_OTHER;
    dc->user_creatable = false;
}

static const InterfaceInfo ppc4xx_host_bridge_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo ppc4xx_host_bridge_info = {
    .name          = TYPE_PPC4xx_HOST_BRIDGE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init    = PPC4xxPCIState::hostBridgeClassInit,
    .interfaces = ppc4xx_host_bridge_interfaces,
};

void PPC4xxPCIState::classInit(DeviceClass *dc)
{
    dc->realize = PPC4xxPCIState::realizeWrapper;
    dc->vmsd = &vmstate_ppc4xx_pci;
}

REGISTER_QEMU_DEVICE(PPC4xxPCIState, TYPE_PPC4xx_PCI_HOST,
                     TYPE_PCI_HOST_BRIDGE)

static void ppc4xx_secondary_register_types(void) __attribute__((constructor));
static void ppc4xx_secondary_register_types(void)
{
    type_register_static(&ppc4xx_host_bridge_info);
}
