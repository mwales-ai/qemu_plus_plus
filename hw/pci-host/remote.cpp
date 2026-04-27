/*
 * Remote PCI host device
 *
 * Unlike PCI host devices that model physical hardware, the purpose
 * of this PCI host is to host multi-process QEMU devices.
 *
 * Multi-process QEMU extends the PCI host of a QEMU machine into a
 * remote process. Any PCI device attached to the remote process is
 * visible in the QEMU guest. This allows existing QEMU device models
 * to be reused in the remote process.
 *
 * This PCI host is purely a container for PCI devices. It's fake in the
 * sense that the guest never sees this PCI host and has no way of
 * accessing it. Its job is just to provide the environment that QEMU
 * PCI device models need when running in a remote process.
 *
 * Copyright © 2018, 2021 Oracle and/or its affiliates.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"

#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pcie_host.h"
#include "hw/qdev-properties.h"
#include "hw/pci-host/remote.h"
#include "system/memory.h"

static const char *remote_pcihost_root_bus_path(PCIHostState *host_bridge,
                                                PCIBus *rootbus)
{
    return "0000:00";
}

void RemotePCIHost::realize(Error **errp)
{
    PCIHostState *pci = PCI_HOST_BRIDGE(this);

    pci->bus = pci_root_bus_new(DEVICE(this), "remote-pci",
                                mr_pci_mem, mr_sys_io,
                                0, TYPE_PCIE_BUS);
}

void RemotePCIHost::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->root_bus_path = remote_pcihost_root_bus_path;

    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(RemotePCIHost, TYPE_REMOTE_PCIHOST, TYPE_PCIE_HOST_BRIDGE)
