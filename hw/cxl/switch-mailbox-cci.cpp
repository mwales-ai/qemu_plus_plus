/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Emulation of a CXL Switch Mailbox CCI PCIe function.
 *
 * Copyright (c) 2023 Huawei Technologies.
 *
 * From www.computeexpresslink.org
 * Compute Express Link (CXL) Specification revision 3.0 Version 1.0
 */
#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci-bridge/cxl_upstream_port.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "hw/cxl/cxl.h"
#include "qom/cpp/object.h"

#define CXL_SWCCI_MSIX_MBOX 3

static void cswmbcci_reset(DeviceState *dev)
{
    CSWMBCCIDev *cswmb = CXL_SWITCH_MAILBOX_CCI(dev);
    cxl_device_register_init_swcci(cswmb, CXL_SWCCI_MSIX_MBOX);
}

static void cswbcci_realize(PCIDevice *pci_dev, Error **errp)
{
    CSWMBCCIDev *cswmb = CXL_SWITCH_MAILBOX_CCI(pci_dev);
    CXLComponentState *cxl_cstate = &cswmb->cxl_cstate;
    CXLDeviceState *cxl_dstate = &cswmb->cxl_dstate;
    CXLDVSECRegisterLocator *regloc_dvsec;
    CXLUpstreamPort *usp;

    if (!cswmb->target) {
        error_setg(errp, "Target not set");
        return;
    }
    usp = CXL_USP(cswmb->target);

    pcie_endpoint_cap_init(pci_dev, 0x80);
    cxl_cstate->dvsec_offset = 0x100;
    cxl_cstate->pdev = pci_dev;
    cswmb->cci = &usp->swcci;
    cxl_device_register_block_init(OBJECT(pci_dev), cxl_dstate, cswmb->cci);
    pci_register_bar(pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &cxl_dstate->device_registers);
    CXLDVSECRegisterLocator regloc_init;
    memset(&regloc_init, 0, sizeof(regloc_init));
    regloc_init.reg0_base_lo = RBI_CXL_DEVICE_REG | 0;
    regloc_dvsec = &regloc_init;
    cxl_component_create_dvsec(cxl_cstate, CXL3_SWITCH_MAILBOX_CCI,
                               REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                               REG_LOC_DVSEC_REVID, (uint8_t *)regloc_dvsec);

    cxl_initialize_mailbox_swcci(cswmb->cci, DEVICE(pci_dev),
                                 DEVICE(cswmb->target),
                                 CXL_MAILBOX_MAX_PAYLOAD_SIZE);
}

static void cswmbcci_exit(PCIDevice *pci_dev)
{
    /* Nothing to do here yet */
}

static const Property cxl_switch_cci_props[] = {
    DEFINE_PROP_LINK("target", CSWMBCCIDev,
                     target, TYPE_CXL_USP, PCIDevice *),
};

/* Subclass struct for PCIDeviceClass virtual method overrides */
struct CSWMBCCIDeviceClass : PCIDeviceClass {
    void pci_realize(PCIDevice *dev, Error **errp) override
    {
        cswbcci_realize(dev, errp);
    }
    void pci_exit(PCIDevice *dev) override
    {
        cswmbcci_exit(dev);
    }
};

static void cswmbcci_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    qom_fixup_vtable<CSWMBCCIDeviceClass>(oc);

    /* Serial bus, CXL Switch CCI */
    pc->class_id = 0x0c0b;
    /*
     * Huawei Technologies
     * CXL Switch Mailbox CCI - DID assigned for emulation only.
     * No real hardware will ever use this ID.
     */
    pc->vendor_id = 0x19e5;
    pc->device_id = 0xa123;
    pc->revision = 0;
    dc->desc = "CXL Switch Mailbox CCI";
    device_class_set_legacy_reset(dc, cswmbcci_reset);
    device_class_set_props(dc, cxl_switch_cci_props);
}

static const InterfaceInfo cswmbcci_interfaces[] = {
    { INTERFACE_PCIE_DEVICE },
    { },
};

static const TypeInfo cswmbcci_info = {
    .name = TYPE_CXL_SWITCH_MAILBOX_CCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CSWMBCCIDev),
    .class_size = sizeof(CSWMBCCIDeviceClass),
    .class_init = cswmbcci_class_init,
    .interfaces = cswmbcci_interfaces,
};

static void cxl_switch_mailbox_cci_register(void)
{
    type_register_static(&cswmbcci_info);
}
type_init(cxl_switch_mailbox_cci_register);
