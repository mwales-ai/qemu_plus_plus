/*
 * virtio ccw serial implementation
 *
 * Copyright 2012, 2015 IBM Corp.
 * Author(s): Cornelia Huck <cornelia.huck@de.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-serial.h"
#include "virtio-ccw.h"

#define TYPE_VIRTIO_SERIAL_CCW "virtio-serial-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(VirtioSerialCcw, VIRTIO_SERIAL_CCW)

struct VirtioSerialCcw {
    VirtioCcwDevice parent_obj;
    VirtIOSerial vdev;

#ifdef __cplusplus
    void init();
    static void classInit(DeviceClass *dc);
#endif
};

static void virtio_ccw_serial_realize(VirtioCcwDevice *ccw_dev, Error **errp)
{
    VirtioSerialCcw *dev = VIRTIO_SERIAL_CCW(ccw_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    DeviceState *proxy = DEVICE(ccw_dev);
    char *bus_name;

    /*
     * For command line compatibility, this sets the virtio-serial-device bus
     * name as before.
     */
    if (proxy->id) {
        bus_name = g_strdup_printf("%s.0", proxy->id);
        virtio_device_set_child_bus_name(VIRTIO_DEVICE(vdev), bus_name);
        g_free(bus_name);
    }

    qdev_realize(vdev, BUS(&ccw_dev->bus), errp);
}


void VirtioSerialCcw::init()
{
    virtio_instance_init_common(OBJECT(this), &vdev, sizeof(vdev),
                                TYPE_VIRTIO_SERIAL);
}

static const Property virtio_ccw_serial_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtioCcwDevice, flags,
                    VIRTIO_CCW_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("max_revision", VirtioCcwDevice, max_rev,
                       VIRTIO_CCW_MAX_REV),
};

void VirtioSerialCcw::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    VirtIOCCWDeviceClass *k = VIRTIO_CCW_DEVICE_CLASS(klass);

    k->realize = virtio_ccw_serial_realize;
    device_class_set_props(dc, virtio_ccw_serial_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VirtioSerialCcw, TYPE_VIRTIO_SERIAL_CCW, TYPE_VIRTIO_CCW_DEVICE)
