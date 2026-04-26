/*
 * Vhost-user GPIO virtio device
 *
 * Copyright (c) 2022 Viresh Kumar <viresh.kumar@linaro.org>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/vhost-user-gpio.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_gpio.h"

static const Property vgpio_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserBase, chardev),
};

static void vgpio_realize(DeviceState *dev, Error **errp)
{
    VHostUserBase *vub = VHOST_USER_BASE(dev);
    VHostUserBaseClass *vubc = VHOST_USER_BASE_GET_CLASS(dev);

    /* Fixed for GPIO */
    vub->virtio_id = VIRTIO_ID_GPIO;
    vub->num_vqs = 2;
    vub->config_size = sizeof(struct virtio_gpio_config);

    vubc->parent_realize(dev, errp);
}

static const VMStateDescription vu_gpio_vmstate = {
    .name = "vhost-user-gpio",
    .unmigratable = 1,
};

void VHostUserGPIO::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    VHostUserBaseClass *vubc = VHOST_USER_BASE_CLASS(klass);

    dc->vmsd = &vu_gpio_vmstate;
    device_class_set_props(dc, vgpio_properties);
    device_class_set_parent_realize(dc, vgpio_realize,
                                    &vubc->parent_realize);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VHostUserGPIO, TYPE_VHOST_USER_GPIO, TYPE_VHOST_USER_BASE)
