/*
 * QEMU Intel 82576 SR/IOV Ethernet Controller Emulation
 *
 * Datasheet:
 * https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82576eg-gbe-datasheet.pdf
 *
 * Copyright (c) 2020-2023 Red Hat, Inc.
 * Copyright (c) 2015 Ravello Systems LTD (http://ravellosystems.com)
 * Developed by Daynix Computing LTD (http://www.daynix.com)
 *
 * Authors:
 * Akihiko Odaki <akihiko.odaki@daynix.com>
 * Gal Hammmer <gal.hammer@sap.com>
 * Marcel Apfelbaum <marcel.apfelbaum@gmail.com>
 * Dmitry Fleytman <dmitry@daynix.com>
 * Leonid Bloch <leonid@daynix.com>
 * Yan Vugenfirer <yan@daynix.com>
 *
 * Based on work done by:
 * Nir Peleg, Tutis Systems Ltd. for Qumranet Inc.
 * Copyright (c) 2008 Qumranet
 * Based on work done by:
 * Copyright (c) 2007 Dan Aloni
 * Copyright (c) 2004 Antony T Curtis
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

extern "C" {
#include "trace.h"
}

#include "hw/hw.h"
#include "hw/net/mii.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msix.h"
#include "net/eth.h"
#include "net/net.h"
#include "igb_common.h"
#include "igb_core.h"
#include "qapi/error.h"

OBJECT_DECLARE_SIMPLE_TYPE(IgbVfState, IGBVF)

struct IgbVfState {
    PCIDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion msix;

    /* methods */
    static hwaddr vfToPfAddr(hwaddr addr, uint16_t vfn, bool write);
    static void writeConfig(PCIDevice *dev, uint32_t addr, uint32_t val,
                            int len);
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);
    void pciRealize(Error **errp);
    void resetHold(ResetType type);
    void pciUninit();
    static void classInit(DeviceClass *dc);
};

