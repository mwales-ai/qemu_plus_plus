/*
 * BCM2835 One-Time Programmable (OTP) Memory
 *
 * The OTP implementation is mostly a stub except for the OTP rows
 * which are accessed directly by other peripherals such as the mailbox.
 *
 * The OTP registers are unimplemented due to lack of documentation.
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/log.h"
#include "hw/nvram/bcm2835_otp.h"
#include "migration/vmstate.h"
}
#include "qom/cpp/object.h"

extern "C"
uint32_t bcm2835_otp_get_row(BCM2835OTPState *s, unsigned int row)
{
    assert(row <= BCM2835_OTP_ROW_COUNT && row >= 1);

    return s->otp_rows[row - 1];
}

extern "C"
void bcm2835_otp_set_row(BCM2835OTPState *s, unsigned int row,
                           uint32_t value)
{
    assert(row <= BCM2835_OTP_ROW_COUNT && row >= 1);

    /* Real OTP rows work as e-fuses */
    s->otp_rows[row - 1] |= value;
}

static uint64_t bcm2835_otp_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case BCM2835_OTP_BOOTMODE_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BOOTMODE_REG\n");
        break;
    case BCM2835_OTP_CONFIG_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CONFIG_REG\n");
        break;
    case BCM2835_OTP_CTRL_LO_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_LO_REG\n");
        break;
    case BCM2835_OTP_CTRL_HI_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_HI_REG\n");
        break;
    case BCM2835_OTP_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_STATUS_REG\n");
        break;
    case BCM2835_OTP_BITSEL_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BITSEL_REG\n");
        break;
    case BCM2835_OTP_DATA_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_DATA_REG\n");
        break;
    case BCM2835_OTP_ADDR_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_ADDR_REG\n");
        break;
    case BCM2835_OTP_WRITE_DATA_READ_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_WRITE_DATA_READ_REG\n");
        break;
    case BCM2835_OTP_INIT_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_INIT_STATUS_REG\n");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }

    return 0;
}

static void bcm2835_otp_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    switch (addr) {
    case BCM2835_OTP_BOOTMODE_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BOOTMODE_REG\n");
        break;
    case BCM2835_OTP_CONFIG_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CONFIG_REG\n");
        break;
    case BCM2835_OTP_CTRL_LO_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_LO_REG\n");
        break;
    case BCM2835_OTP_CTRL_HI_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_HI_REG\n");
        break;
    case BCM2835_OTP_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_STATUS_REG\n");
        break;
    case BCM2835_OTP_BITSEL_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BITSEL_REG\n");
        break;
    case BCM2835_OTP_DATA_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_DATA_REG\n");
        break;
    case BCM2835_OTP_ADDR_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_ADDR_REG\n");
        break;
    case BCM2835_OTP_WRITE_DATA_READ_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_WRITE_DATA_READ_REG\n");
        break;
    case BCM2835_OTP_INIT_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_INIT_STATUS_REG\n");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static MemoryRegionOps bcm2835_otp_ops;

static void __attribute__((constructor)) init_bcm2835_otp_ops(void)
{
    memset(&bcm2835_otp_ops, 0, sizeof(bcm2835_otp_ops));
    bcm2835_otp_ops.read = bcm2835_otp_read;
    bcm2835_otp_ops.write = bcm2835_otp_write;
    bcm2835_otp_ops.endianness = DEVICE_NATIVE_ENDIAN;
    bcm2835_otp_ops.impl.min_access_size = 4;
    bcm2835_otp_ops.impl.max_access_size = 4;
}

void BCM2835OTPState::realize(Error **errp)
{
    memory_region_init_io(&iomem, OBJECT(this), &bcm2835_otp_ops, this,
                          TYPE_BCM2835_OTP, 0x80);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);

    memset(otp_rows, 0x00, sizeof(otp_rows));
}

static const VMStateField vmstate_bcm2835_otp_fields[] = {
    VMSTATE_UINT32_ARRAY(otp_rows, BCM2835OTPState, BCM2835_OTP_ROW_COUNT),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_bcm2835_otp = {
    .name = TYPE_BCM2835_OTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_bcm2835_otp_fields,
};

void BCM2835OTPState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_bcm2835_otp;
}

REGISTER_QEMU_DEVICE(BCM2835OTPState, TYPE_BCM2835_OTP, TYPE_SYS_BUS_DEVICE)
