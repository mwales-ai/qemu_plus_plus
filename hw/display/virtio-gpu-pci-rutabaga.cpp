/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-gpu-pci.h"
#include "qom/object.h"

#define TYPE_VIRTIO_GPU_RUTABAGA_PCI "virtio-gpu-rutabaga-pci"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOGPURutabagaPCI, VIRTIO_GPU_RUTABAGA_PCI)

struct VirtIOGPURutabagaPCI {
    VirtIOGPUPCIBase parent_obj;

    VirtIOGPURutabaga vdev;
};

static void virtio_gpu_rutabaga_initfn(Object *obj)
{
    VirtIOGPURutabagaPCI *dev = VIRTIO_GPU_RUTABAGA_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_GPU_RUTABAGA);
    VIRTIO_GPU_PCI_BASE(obj)->vgpu = VIRTIO_GPU_BASE(&dev->vdev);
}

static const InterfaceInfo virtio_gpu_rutabaga_pci_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_INIT_CLASS_SIZED_IFACES(virtio_gpu_rutabaga_pci,
                                              VirtIOGPURutabagaPCI,
                                              TYPE_VIRTIO_GPU_RUTABAGA_PCI,
                                              TYPE_VIRTIO_GPU_PCI_BASE,
                                              virtio_gpu_rutabaga_initfn,
                                              NULL,
                                              virtio_gpu_rutabaga_pci_interfaces)

module_obj(TYPE_VIRTIO_GPU_RUTABAGA_PCI);
module_kconfig(VIRTIO_PCI);
module_dep("hw-display-virtio-gpu-pci");
