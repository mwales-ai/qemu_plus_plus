/*
 * virtio ccw input implementation
 *
 * Copyright 2012, 2015 IBM Corp.
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
#include "hw/virtio/virtio-input.h"

#define TYPE_VIRTIO_INPUT_CCW "virtio-input-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputCcw, VIRTIO_INPUT_CCW)

struct VirtIOInputCcw {
    VirtioCcwDevice parent_obj;
    VirtIOInput vdev;
};

#define TYPE_VIRTIO_INPUT_HID_CCW "virtio-input-hid-ccw"
#define TYPE_VIRTIO_KEYBOARD_CCW "virtio-keyboard-ccw"
#define TYPE_VIRTIO_MOUSE_CCW "virtio-mouse-ccw"
#define TYPE_VIRTIO_TABLET_CCW "virtio-tablet-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputHIDCcw, VIRTIO_INPUT_HID_CCW)

struct VirtIOInputHIDCcw {
    VirtioCcwDevice parent_obj;
    VirtIOInputHID vdev;
};

static void virtio_ccw_input_realize(VirtioCcwDevice *ccw_dev, Error **errp)
{
    VirtIOInputCcw *dev = VIRTIO_INPUT_CCW(ccw_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    qdev_realize(vdev, BUS(&ccw_dev->bus), errp);
}

static const Property virtio_ccw_input_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtioCcwDevice, flags,
                    VIRTIO_CCW_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("max_revision", VirtioCcwDevice, max_rev,
                       VIRTIO_CCW_MAX_REV),
};

static void virtio_ccw_input_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtIOCCWDeviceClass *k = VIRTIO_CCW_DEVICE_CLASS(klass);

    k->realize = virtio_ccw_input_realize;
    device_class_set_props(dc, virtio_ccw_input_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static void virtio_ccw_keyboard_instance_init(Object *obj)
{
    VirtIOInputHIDCcw *dev = VIRTIO_INPUT_HID_CCW(obj);
    VirtioCcwDevice *ccw_dev = VIRTIO_CCW_DEVICE(obj);

    ccw_dev->force_revision_1 = true;
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_KEYBOARD);
}

static void virtio_ccw_mouse_instance_init(Object *obj)
{
    VirtIOInputHIDCcw *dev = VIRTIO_INPUT_HID_CCW(obj);
    VirtioCcwDevice *ccw_dev = VIRTIO_CCW_DEVICE(obj);

    ccw_dev->force_revision_1 = true;
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_MOUSE);
}

static void virtio_ccw_tablet_instance_init(Object *obj)
{
    VirtIOInputHIDCcw *dev = VIRTIO_INPUT_HID_CCW(obj);
    VirtioCcwDevice *ccw_dev = VIRTIO_CCW_DEVICE(obj);

    ccw_dev->force_revision_1 = true;
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_TABLET);
}

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS(VirtIOInputCcw,
                                               TYPE_VIRTIO_INPUT_CCW,
                                               TYPE_VIRTIO_CCW_DEVICE,
                                               virtio_ccw_input_class_init)

REGISTER_QEMU_OBJECT_ABSTRACT_SIZED(VirtIOInputHIDCcw,
                                     TYPE_VIRTIO_INPUT_HID_CCW,
                                     TYPE_VIRTIO_INPUT_CCW)

REGISTER_QEMU_OBJECT_INIT_ONLY(virtio_ccw_keyboard, TYPE_VIRTIO_KEYBOARD_CCW,
                                TYPE_VIRTIO_INPUT_HID_CCW,
                                virtio_ccw_keyboard_instance_init)

REGISTER_QEMU_OBJECT_INIT_ONLY(virtio_ccw_mouse, TYPE_VIRTIO_MOUSE_CCW,
                                TYPE_VIRTIO_INPUT_HID_CCW,
                                virtio_ccw_mouse_instance_init)

REGISTER_QEMU_OBJECT_INIT_ONLY(virtio_ccw_tablet, TYPE_VIRTIO_TABLET_CCW,
                                TYPE_VIRTIO_INPUT_HID_CCW,
                                virtio_ccw_tablet_instance_init)
