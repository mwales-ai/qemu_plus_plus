/*
 * QEMU lowRISC Ibex Timer device
 *
 * Copyright (c) 2021 Western Digital
 *
 * For details check the documentation here:
 *    https://docs.opentitan.org/hw/ip/rv_timer/doc/
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/timer/ibex_timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "target/riscv/cpu.h"
#include "migration/vmstate.h"
#include "qom/cpp/object.h"

REG32(ALERT_TEST, 0x00)
    FIELD(ALERT_TEST, FATAL_FAULT, 0, 1)
REG32(CTRL, 0x04)
    FIELD(CTRL, ACTIVE, 0, 1)
REG32(CFG0, 0x100)
    FIELD(CFG0, PRESCALE, 0, 12)
    FIELD(CFG0, STEP, 16, 8)
REG32(LOWER0, 0x104)
REG32(UPPER0, 0x108)
REG32(COMPARE_LOWER0, 0x10C)
REG32(COMPARE_UPPER0, 0x110)
REG32(INTR_ENABLE, 0x114)
    FIELD(INTR_ENABLE, IE_0, 0, 1)
REG32(INTR_STATE, 0x118)
    FIELD(INTR_STATE, IS_0, 0, 1)
REG32(INTR_TEST, 0x11C)
    FIELD(INTR_TEST, T_0, 0, 1)

static uint64_t cpu_riscv_read_rtc(uint32_t timebase_freq)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    timebase_freq, NANOSECONDS_PER_SECOND);
}

static void ibex_timer_update_irqs(IbexTimerState *s)
{
    uint64_t value = s->timer_compare_lower0 |
                         ((uint64_t)s->timer_compare_upper0 << 32);
    uint64_t next, diff;
    uint64_t now = cpu_riscv_read_rtc(s->timebase_freq);

    if (!(s->timer_ctrl & R_CTRL_ACTIVE_MASK)) {
        /* Timer isn't active */
        return;
    }

    /* Update the CPUs mtimecmp */
    s->mtimecmp = value;

    if (s->mtimecmp <= now) {
        /*
         * If the mtimecmp was in the past raise the interrupt now.
         */
        qemu_irq_raise(s->m_timer_irq);
        if (s->timer_intr_enable & R_INTR_ENABLE_IE_0_MASK) {
            s->timer_intr_state |= R_INTR_STATE_IS_0_MASK;
            qemu_set_irq(s->irq, true);
        }
        return;
    }

    /* Setup a timer to trigger the interrupt in the future */
    qemu_irq_lower(s->m_timer_irq);
    qemu_set_irq(s->irq, false);

    diff = s->mtimecmp - now;
    next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                 muldiv64(diff,
                                          NANOSECONDS_PER_SECOND,
                                          s->timebase_freq);

    if (next < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        /* We overflowed the timer, just set it as large as we can */
        timer_mod(s->mtimer, 0x7FFFFFFFFFFFFFFF);
    } else {
        timer_mod(s->mtimer, next);
    }
}

static void ibex_timer_cb(void *opaque)
{
    IbexTimerState *s = static_cast<IbexTimerState *>(opaque);

    qemu_irq_raise(s->m_timer_irq);
    if (s->timer_intr_enable & R_INTR_ENABLE_IE_0_MASK) {
        s->timer_intr_state |= R_INTR_STATE_IS_0_MASK;
        qemu_set_irq(s->irq, true);
    }
}

void IbexTimerState::reset()
{
    mtimer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                          &ibex_timer_cb, this);
    mtimecmp = 0;

    timer_ctrl = 0x00000000;
    timer_cfg0 = 0x00010000;
    timer_compare_lower0 = 0xFFFFFFFF;
    timer_compare_upper0 = 0xFFFFFFFF;
    timer_intr_enable = 0x00000000;
    timer_intr_state = 0x00000000;

    ibex_timer_update_irqs(this);
}

