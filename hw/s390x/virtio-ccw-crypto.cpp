/*
 * virtio ccw crypto implementation
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
#include "hw/virtio/virtio-crypto.h"

#define TYPE_VIRTIO_CRYPTO_CCW "virtio-crypto-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOCryptoCcw, VIRTIO_CRYPTO_CCW)

struct VirtIOCryptoCcw {
    VirtioCcwDevice parent_obj;
    VirtIOCrypto vdev;

#ifdef __cplusplus
    void init();
    static void classInit(DeviceClass *dc);
#endif
};

static void virtio_ccw_crypto_realize(VirtioCcwDevice *ccw_dev, Error **errp)
{
    VirtIOCryptoCcw *dev = VIRTIO_CRYPTO_CCW(ccw_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    if (!qdev_realize(vdev, BUS(&ccw_dev->bus), errp)) {
        return;
    }
}

void VirtIOCryptoCcw::init()
{
    VirtioCcwDevice *ccw_dev = VIRTIO_CCW_DEVICE(this);

    ccw_dev->force_revision_1 = true;
    virtio_instance_init_common(OBJECT(this), &vdev, sizeof(vdev),
                                TYPE_VIRTIO_CRYPTO);
}

static const Property virtio_ccw_crypto_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtioCcwDevice, flags,
                    VIRTIO_CCW_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("max_revision", VirtioCcwDevice, max_rev,
                       VIRTIO_CCW_MAX_REV),
};

void VirtIOCryptoCcw::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    VirtIOCCWDeviceClass *k = VIRTIO_CCW_DEVICE_CLASS(klass);

    k->realize = virtio_ccw_crypto_realize;
    device_class_set_props(dc, virtio_ccw_crypto_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VirtIOCryptoCcw, TYPE_VIRTIO_CRYPTO_CCW, TYPE_VIRTIO_CCW_DEVICE)
