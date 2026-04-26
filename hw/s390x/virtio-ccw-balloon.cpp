/*
 * virtio ccw balloon implementation
 *
 * Copyright 2012, 2015 IBM Corp.
 * Author(s): Cornelia Huck <cornelia.huck@de.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "virtio-ccw.h"
#include "hw/virtio/virtio-balloon.h"

#define TYPE_VIRTIO_BALLOON_CCW "virtio-balloon-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOBalloonCcw, VIRTIO_BALLOON_CCW)

struct VirtIOBalloonCcw {
    VirtioCcwDevice parent_obj;
    VirtIOBalloon vdev;

#ifdef __cplusplus
    void init();
    static void classInit(DeviceClass *dc);
#endif
};

static void virtio_ccw_balloon_realize(VirtioCcwDevice *ccw_dev, Error **errp)
{
    VirtIOBalloonCcw *dev = VIRTIO_BALLOON_CCW(ccw_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    qdev_realize(vdev, BUS(&ccw_dev->bus), errp);
}

void VirtIOBalloonCcw::init()
{
    virtio_instance_init_common(OBJECT(this), &vdev, sizeof(vdev),
                                TYPE_VIRTIO_BALLOON);
    object_property_add_alias(OBJECT(this), "guest-stats", OBJECT(&vdev),
                              "guest-stats");
    object_property_add_alias(OBJECT(this), "guest-stats-polling-interval",
                              OBJECT(&vdev),
                              "guest-stats-polling-interval");
}

static const Property virtio_ccw_balloon_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtioCcwDevice, flags,
                    VIRTIO_CCW_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("max_revision", VirtioCcwDevice, max_rev,
                       VIRTIO_CCW_MAX_REV),
};

void VirtIOBalloonCcw::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    VirtIOCCWDeviceClass *k = VIRTIO_CCW_DEVICE_CLASS(klass);

    k->realize = virtio_ccw_balloon_realize;
    device_class_set_props(dc, virtio_ccw_balloon_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VirtIOBalloonCcw, TYPE_VIRTIO_BALLOON_CCW, TYPE_VIRTIO_CCW_DEVICE)
