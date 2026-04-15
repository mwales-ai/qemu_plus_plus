/*
 * ARM SBSA Reference Platform Embedded Controller
 *
 * A device to allow PSCI running in the secure side of sbsa-ref machine
 * to communicate platform power states to qemu.
 *
 * Copyright (c) 2020 Nuvia Inc
 * Written by Graeme Gregory <graeme@nuviainc.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qemu/log.h"
#include "hw/sysbus.h"
#include "system/runstate.h"
#include "qom/cpp/object.h"

#define TYPE_SBSA_SECURE_EC "sbsa-ec"

typedef struct SECUREECState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    void init();
    static void classInit(DeviceClass *dc);
} SECUREECState;

OBJECT_DECLARE_SIMPLE_TYPE(SECUREECState, SBSA_SECURE_EC)

enum sbsa_ec_powerstates {
    SBSA_EC_CMD_POWEROFF = 0x01,
    SBSA_EC_CMD_REBOOT = 0x02,
};

static uint64_t sbsa_ec_read(void *opaque, hwaddr offset, unsigned size)
{
    /* No use for this currently */
    qemu_log_mask(LOG_GUEST_ERROR, "sbsa-ec: no readable registers");
    return 0;
}

static void sbsa_ec_write(void *opaque, hwaddr offset,
                          uint64_t value, unsigned size)
{
    if (offset == 0) { /* PSCI machine power command register */
        switch (value) {
        case SBSA_EC_CMD_POWEROFF:
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            break;
        case SBSA_EC_CMD_REBOOT:
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sbsa-ec: unknown power command");
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "sbsa-ec: unknown EC register");
    }
}

static const MemoryRegionOps sbsa_ec_ops = {
    .read = sbsa_ec_read,
    .write = sbsa_ec_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

void SECUREECState::init()
{
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &sbsa_ec_ops, this, "sbsa-ec", 0x1000);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

void SECUREECState::classInit(DeviceClass *dc)
{
    /* No vmstate or reset required: device has no internal state */
    dc->user_creatable = false;
}

REGISTER_QEMU_DEVICE(SECUREECState, TYPE_SBSA_SECURE_EC, TYPE_SYS_BUS_DEVICE)