static uint64_t ibex_timer_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    IbexTimerState *s = static_cast<IbexTimerState *>(opaque);
    uint64_t now = cpu_riscv_read_rtc(s->timebase_freq);
    uint64_t retvalue = 0;

    switch (addr >> 2) {
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                        "Attempted to read ALERT_TEST, a write only register");
        break;
    case R_CTRL:
        retvalue = s->timer_ctrl;
        break;
    case R_CFG0:
        retvalue = s->timer_cfg0;
        break;
    case R_LOWER0:
        retvalue = now;
        break;
    case R_UPPER0:
        retvalue = now >> 32;
        break;
    case R_COMPARE_LOWER0:
        retvalue = s->timer_compare_lower0;
        break;
    case R_COMPARE_UPPER0:
        retvalue = s->timer_compare_upper0;
        break;
    case R_INTR_ENABLE:
        retvalue = s->timer_intr_enable;
        break;
    case R_INTR_STATE:
        retvalue = s->timer_intr_state;
        break;
    case R_INTR_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Attempted to read INTR_TEST, a write only register");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }

    return retvalue;
}

static void ibex_timer_write(void *opaque, hwaddr addr,
                             uint64_t val64, unsigned int size)
{
    IbexTimerState *s = static_cast<IbexTimerState *>(opaque);
    uint32_t val = val64;

    switch (addr >> 2) {
    case R_ALERT_TEST:
        qemu_log_mask(LOG_UNIMP, "Alert triggering not supported");
        break;
    case R_CTRL:
        s->timer_ctrl = val;
        break;
    case R_CFG0:
        qemu_log_mask(LOG_UNIMP, "Changing prescale or step not supported");
        s->timer_cfg0 = val;
        break;
    case R_LOWER0:
        qemu_log_mask(LOG_UNIMP, "Changing timer value is not supported");
        break;
    case R_UPPER0:
        qemu_log_mask(LOG_UNIMP, "Changing timer value is not supported");
        break;
    case R_COMPARE_LOWER0:
        s->timer_compare_lower0 = val;
        ibex_timer_update_irqs(s);
        break;
    case R_COMPARE_UPPER0:
        s->timer_compare_upper0 = val;
        ibex_timer_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->timer_intr_enable = val;
        break;
    case R_INTR_STATE:
        /* Write 1 to clear */
        s->timer_intr_state &= ~val;
        break;
    case R_INTR_TEST:
        if (s->timer_intr_enable & val & R_INTR_ENABLE_IE_0_MASK) {
            s->timer_intr_state |= R_INTR_STATE_IS_0_MASK;
            qemu_set_irq(s->irq, true);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps ibex_timer_ops = {
    .read = ibex_timer_read,
    .write = ibex_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
};

static int ibex_timer_post_load(void *opaque, int version_id)
{
    IbexTimerState *s = static_cast<IbexTimerState *>(opaque);

    ibex_timer_update_irqs(s);
    return 0;
}

static const VMStateDescription vmstate_ibex_timer = {
    .name = TYPE_IBEX_TIMER,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = ibex_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(timer_ctrl, IbexTimerState),
        VMSTATE_UINT32(timer_cfg0, IbexTimerState),
        VMSTATE_UINT32(timer_compare_lower0, IbexTimerState),
        VMSTATE_UINT32(timer_compare_upper0, IbexTimerState),
        VMSTATE_UINT32(timer_intr_enable, IbexTimerState),
        VMSTATE_UINT32(timer_intr_state, IbexTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ibex_timer_properties[] = {
    DEFINE_PROP_UINT32("timebase-freq", IbexTimerState, timebase_freq, 10000),
};

void IbexTimerState::init()
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    sysbus_init_irq(sbd, &irq);

    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &ibex_timer_ops, this, TYPE_IBEX_TIMER, 0x400);
    sysbus_init_mmio(sbd, &mmio);
}

void IbexTimerState::realize(Error **errp)
{
    qdev_init_gpio_out(reinterpret_cast<DeviceState *>(this), &m_timer_irq, 1);
}

void IbexTimerState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_ibex_timer;
    device_class_set_props(dc, ibex_timer_properties);
}

REGISTER_QEMU_DEVICE(IbexTimerState, TYPE_IBEX_TIMER, TYPE_SYS_BUS_DEVICE)
