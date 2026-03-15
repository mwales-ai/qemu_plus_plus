/*
 * Virtio Support
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio.h"

/* Helper to cast vdev->config (void*) to uint8_t* for pointer arithmetic */
static inline uint8_t *vdev_config(VirtIODevice *vdev)
{
    return static_cast<uint8_t *>(vdev->config);
}

uint32_t virtio_config_readb(VirtIODevice *vdev, uint32_t addr)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint8_t val;

    if (addr + sizeof(val) > vdev->config_len) {
        return (uint32_t)-1;
    }

    k->get_config(vdev, vdev_config(vdev));

    val = ldub_p(vdev_config(vdev) + addr);
    return val;
}

uint32_t virtio_config_readw(VirtIODevice *vdev, uint32_t addr)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint16_t val;

    if (addr + sizeof(val) > vdev->config_len) {
        return (uint32_t)-1;
    }

    k->get_config(vdev, vdev_config(vdev));

    val = lduw_p(vdev_config(vdev) + addr);
    return val;
}

uint32_t virtio_config_readl(VirtIODevice *vdev, uint32_t addr)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint32_t val;

    if (addr + sizeof(val) > vdev->config_len) {
        return (uint32_t)-1;
    }

    k->get_config(vdev, vdev_config(vdev));

    val = ldl_p(vdev_config(vdev) + addr);
    return val;
}

void virtio_config_writeb(VirtIODevice *vdev, uint32_t addr, uint32_t data)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint8_t val = data;

    if (addr + sizeof(val) > vdev->config_len) {
        return;
    }

    stb_p(vdev_config(vdev) + addr, val);

    if (k->set_config) {
        k->set_config(vdev, vdev_config(vdev));
    }
}

void virtio_config_writew(VirtIODevice *vdev, uint32_t addr, uint32_t data)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint16_t val = data;

    if (addr + sizeof(val) > vdev->config_len) {
        return;
    }

    stw_p(vdev_config(vdev) + addr, val);

    if (k->set_config) {
        k->set_config(vdev, vdev_config(vdev));
    }
}

void virtio_config_writel(VirtIODevice *vdev, uint32_t addr, uint32_t data)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint32_t val = data;

    if (addr + sizeof(val) > vdev->config_len) {
        return;
    }

    stl_p(vdev_config(vdev) + addr, val);

    if (k->set_config) {
        k->set_config(vdev, vdev_config(vdev));
    }
}

uint32_t virtio_config_modern_readb(VirtIODevice *vdev, uint32_t addr)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint8_t val;

    if (addr + sizeof(val) > vdev->config_len) {
        return (uint32_t)-1;
    }

    k->get_config(vdev, vdev_config(vdev));

    val = ldub_p(vdev_config(vdev) + addr);
    return val;
}

uint32_t virtio_config_modern_readw(VirtIODevice *vdev, uint32_t addr)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint16_t val;

    if (addr + sizeof(val) > vdev->config_len) {
        return (uint32_t)-1;
    }

    k->get_config(vdev, vdev_config(vdev));

    val = lduw_le_p(vdev_config(vdev) + addr);
    return val;
}

uint32_t virtio_config_modern_readl(VirtIODevice *vdev, uint32_t addr)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint32_t val;

    if (addr + sizeof(val) > vdev->config_len) {
        return (uint32_t)-1;
    }

    k->get_config(vdev, vdev_config(vdev));

    val = ldl_le_p(vdev_config(vdev) + addr);
    return val;
}

void virtio_config_modern_writeb(VirtIODevice *vdev,
                                 uint32_t addr, uint32_t data)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint8_t val = data;

    if (addr + sizeof(val) > vdev->config_len) {
        return;
    }

    stb_p(vdev_config(vdev) + addr, val);

    if (k->set_config) {
        k->set_config(vdev, vdev_config(vdev));
    }
}

void virtio_config_modern_writew(VirtIODevice *vdev,
                                 uint32_t addr, uint32_t data)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint16_t val = data;

    if (addr + sizeof(val) > vdev->config_len) {
        return;
    }

    stw_le_p(vdev_config(vdev) + addr, val);

    if (k->set_config) {
        k->set_config(vdev, vdev_config(vdev));
    }
}

void virtio_config_modern_writel(VirtIODevice *vdev,
                                 uint32_t addr, uint32_t data)
{
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint32_t val = data;

    if (addr + sizeof(val) > vdev->config_len) {
        return;
    }

    stl_le_p(vdev_config(vdev) + addr, val);

    if (k->set_config) {
        k->set_config(vdev, vdev_config(vdev));
    }
}

