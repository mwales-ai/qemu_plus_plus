/*
 * Generic vhost-user-test-device implementation for any vhost-user-backend
 *
 * This is a concrete implementation of vhost-user-base which can be
 * configured via properties. It is useful for development and
 * prototyping. It expects configuration details (if any) to be
 * handled by the vhost-user daemon itself.
 *
 * Copyright (c) 2023 Linaro Ltd
 * Author: Alex Bennée <alex.bennee@linaro.org>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/vhost-user-base.h"
#include "qemu/error-report.h"
#include "qom/cpp/object.h"

/*
 * The following is a concrete implementation of the base class which
 * allows the user to define the key parameters via the command line.
 */

static const VMStateDescription vud_vmstate = {
    .name = "vhost-user-test-device",
    .unmigratable = 1,
};

static const Property vud_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserBase, chardev),
    DEFINE_PROP_UINT16("virtio-id", VHostUserBase, virtio_id, 0),
    DEFINE_PROP_UINT32("vq_size", VHostUserBase, vq_size, 64),
    DEFINE_PROP_UINT32("num_vqs", VHostUserBase, num_vqs, 1),
    DEFINE_PROP_UINT32("config_size", VHostUserBase, config_size, 0),
};

static void vud_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, vud_properties);
    dc->vmsd = &vud_vmstate;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

REGISTER_QEMU_DEVICE_CUSTOM_CI(VHostUserBase, VHostUserBaseClass,
                               TYPE_VHOST_USER_TEST_DEVICE,
                               TYPE_VHOST_USER_BASE,
                               vud_class_init)
