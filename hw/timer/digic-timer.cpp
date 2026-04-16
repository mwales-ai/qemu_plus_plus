/*
 * QEMU model of the Canon DIGIC timer block.
 *
 * Copyright (C) 2013 Antony Pavlov <antonynpavlov@gmail.com>
 *
 * This model is based on reverse engineering efforts
 * made by CHDK (http://chdk.wikia.com) and
 * Magic Lantern (http://www.magiclantern.fm) projects
 * contributors.
 *
 * See "Timer/Clock Module" docs here:
 *   http://magiclantern.wikia.com/wiki/Register_Map
 *
 * The QEMU model of the OSTimer in PKUnity SoC by Guan Xuetao
 * is used as a template.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/ptimer.h"
#include "qemu/module.h"
#include "qemu/log.h"

#include "hw/timer/digic-timer.h"
#include "migration/vmstate.h"
#include "qom/cpp/object.h"

static const VMStateField vmstate_digic_timer_fields[] = {
    VMSTATE_PTIMER(ptimer, DigicTimerState),
    VMSTATE_UINT32(control, DigicTimerState),
    VMSTATE_UINT32(relvalue, DigicTimerState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_digic_timer = {
    .name = "digic.timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_digic_timer_fields,
};

void DigicTimerState::reset()
{
    ptimer_transaction_begin(ptimer);
    ptimer_stop(ptimer);
    ptimer_transaction_commit(ptimer);
    control = 0;
    relvalue = 0;
}

static uint64_t digic_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    DigicTimerState *s = static_cast<DigicTimerState *>(opaque);
    uint64_t ret = 0;

    switch (offset) {
    case DIGIC_TIMER_CONTROL:
        ret = s->control;
        break;
    case DIGIC_TIMER_RELVALUE:
        ret = s->relvalue;
        break;
    case DIGIC_TIMER_VALUE:
        ret = ptimer_get_count(s->ptimer) & 0xffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "digic-timer: read access to unknown register 0x"
                      HWADDR_FMT_plx "\n", offset);
    }

    return ret;
}

static void digic_timer_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    DigicTimerState *s = static_cast<DigicTimerState *>(opaque);

    switch (offset) {
    case DIGIC_TIMER_CONTROL:
        if (value & DIGIC_TIMER_CONTROL_RST) {
            s->reset();
            break;
        }

        ptimer_transaction_begin(s->ptimer);
        if (value & DIGIC_TIMER_CONTROL_EN) {
            ptimer_run(s->ptimer, 0);
        }

        s->control = (uint32_t)value;
        ptimer_transaction_commit(s->ptimer);
        break;

    case DIGIC_TIMER_RELVALUE:
        s->relvalue = extract32(value, 0, 16);
        ptimer_transaction_begin(s->ptimer);
        ptimer_set_limit(s->ptimer, s->relvalue, 1);
        ptimer_transaction_commit(s->ptimer);
        break;

    case DIGIC_TIMER_VALUE:
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "digic-timer: read access to unknown register 0x"
                      HWADDR_FMT_plx "\n", offset);
    }
}

static const MemoryRegionOps digic_timer_ops = {
    .read = digic_timer_read,
    .write = digic_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void digic_timer_tick(void *opaque)
{
    /* Nothing to do on timer rollover */
}

void DigicTimerState::init()
{
    ptimer = ptimer_init(digic_timer_tick, NULL, PTIMER_POLICY_LEGACY);

    /*
     * FIXME: there is no documentation on Digic timer
     * frequency setup so let it always run at 1 MHz
     */
    ptimer_transaction_begin(ptimer);
    ptimer_set_freq(ptimer, 1 * 1000 * 1000);
    ptimer_transaction_commit(ptimer);

    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &digic_timer_ops, this, TYPE_DIGIC_TIMER, 0x100);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

void DigicTimerState::finalize()
{
    ptimer_free(ptimer);
}

void DigicTimerState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_digic_timer;
}

REGISTER_QEMU_DEVICE(DigicTimerState, TYPE_DIGIC_TIMER, TYPE_SYS_BUS_DEVICE)