hwaddr IgbVfState::vfToPfAddr(hwaddr addr, uint16_t vfn, bool write)
{
    switch (addr) {
    case E1000_CTRL:
    case E1000_CTRL_DUP:
        return E1000_PVTCTRL(vfn);
    case E1000_EICS:
        return E1000_PVTEICS(vfn);
    case E1000_EIMS:
        return E1000_PVTEIMS(vfn);
    case E1000_EIMC:
        return E1000_PVTEIMC(vfn);
    case E1000_EIAC:
        return E1000_PVTEIAC(vfn);
    case E1000_EIAM:
        return E1000_PVTEIAM(vfn);
    case E1000_EICR:
        return E1000_PVTEICR(vfn);
    case E1000_EITR(0):
    case E1000_EITR(1):
    case E1000_EITR(2):
        return E1000_EITR(22) + (addr - E1000_EITR(0)) - vfn * 0xC;
    case E1000_IVAR0:
        return E1000_VTIVAR + vfn * 4;
    case E1000_IVAR_MISC:
        return E1000_VTIVAR_MISC + vfn * 4;
    case 0x0F04: /* PBACL */
        return E1000_PBACLR;
    case 0x0F0C: /* PSRTYPE */
        return E1000_PSRTYPE(vfn);
    case E1000_V2PMAILBOX(0):
        return E1000_V2PMAILBOX(vfn);
    default:
        break;
    }

    if (addr >= E1000_VMBMEM(0) && addr <= E1000_VMBMEM(0) + 0x3F) {
        return addr + vfn * 0x40;
    }

    switch (addr) {
    case E1000_RDBAL_A(0):
        return E1000_RDBAL(vfn);
    case E1000_RDBAL_A(1):
        return E1000_RDBAL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RDBAH_A(0):
        return E1000_RDBAH(vfn);
    case E1000_RDBAH_A(1):
        return E1000_RDBAH(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RDLEN_A(0):
        return E1000_RDLEN(vfn);
    case E1000_RDLEN_A(1):
        return E1000_RDLEN(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_SRRCTL_A(0):
        return E1000_SRRCTL(vfn);
    case E1000_SRRCTL_A(1):
        return E1000_SRRCTL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RDH_A(0):
        return E1000_RDH(vfn);
    case E1000_RDH_A(1):
        return E1000_RDH(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RXCTL_A(0):
        return E1000_RXCTL(vfn);
    case E1000_RXCTL_A(1):
        return E1000_RXCTL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RDT_A(0):
        return E1000_RDT(vfn);
    case E1000_RDT_A(1):
        return E1000_RDT(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RXDCTL_A(0):
        return E1000_RXDCTL(vfn);
    case E1000_RXDCTL_A(1):
        return E1000_RXDCTL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_RQDPC_A(0):
        return E1000_RQDPC(vfn);
    case E1000_RQDPC_A(1):
        return E1000_RQDPC(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDBAL_A(0):
        return E1000_TDBAL(vfn);
    case E1000_TDBAL_A(1):
        return E1000_TDBAL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDBAH_A(0):
        return E1000_TDBAH(vfn);
    case E1000_TDBAH_A(1):
        return E1000_TDBAH(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDLEN_A(0):
        return E1000_TDLEN(vfn);
    case E1000_TDLEN_A(1):
        return E1000_TDLEN(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDH_A(0):
        return E1000_TDH(vfn);
    case E1000_TDH_A(1):
        return E1000_TDH(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TXCTL_A(0):
        return E1000_TXCTL(vfn);
    case E1000_TXCTL_A(1):
        return E1000_TXCTL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDT_A(0):
        return E1000_TDT(vfn);
    case E1000_TDT_A(1):
        return E1000_TDT(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TXDCTL_A(0):
        return E1000_TXDCTL(vfn);
    case E1000_TXDCTL_A(1):
        return E1000_TXDCTL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDWBAL_A(0):
        return E1000_TDWBAL(vfn);
    case E1000_TDWBAL_A(1):
        return E1000_TDWBAL(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_TDWBAH_A(0):
        return E1000_TDWBAH(vfn);
    case E1000_TDWBAH_A(1):
        return E1000_TDWBAH(vfn + IGB_MAX_VF_FUNCTIONS);
    case E1000_VFGPRC:
        return E1000_PVFGPRC(vfn);
    case E1000_VFGPTC:
        return E1000_PVFGPTC(vfn);
    case E1000_VFGORC:
        return E1000_PVFGORC(vfn);
    case E1000_VFGOTC:
        return E1000_PVFGOTC(vfn);
    case E1000_VFMPRC:
        return E1000_PVFMPRC(vfn);
    case E1000_VFGPRLBC:
        return E1000_PVFGPRLBC(vfn);
    case E1000_VFGPTLBC:
        return E1000_PVFGPTLBC(vfn);
    case E1000_VFGORLBC:
        return E1000_PVFGORLBC(vfn);
    case E1000_VFGOTLBC:
        return E1000_PVFGOTLBC(vfn);
    case E1000_STATUS:
    case E1000_FRTIMER:
        if (write) {
            return HWADDR_MAX;
        }
        /* fallthrough */
    case 0x34E8: /* PBTWAC */
    case 0x24E8: /* PBRWAC */
        return addr;
    }

    trace_igbvf_wrn_io_addr_unknown(addr);

    return HWADDR_MAX;
}

void IgbVfState::writeConfig(PCIDevice *dev, uint32_t addr, uint32_t val,
    int len)
{
    trace_igbvf_write_config(addr, val, len);
    pci_default_write_config(dev, addr, val, len);
    if (object_property_get_bool(reinterpret_cast<Object *>(pcie_sriov_get_pf(dev)),
                                 "x-pcie-flr-init", &error_abort)) {
        pcie_cap_flr_write_config(dev, addr, val, len);
    }
}

uint64_t IgbVfState::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    PCIDevice *vf = reinterpret_cast<PCIDevice *>(opaque);
    PCIDevice *pf = pcie_sriov_get_pf(vf);

    addr = vfToPfAddr(addr, pcie_sriov_vf_number(vf), false);
    return addr == HWADDR_MAX ? 0 : igb_mmio_read(pf, addr, size);
}

void IgbVfState::mmioWrite(void *opaque, hwaddr addr, uint64_t val,
    unsigned size)
{
    PCIDevice *vf = reinterpret_cast<PCIDevice *>(opaque);
    PCIDevice *pf = pcie_sriov_get_pf(vf);

    addr = vfToPfAddr(addr, pcie_sriov_vf_number(vf), true);
    if (addr != HWADDR_MAX) {
        igb_mmio_write(pf, addr, val, size);
    }
}

static const MemoryRegionOps mmio_ops = {
    .read = IgbVfState::mmioRead,
    .write = IgbVfState::mmioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void igbvf_pci_realize(PCIDevice *dev, Error **errp)
{
    IgbVfState *s = reinterpret_cast<IgbVfState *>(dev);
    s->pciRealize(errp);
}

void IgbVfState::pciRealize(Error **errp)
{
    PCIDevice *dev = reinterpret_cast<PCIDevice *>(reinterpret_cast<DeviceState *>(this));
    int ret;
    int i;

    dev->config_write = IgbVfState::writeConfig;

    memory_region_init_io(&this->mmio, reinterpret_cast<Object *>(this), &mmio_ops, this, "igbvf-mmio",
        IGBVF_MMIO_SIZE);
    pci_register_bar(dev, IGBVF_MMIO_BAR_IDX, PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &this->mmio);

    memory_region_init(&this->msix, reinterpret_cast<Object *>(this), "igbvf-msix", IGBVF_MSIX_SIZE);
    pci_register_bar(dev, IGBVF_MSIX_BAR_IDX, PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &this->msix);

    ret = msix_init(dev, IGBVF_MSIX_VEC_NUM, &this->msix, IGBVF_MSIX_BAR_IDX, 0,
        &this->msix, IGBVF_MSIX_BAR_IDX, 0x2000, 0x70, errp);
    if (ret) {
        return;
    }

    for (i = 0; i < IGBVF_MSIX_VEC_NUM; i++) {
        msix_vector_use(dev, i);
    }

    if (pcie_endpoint_cap_init(dev, 0xa0) < 0) {
        hw_error("Failed to initialize PCIe capability");
    }

    if (object_property_get_bool(reinterpret_cast<Object *>(pcie_sriov_get_pf(dev)),
                                 "x-pcie-flr-init", &error_abort)) {
        pcie_cap_flr_init(dev);
    }

    if (pcie_aer_init(dev, 1, 0x100, 0x40, errp) < 0) {
        hw_error("Failed to initialize AER capability");
    }

    pcie_ari_init(dev, 0x150);
}

static void igbvf_qdev_reset_hold(Object *obj, ResetType type)
{
    IgbVfState *s = reinterpret_cast<IgbVfState *>(obj);
    s->resetHold(type);
}

void IgbVfState::resetHold(ResetType type)
{
    PCIDevice *vf = reinterpret_cast<PCIDevice *>(reinterpret_cast<DeviceState *>(this));

    igb_vf_reset(pcie_sriov_get_pf(vf), pcie_sriov_vf_number(vf));
}

static void igbvf_pci_uninit(PCIDevice *dev)
{
    IgbVfState *s = reinterpret_cast<IgbVfState *>(dev);
    s->pciUninit();
}

void IgbVfState::pciUninit()
{
    PCIDevice *dev = reinterpret_cast<PCIDevice *>(reinterpret_cast<DeviceState *>(this));

    pcie_aer_exit(dev);
    pcie_cap_exit(dev);
    msix_unuse_all_vectors(dev);
    msix_uninit(dev, &this->msix, &this->msix);
}

void IgbVfState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    PCIDeviceClass *c = reinterpret_cast<PCIDeviceClass *>(klass);
    ResettableClass *rc = reinterpret_cast<ResettableClass *>(klass);

    c->realize = igbvf_pci_realize;
    c->exit = igbvf_pci_uninit;
    c->vendor_id = PCI_VENDOR_ID_INTEL;
    c->device_id = E1000_DEV_ID_82576_VF;
    c->revision = 1;
    c->class_id = PCI_CLASS_NETWORK_ETHERNET;

    rc->phases.hold = igbvf_qdev_reset_hold;

    dc->desc = "Intel 82576 Virtual Function";
    dc->user_creatable = false;

    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const InterfaceInfo igbvf_interfaces[] = {
    { INTERFACE_PCIE_DEVICE },
    { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(IgbVfState, TYPE_IGBVF,
                             TYPE_PCI_DEVICE, igbvf_interfaces)
