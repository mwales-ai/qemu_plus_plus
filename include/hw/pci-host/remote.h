/*
 * PCI Host for remote device
 *
 * Copyright © 2018, 2021 Oracle and/or its affiliates.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef PCI_HOST_REMOTE_H
#define PCI_HOST_REMOTE_H

#include "system/memory.h"
#include "hw/pci/pcie_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE_REMOTE_PCIHOST "remote-pcihost"
OBJECT_DECLARE_SIMPLE_TYPE(RemotePCIHost, REMOTE_PCIHOST)

struct RemotePCIHost {
    /*< private >*/
    PCIExpressHost parent_obj;
    /*< public >*/

    MemoryRegion *mr_pci_mem;
    MemoryRegion *mr_sys_io;
    MemoryRegion *mr_sys_mem;

#ifdef __cplusplus
    void realize(Error **errp);
    static void classInit(DeviceClass *dc);
#endif
};

#ifdef __cplusplus
}
#endif

#endif
