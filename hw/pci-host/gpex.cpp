/*
 * QEMU Generic PCI Express Bridge Emulation
 *
 * Copyright (C) 2015 Alexander Graf <agraf@suse.de>
 *
 * Code loosely based on q35.c.
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
 *
 * Check out these documents for more information on the device:
 *
 * http://www.kernel.org/doc/Documentation/devicetree/bindings/pci/host-generic-pci.txt
 * http://www.firmware.org/1275/practice/imap/imap0_9d.pdf
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/gpex.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/****************************************************************************
 * GPEX host
 */

struct GPEXIrq {
    qemu_irq irq;
    int irq_num;
};

void GPEXHost::setIrq(void *opaque, int irq_num, int level)
{
    GPEXHost *s = static_cast<GPEXHost *>(opaque);

    qemu_set_irq(s->irq[irq_num].irq, level);
}

int gpex_set_irq_num(GPEXHost *s, int index, int gsi)
{
    if (index >= s->num_irqs) {
        return -EINVAL;
    }

    s->irq[index].irq_num = gsi;
    return 0;
}

PCIINTxRoute GPEXHost::routeIntxPinToIrq(void *opaque, int pin)
{
    PCIINTxRoute route;
    GPEXHost *s = static_cast<GPEXHost *>(opaque);
    int gsi = s->irq[pin].irq_num;

    route.irq = gsi;
    if (gsi < 0) {
        route.mode = PCI_INTX_DISABLED;
    } else {
        route.mode = PCI_INTX_ENABLED;
    }

    return route;
}

int GPEXHost::swizzleMapIrqFn(PCIDevice *pci_dev, int pin)
{
    PCIBus *bus = pci_device_root_bus(pci_dev);

    return (PCI_SLOT(pci_dev->devfn) + pin) % bus->nirq;
}

void GPEXHost::realize(Error **errp)
{
    DeviceState *dev = reinterpret_cast<DeviceState *>(this);
    GPEXHost *s = this;
    PCIHostState *pci = reinterpret_cast<PCIHostState *>(dev);
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(dev);
    PCIExpressHost *pex = reinterpret_cast<PCIExpressHost *>(dev);
    int i;

    irq = static_cast<GPEXIrq *>(g_malloc0_n(num_irqs, sizeof(*irq)));

    pcie_host_mmcfg_init(pex, PCIE_MMCFG_SIZE_MAX);
    sysbus_init_mmio(sbd, &pex->mmio);

    /*
     * Note that the MemoryRegions io_mmio and io_ioport that we pass
     * to pci_register_root_bus() are not the same as the
     * MemoryRegions io_mmio_window and io_ioport_window that we
     * expose as SysBus MRs. The difference is in the behaviour of
     * accesses to addresses where no PCI device has been mapped.
     *
     * io_mmio and io_ioport are the underlying PCI view of the PCI
     * address space, and when a PCI device does a bus master access
     * to a bad address this is reported back to it as a transaction
     * failure.
     *
     * io_mmio_window and io_ioport_window implement "unmapped
     * addresses read as -1 and ignore writes"; this is traditional
     * x86 PC behaviour, which is not mandated by the PCI spec proper
     * but expected by much PCI-using guest software, including Linux.
     *
     * In the interests of not being unnecessarily surprising, we
     * implement it in the gpex PCI host controller, by providing the
     * _window MRs, which are containers with io ops that implement
     * the 'background' behaviour and which hold the real PCI MRs as
     * subregions.
     */
    memory_region_init(&s->io_mmio, reinterpret_cast<Object *>(s), "gpex_mmio", UINT64_MAX);
    memory_region_init(&s->io_ioport, reinterpret_cast<Object *>(s), "gpex_ioport", 64 * 1024);

    if (s->allow_unmapped_accesses) {
        memory_region_init_io(&s->io_mmio_window, reinterpret_cast<Object *>(s),
                              &unassigned_io_ops, reinterpret_cast<Object *>(s),
                              "gpex_mmio_window", UINT64_MAX);
        memory_region_init_io(&s->io_ioport_window, reinterpret_cast<Object *>(s),
                              &unassigned_io_ops, reinterpret_cast<Object *>(s),
                              "gpex_ioport_window", 64 * 1024);

        memory_region_add_subregion(&s->io_mmio_window, 0, &s->io_mmio);
        memory_region_add_subregion(&s->io_ioport_window, 0, &s->io_ioport);
        sysbus_init_mmio(sbd, &s->io_mmio_window);
        sysbus_init_mmio(sbd, &s->io_ioport_window);
    } else {
        sysbus_init_mmio(sbd, &s->io_mmio);
        sysbus_init_mmio(sbd, &s->io_ioport);
    }

    for (i = 0; i < s->num_irqs; i++) {
        sysbus_init_irq(sbd, &s->irq[i].irq);
        s->irq[i].irq_num = -1;
    }

    pci->bus = pci_register_root_bus(dev, "pcie.0", GPEXHost::setIrq,
                                     GPEXHost::swizzleMapIrqFn,
                                     s, &s->io_mmio, &s->io_ioport, 0,
                                     s->num_irqs, TYPE_PCIE_BUS);

    pci_bus_set_route_irq_fn(pci->bus, GPEXHost::routeIntxPinToIrq);
    qdev_realize(reinterpret_cast<DeviceState *>(&s->gpex_root), reinterpret_cast<BusState *>(pci->bus), &error_fatal);
}

