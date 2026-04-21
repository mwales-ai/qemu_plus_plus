/*
 * vhost vsock ccw implementation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "virtio-ccw.h"
#include "hw/virtio/vhost-vsock.h"

#define TYPE_VHOST_VSOCK_CCW "vhost-vsock-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(VHostVSockCCWState, VHOST_VSOCK_CCW)

struct VHostVSockCCWState {
    VirtioCcwDevice parent_obj;
    VHostVSock vdev;

    static void realize(VirtioCcwDevice *ccw_dev, Error **errp)
    {
        VHostVSockCCWState *dev = VHOST_VSOCK_CCW(ccw_dev);
        DeviceState *vdev = DEVICE(&dev->vdev);

        qdev_realize(vdev, BUS(&ccw_dev->bus), errp);
    }

    void init()
    {
        Object *obj = reinterpret_cast<Object *>(this);
        VirtioCcwDevice *ccw_dev = reinterpret_cast<VirtioCcwDevice *>(this);
        VirtIODevice *virtio_dev;

        virtio_instance_init_common(obj, &vdev, sizeof(vdev),
                                    TYPE_VHOST_VSOCK);

        virtio_dev = VIRTIO_DEVICE(&vdev);

        if (!virtio_legacy_check_disabled(virtio_dev)) {
            ccw_dev->force_revision_1 = true;
        }
    }

    static void classInit(DeviceClass *dc);
};

static const Property vhost_vsock_ccw_properties[] = {
    DEFINE_PROP_UINT32("max_revision", VirtioCcwDevice, max_rev,
                       VIRTIO_CCW_MAX_REV),
};

void VHostVSockCCWState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    VirtIOCCWDeviceClass *k = reinterpret_cast<VirtIOCCWDeviceClass *>(klass);

    k->realize = realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, vhost_vsock_ccw_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VHostVSockCCWState, TYPE_VHOST_VSOCK_CCW, TYPE_VIRTIO_CCW_DEVICE)
