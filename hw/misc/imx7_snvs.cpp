/*
 * IMX7 Secure Non-Volatile Storage
 *
 * Copyright (c) 2018, Impinj, Inc.
 *
 * Author: Andrey Smirnov <andrew.smirnov@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * Bare minimum emulation code needed to support being able to shut
 * down linux guest gracefully.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/misc/imx7_snvs.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qom/cpp/object.h"
#include "system/system.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "trace.h"

#define RTC_FREQ    32768ULL

static const VMStateField vmstate_imx7_snvs_fields[] = {
        VMSTATE_UINT64(tick_offset, IMX7SNVSState),
        VMSTATE_UINT64(lpcr, IMX7SNVSState),
        VMSTATE_END_OF_LIST()
    };

static const VMStateDescription vmstate_imx7_snvs = {
    .name = TYPE_IMX7_SNVS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_imx7_snvs_fields,
};

static uint64_t imx7_snvs_get_count(IMX7SNVSState *s)
{
    uint64_t ticks = muldiv64(qemu_clock_get_ns(rtc_clock), RTC_FREQ,
                              NANOSECONDS_PER_SECOND);
    return s->tick_offset + ticks;
}

static uint64_t imx7_snvs_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX7SNVSState *s = IMX7_SNVS(opaque);
    uint64_t ret = 0;

    switch (offset) {
    case SNVS_LPSRTCMR:
        ret = extract64(imx7_snvs_get_count(s), 32, 15);
        break;
    case SNVS_LPSRTCLR:
        ret = extract64(imx7_snvs_get_count(s), 0, 32);
        break;
    case SNVS_LPCR:
        ret = s->lpcr;
        break;
    }

    trace_imx7_snvs_read(offset, ret, size);

    return ret;
}

void IMX7SNVSState::reset()
{
    lpcr = 0;
}

static void imx7_snvs_write(void *opaque, hwaddr offset,
                            uint64_t v, unsigned size)
{
    trace_imx7_snvs_write(offset, v, size);

    IMX7SNVSState *s = IMX7_SNVS(opaque);

    uint64_t new_value = 0, snvs_count = 0;

    if (offset == SNVS_LPSRTCMR || offset == SNVS_LPSRTCLR) {
        snvs_count = imx7_snvs_get_count(s);
    }

    switch (offset) {
    case SNVS_LPSRTCMR:
        new_value = deposit64(snvs_count, 32, 32, v);
        break;
    case SNVS_LPSRTCLR:
        new_value = deposit64(snvs_count, 0, 32, v);
        break;
    case SNVS_LPCR: {
        s->lpcr = v;

        const uint32_t mask  = SNVS_LPCR_TOP | SNVS_LPCR_DP_EN;

        if ((v & mask) == mask) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        break;
    }
    }

    if (offset == SNVS_LPSRTCMR || offset == SNVS_LPSRTCLR) {
        s->tick_offset += new_value - snvs_count;
    }
}

static const struct MemoryRegionOps imx7_snvs_ops = {
    .read = imx7_snvs_read,
    .write = imx7_snvs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        /*
         * Our device would not work correctly if the guest was doing
         * unaligned access. This might not be a limitation on the real
         * device but in practice there is no reason for a guest to access
         * this device unaligned.
         */
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void IMX7SNVSState::init()
{
    struct tm tm;

    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &imx7_snvs_ops, this, TYPE_IMX7_SNVS, 0x1000);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &mmio);

    qemu_get_timedate(&tm, 0);
    tick_offset = mktimegm(&tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

void IMX7SNVSState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_imx7_snvs;
    dc->desc  = "i.MX7 Secure Non-Volatile Storage Module";
}

REGISTER_QEMU_DEVICE(IMX7SNVSState, TYPE_IMX7_SNVS, TYPE_SYS_BUS_DEVICE)