static void gpex_host_realize(DeviceState *dev, Error **errp)
{
    GPEXHost *s = reinterpret_cast<GPEXHost *>(dev);
    s->realize(errp);
}

void GPEXHost::unrealize()
{
    g_free(irq);
}

static void gpex_host_unrealize(DeviceState *dev)
{
    GPEXHost *s = reinterpret_cast<GPEXHost *>(dev);
    s->unrealize();
}

const char *GPEXHost::rootBusPath(PCIHostState *host_bridge,
                                  PCIBus *rootbus)
{
    return "0000:00";
}

static const Property gpex_host_properties[] = {
    /*
     * Permit CPU accesses to unmapped areas of the PIO and MMIO windows
     * (discarding writes and returning -1 for reads) rather than aborting.
     */
    DEFINE_PROP_BOOL("allow-unmapped-accesses", GPEXHost,
                     allow_unmapped_accesses, true),
    DEFINE_PROP_UINT64(PCI_HOST_ECAM_BASE, GPEXHost, gpex_cfg.ecam.base, 0),
    DEFINE_PROP_SIZE(PCI_HOST_ECAM_SIZE, GPEXHost, gpex_cfg.ecam.size, 0),
    DEFINE_PROP_UINT64(PCI_HOST_PIO_BASE, GPEXHost, gpex_cfg.pio.base, 0),
    DEFINE_PROP_SIZE(PCI_HOST_PIO_SIZE, GPEXHost, gpex_cfg.pio.size, 0),
    DEFINE_PROP_UINT64(PCI_HOST_BELOW_4G_MMIO_BASE, GPEXHost,
                       gpex_cfg.mmio32.base, 0),
    DEFINE_PROP_SIZE(PCI_HOST_BELOW_4G_MMIO_SIZE, GPEXHost,
                     gpex_cfg.mmio32.size, 0),
    DEFINE_PROP_UINT64(PCI_HOST_ABOVE_4G_MMIO_BASE, GPEXHost,
                       gpex_cfg.mmio64.base, 0),
    DEFINE_PROP_SIZE(PCI_HOST_ABOVE_4G_MMIO_SIZE, GPEXHost,
                     gpex_cfg.mmio64.size, 0),
    DEFINE_PROP_UINT8("num-irqs", GPEXHost, num_irqs, PCI_NUM_PINS),
};

void GPEXHost::classInit(ObjectClass *klass, const void *data)
    {
        DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
        PCIHostBridgeClass *hc = reinterpret_cast<PCIHostBridgeClass *>(klass);

        hc->root_bus_path = GPEXHost::rootBusPath;
        dc->realize = gpex_host_realize;
        dc->unrealize = gpex_host_unrealize;
        set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
        dc->fw_name = "pci";
        device_class_set_props(dc, gpex_host_properties);
}

void GPEXHost::initfn(Object *obj)
{
    GPEXHost *s = reinterpret_cast<GPEXHost *>(obj);
    GPEXRootState *root = &s->gpex_root;

    object_initialize_child(obj, "gpex_root", root, TYPE_GPEX_ROOT_DEVICE);
    qdev_prop_set_int32(reinterpret_cast<DeviceState *>(root), "addr", PCI_DEVFN(0, 0));
    qdev_prop_set_bit(reinterpret_cast<DeviceState *>(root), "multifunction", false);
}

static const TypeInfo gpex_host_info = {
    .name       = TYPE_GPEX_HOST,
    .parent     = TYPE_PCIE_HOST_BRIDGE,
    .instance_size = sizeof(GPEXHost),
    .instance_init = GPEXHost::initfn,
    .class_init = GPEXHost::classInit,
};

/****************************************************************************
 * GPEX Root D0:F0
 */

static const VMStateDescription vmstate_gpex_root = {
    .name = "gpex_root",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, GPEXRootState),
        VMSTATE_END_OF_LIST()
    }
};

void GPEXRootState::classInit(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "QEMU generic PCIe host bridge";
    dc->vmsd = &vmstate_gpex_root;
    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = PCI_DEVICE_ID_REDHAT_PCIE_HOST;
    k->revision = 0;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo gpex_root_info = {
    .name = TYPE_GPEX_ROOT_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GPEXRootState),
    .class_init = GPEXRootState::classInit,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void gpex_register(void)
{
    type_register_static(&gpex_root_info);
    type_register_static(&gpex_host_info);
}

type_init(gpex_register)
