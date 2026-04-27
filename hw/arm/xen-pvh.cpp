/*
 * QEMU ARM Xen PVH Machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/qapi-commands-migration.h"
#include "hw/boards.h"
#include "system/system.h"
#include "hw/xen/xen-pvh-common.h"
#include "hw/arm/machines-qom.h"

extern "C" {
#include "qemu/error-report.h"
}

#define TYPE_XEN_ARM  MACHINE_TYPE_NAME("xenpvh")

/*
 * VIRTIO_MMIO_DEV_SIZE is imported from tools/libs/light/libxl_arm.c under Xen
 * repository.
 *
 * Origin: git://xenbits.xen.org/xen.git 2128143c114c
 */
#define VIRTIO_MMIO_DEV_SIZE   0x200

#define NR_VIRTIO_MMIO_DEVICES   \
   (GUEST_VIRTIO_MMIO_SPI_LAST - GUEST_VIRTIO_MMIO_SPI_FIRST)

void XenPVHMachineState::init()
{
    XenPVHMachineState *s = this;

    /* Default values.  */
    MemMapEntry ram_low_val = { GUEST_RAM0_BASE, GUEST_RAM0_SIZE };
    s->cfg.ram_low = ram_low_val;
    MemMapEntry ram_high_val = { GUEST_RAM1_BASE, GUEST_RAM1_SIZE };
    s->cfg.ram_high = ram_high_val;

    s->cfg.virtio_mmio_num = NR_VIRTIO_MMIO_DEVICES;
    s->cfg.virtio_mmio_irq_base = GUEST_VIRTIO_MMIO_SPI_FIRST;
    MemMapEntry virtio_mmio_val = { GUEST_VIRTIO_MMIO_BASE,
                                    VIRTIO_MMIO_DEV_SIZE };
    s->cfg.virtio_mmio = virtio_mmio_val;
}

static void xen_pvh_set_pci_intx_irq(void *opaque, int intx_irq, int level)
{
    XenPVHMachineState *s = XEN_PVH_MACHINE(opaque);
    int irq = s->cfg.pci_intx_irq_base + intx_irq;

    if (xendevicemodel_set_irq_level(xen_dmod, xen_domid, irq, level)) {
        error_report("xendevicemodel_set_pci_intx_level failed");
    }
}

void XenPVHMachineState::classInit(DeviceClass *dc)
{
    ObjectClass *oc = reinterpret_cast<ObjectClass *>(dc);
    XenPVHMachineClass *xpc = XEN_PVH_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Xen PVH ARM machine";

    /*
     * mc->max_cpus holds the MAX value allowed in the -smp command-line opts.
     *
     * 1. If users don't pass any -smp option:
     *   ms->smp.cpus will default to 1.
     *   ms->smp.max_cpus will default to 1.
     *
     * 2. If users pass -smp X:
     *   ms->smp.cpus will be set to X.
     *   ms->smp.max_cpus will also be set to X.
     *
     * 3. If users pass -smp X,maxcpus=Y:
     *   ms->smp.cpus will be set to X.
     *   ms->smp.max_cpus will be set to Y.
     *
     * In scenarios 2 and 3, if X or Y are set to something larger than
     * mc->max_cpus, QEMU will bail out with an error message.
     */
    mc->max_cpus = GUEST_MAX_VCPUS;

    /* Xen/ARM does not use buffered IOREQs.  */
    xpc->handle_bufioreq = HVM_IOREQSRV_BUFIOREQ_OFF;

    /* PCI INTX delivery.  */
    xpc->set_pci_intx_irq = xen_pvh_set_pci_intx_irq;

    /* List of supported features known to work on PVH ARM.  */
    xpc->has_pci = true;
    xpc->has_tpm = true;
    xpc->has_virtio_mmio = true;

    xen_pvh_class_setup_common_props(xpc);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(XenPVHMachineState, TYPE_XEN_ARM,
                             TYPE_XEN_PVH_MACHINE, arm_aarch64_machine_interfaces)
