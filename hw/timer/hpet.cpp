/*
 *  High Precision Event Timer emulation
 *
 *  Copyright (c) 2007 Alexander Graf
 *  Copyright (c) 2008 IBM Corporation
 *
 *  Authors: Beth Kon <bkon@us.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************
 *
 * This driver attempts to emulate an HPET device in software.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/qdev-properties.h"
#include "hw/timer/hpet.h"
#include "hw/sysbus.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "migration/vmstate.h"
#include "hw/timer/i8254.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "qemu/lockable.h"
#include "qemu/seqlock.h"
#include "qemu/main-loop.h"
#include "trace.h"

struct hpet_fw_config hpet_fw_cfg = {.count = UINT8_MAX};

#define HPET_MSI_SUPPORT        0

OBJECT_DECLARE_SIMPLE_TYPE(HPETState, HPET)

struct HPETState;
typedef struct HPETTimer {  /* timers */
    uint8_t tn;             /*timer number*/
    QEMUTimer *qemu_timer;
    struct HPETState *state;
    /* Memory-mapped, software visible timer registers */
    uint64_t config;        /* configuration/cap */
    uint64_t cmp;           /* comparator */
    uint64_t fsb;           /* FSB route */
    /* Hidden register state */
    uint64_t cmp64;         /* comparator (extended to counter width) */
    uint64_t period;        /* Last value written to comparator */
    uint8_t wrap_flag;      /* timer pop will indicate wrap for one-shot 32-bit
                             * mode. Next pop will be actual timer expiration.
                             */
    uint64_t last;          /* last value armed, to avoid timer storms */

    /* ----- HPETTimer methods ----- */
    uint32_t intRoute();
    uint32_t fsbRoute();
    uint32_t isPeriodic();
    uint32_t isEnabled();
    uint64_t calculateCmp64(uint64_t cur_tick, uint64_t target);
} HPETTimer;

struct HPETState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    QemuMutex lock;
    MemoryRegion iomem;
    uint64_t hpet_offset;
    bool hpet_offset_saved;
    QemuSeqLock state_version;
    qemu_irq irqs[HPET_NUM_IRQ_ROUTES];
    uint32_t flags;
    uint8_t rtc_irq_level;
    qemu_irq pit_enabled;
    uint8_t num_timers;
    uint8_t num_timers_save;
    uint32_t intcap;
    HPETTimer timer[HPET_MAX_TIMERS];

    /* Memory-mapped, software visible registers */
    uint64_t capability;        /* capabilities */
    uint64_t config;            /* configuration */
    uint64_t isr;               /* interrupt status reg */
    uint64_t hpet_counter;      /* main counter */
    uint8_t  hpet_id;           /* instance id */

    /* ----- helper methods ----- */
    uint32_t inLegacyMode();
    uint32_t isEnabled();
    uint64_t getTicks();
    uint64_t getNs(uint64_t tick);

    void updateIrq(HPETTimer *t, int set);
    void armTimer(HPETTimer *t, uint64_t tick);
    void setTimer(HPETTimer *t);
    void delTimer(HPETTimer *t);

    /* ----- QOM callbacks ----- */
    void realize(DeviceState *dev, Error **errp);
    void reset(DeviceState *d);
    void initInstance(Object *obj);

    static void classInit(ObjectClass *klass, const void *data);

    /* static MMIO callbacks */
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size);

    /* static GPIO callback */
    static void handleLegacyIrq(void *opaque, int n, int level);

    /* static timer callback */
    static void timerCallback(void *opaque);

    /* static VMState callbacks */
    static int preSave(void *opaque);
    static bool validateNumTimers(void *opaque, int version_id);
    static int postLoad(void *opaque, int version_id);
    static bool offsetNeeded(void *opaque);
    static bool rtcIrqLevelNeeded(void *opaque);
};

/* ----- HPETTimer methods ----- */

uint32_t HPETTimer::intRoute()
{
    return (config & HPET_TN_INT_ROUTE_MASK) >> HPET_TN_INT_ROUTE_SHIFT;
}

uint32_t HPETTimer::fsbRoute()
{
    return config & HPET_TN_FSB_ENABLE;
}

uint32_t HPETTimer::isPeriodic()
{
    return config & HPET_TN_PERIODIC;
}

uint32_t HPETTimer::isEnabled()
{
    return config & HPET_TN_ENABLE;
}

/*
 * calculate next value of the general counter that matches the
 * target (either entirely, or the low 32-bit only depending on
 * the timer mode).
 */
uint64_t HPETTimer::calculateCmp64(uint64_t cur_tick, uint64_t target)
{
    if (config & HPET_TN_32BIT) {
        uint64_t result = deposit64(cur_tick, 0, 32, target);
        if (result < cur_tick) {
            result += 0x100000000ULL;
        }
        return result;
    } else {
        return target;
    }
}

/* ----- pure utility functions ----- */

static uint32_t hpet_time_after(uint64_t a, uint64_t b)
{
    return ((int64_t)(b - a) < 0);
}

static uint64_t ticks_to_ns(uint64_t value)
{
    return value * HPET_CLK_PERIOD;
}

static uint64_t ns_to_ticks(uint64_t value)
{
    return value / HPET_CLK_PERIOD;
}

static uint64_t hpet_fixup_reg(uint64_t new_val, uint64_t old, uint64_t mask)
{
    new_val &= mask;
    new_val |= old & ~mask;
    return new_val;
}

static int activating_bit(uint64_t old, uint64_t new_val, uint64_t mask)
{
    return (!(old & mask) && (new_val & mask));
}

static int deactivating_bit(uint64_t old, uint64_t new_val, uint64_t mask)
{
    return ((old & mask) && !(new_val & mask));
}

static uint64_t hpet_next_wrap(uint64_t cur_tick)
{
    return (cur_tick | 0xffffffffU) + 1;
}

/* ----- HPETState methods ----- */

uint32_t HPETState::inLegacyMode()
{
    return config & HPET_CFG_LEGACY;
}

uint32_t HPETState::isEnabled()
{
    return config & HPET_CFG_ENABLE;
}

uint64_t HPETState::getTicks()
{
    return ns_to_ticks(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + hpet_offset);
}

uint64_t HPETState::getNs(uint64_t tick)
{
    return ticks_to_ns(tick) - hpet_offset;
}

void HPETState::updateIrq(HPETTimer *t, int set)
{
    uint64_t mask;
    int route;

    if (t->tn <= 1 && inLegacyMode()) {
        /* if LegacyReplacementRoute bit is set, HPET specification requires
         * timer0 be routed to IRQ0 in NON-APIC or IRQ2 in the I/O APIC,
         * timer1 be routed to IRQ8 in NON-APIC or IRQ8 in the I/O APIC.
         */
        route = (t->tn == 0) ? 0 : RTC_ISA_IRQ;
    } else {
        route = t->intRoute();
    }
    mask = 1 << t->tn;

    if (set && (t->config & HPET_TN_TYPE_LEVEL)) {
        /*
         * If HPET_TN_ENABLE bit is 0, "the timer will still operate and
         * generate appropriate status bits, but will not cause an interrupt"
         */
        isr |= mask;
    } else {
        isr &= ~mask;
    }

    if (set && t->isEnabled() && isEnabled()) {
        if (t->fsbRoute()) {
            address_space_stl_le(&address_space_memory, t->fsb >> 32,
                                 t->fsb & 0xffffffff, MEMTXATTRS_UNSPECIFIED,
                                 NULL);
        } else if (t->config & HPET_TN_TYPE_LEVEL) {
            BQL_LOCK_GUARD();
            qemu_irq_raise(irqs[route]);
        } else {
            BQL_LOCK_GUARD();
            qemu_irq_pulse(irqs[route]);
        }
    } else {
        if (!t->fsbRoute()) {
            BQL_LOCK_GUARD();
            qemu_irq_lower(irqs[route]);
        }
    }
}

int HPETState::preSave(void *opaque)
{
    HPETState *s = static_cast<HPETState *>(opaque);

    /* save current counter value */
    if (s->isEnabled()) {
        s->hpet_counter = s->getTicks();
    }

    /*
     * The number of timers must match on source and destination, but it was
     * also added to the migration stream.  Check that it matches the value
     * that was configured.
     */
    s->num_timers_save = s->num_timers;
    return 0;
}

bool HPETState::validateNumTimers(void *opaque, int version_id)
{
    HPETState *s = static_cast<HPETState *>(opaque);

    return s->num_timers == s->num_timers_save;
}

int HPETState::postLoad(void *opaque, int version_id)
{
    HPETState *s = static_cast<HPETState *>(opaque);
    int i;

    for (i = 0; i < s->num_timers; i++) {
        HPETTimer *t = &s->timer[i];
        t->cmp64 = t->calculateCmp64(s->hpet_counter, t->cmp);
        t->last = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - NANOSECONDS_PER_SECOND;
    }
    /* Recalculate the offset between the main counter and guest time */
    if (!s->hpet_offset_saved) {
        s->hpet_offset = ticks_to_ns(s->hpet_counter)
                        - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }

    return 0;
}

bool HPETState::offsetNeeded(void *opaque)
{
    HPETState *s = static_cast<HPETState *>(opaque);

    return s->isEnabled() && s->hpet_offset_saved;
}

bool HPETState::rtcIrqLevelNeeded(void *opaque)
{
    HPETState *s = static_cast<HPETState *>(opaque);

    return s->rtc_irq_level != 0;
}

static const VMStateField vmstate_hpet_rtc_irq_level_fields[] = {
    VMSTATE_UINT8(rtc_irq_level, HPETState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_hpet_rtc_irq_level = {
    .name = "hpet/rtc_irq_level",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = HPETState::rtcIrqLevelNeeded,
    .fields = vmstate_hpet_rtc_irq_level_fields,
};

static const VMStateField vmstate_hpet_offset_fields[] = {
    VMSTATE_UINT64(hpet_offset, HPETState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_hpet_offset = {
    .name = "hpet/offset",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = HPETState::offsetNeeded,
    .fields = vmstate_hpet_offset_fields,
};

static const VMStateField vmstate_hpet_timer_fields[] = {
    VMSTATE_UINT8(tn, HPETTimer),
    VMSTATE_UINT64(config, HPETTimer),
    VMSTATE_UINT64(cmp, HPETTimer),
    VMSTATE_UINT64(fsb, HPETTimer),
    VMSTATE_UINT64(period, HPETTimer),
    VMSTATE_UINT8(wrap_flag, HPETTimer),
    VMSTATE_TIMER_PTR(qemu_timer, HPETTimer),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_hpet_timer = {
    .name = "hpet_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_hpet_timer_fields,
};

static const VMStateField vmstate_hpet_fields[] = {
    VMSTATE_UINT64(config, HPETState),
    VMSTATE_UINT64(isr, HPETState),
    VMSTATE_UINT64(hpet_counter, HPETState),
    VMSTATE_UINT8(num_timers_save, HPETState),
    VMSTATE_VALIDATE("num_timers must match", HPETState::validateNumTimers),
    VMSTATE_STRUCT_VARRAY_UINT8(timer, HPETState, num_timers_save, 0,
                                vmstate_hpet_timer, HPETTimer),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription * const vmstate_hpet_subsections[] = {
    &vmstate_hpet_rtc_irq_level,
    &vmstate_hpet_offset,
    NULL
};

static const VMStateDescription vmstate_hpet = {
    .name = "hpet",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = HPETState::postLoad,
    .pre_save = HPETState::preSave,
    .fields = vmstate_hpet_fields,
    .subsections = vmstate_hpet_subsections,
};

void HPETState::armTimer(HPETTimer *t, uint64_t tick)
{
    uint64_t ns = getNs(tick);

    /* Clamp period to reasonable min value (1 us) */
    if (t->isPeriodic() && ns - t->last < 1000) {
        ns = t->last + 1000;
    }

    t->last = ns;
    timer_mod(t->qemu_timer, ns);
}

/*
 * timer expiration callback
 */
void HPETState::timerCallback(void *opaque)
{
    HPETTimer *t = static_cast<HPETTimer *>(opaque);
    HPETState *s = t->state;
    uint64_t period = t->period;
    uint64_t cur_tick = s->getTicks();

    if (t->isPeriodic() && period != 0) {
        while (hpet_time_after(cur_tick, t->cmp64)) {
            t->cmp64 += period;
        }
        if (t->config & HPET_TN_32BIT) {
            t->cmp = (uint32_t)t->cmp64;
        } else {
            t->cmp = t->cmp64;
        }
        s->armTimer(t, t->cmp64);
    } else if (t->wrap_flag) {
        t->wrap_flag = 0;
        s->armTimer(t, t->cmp64);
    }
    s->updateIrq(t, 1);
}

void HPETState::setTimer(HPETTimer *t)
{
    uint64_t cur_tick = getTicks();

    t->wrap_flag = 0;
    t->cmp64 = t->calculateCmp64(cur_tick, t->cmp);
    if (t->config & HPET_TN_32BIT) {

        /* hpet spec says in one-shot 32-bit mode, generate an interrupt when
         * counter wraps in addition to an interrupt with comparator match.
         */
        if (!t->isPeriodic() && t->cmp64 > hpet_next_wrap(cur_tick)) {
            t->wrap_flag = 1;
            armTimer(t, hpet_next_wrap(cur_tick));
            return;
        }
    }
    armTimer(t, t->cmp64);
}

void HPETState::delTimer(HPETTimer *t)
{
    timer_del(t->qemu_timer);

    if (isr & (1 << t->tn)) {
        /* For level-triggered interrupt, this leaves ISR set but lowers irq.  */
        updateIrq(t, 1);
    }
}

uint64_t HPETState::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    HPETState *s = static_cast<HPETState *>(opaque);
    int shift = (addr & 4) * 8;
    uint64_t cur_tick;

    trace_hpet_ram_read(addr);
    addr &= ~4;

    if (addr == HPET_COUNTER) {
        unsigned version;

        /*
         * Write update is rare, so busywait here is unlikely to happen
         */
        do {
            version = seqlock_read_begin(&s->state_version);
            if (unlikely(!s->isEnabled())) {
                cur_tick = s->hpet_counter;
            } else {
                cur_tick = s->getTicks();
            }
        } while (seqlock_read_retry(&s->state_version, version));
        trace_hpet_ram_read_reading_counter(addr & 4, cur_tick);
        return cur_tick >> shift;
    }

    QEMU_LOCK_GUARD(&s->lock);
    /*address range of all global regs*/
    if (addr <= 0xff) {
        switch (addr) {
        case HPET_ID: // including HPET_PERIOD
            return s->capability >> shift;
        case HPET_CFG:
            return s->config >> shift;
        case HPET_STATUS:
            return s->isr >> shift;
        default:
            trace_hpet_ram_read_invalid();
            break;
        }
    } else {
        uint8_t timer_id = (addr - 0x100) / 0x20;
        HPETTimer *timer = &s->timer[timer_id];

        if (timer_id > s->num_timers) {
            trace_hpet_timer_id_out_of_range(timer_id);
            return 0;
        }

        switch (addr & 0x1f) {
        case HPET_TN_CFG: // including interrupt capabilities
            return timer->config >> shift;
        case HPET_TN_CMP: // comparator register
            return timer->cmp >> shift;
        case HPET_TN_ROUTE:
            return timer->fsb >> shift;
        default:
            trace_hpet_ram_read_invalid();
            break;
        }
    }
    return 0;
}

void HPETState::mmioWrite(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    int i;
    HPETState *s = static_cast<HPETState *>(opaque);
    int shift = (addr & 4) * 8;
    int len = MIN(size * 8, 64 - shift);
    uint64_t old_val, new_val, cleared;

    QEMU_LOCK_GUARD(&s->lock);
    trace_hpet_ram_write(addr, value);
    addr &= ~4;

    /*address range of all global regs*/
    if (addr <= 0xff) {
        switch (addr) {
        case HPET_ID:
            return;
        case HPET_CFG:
            old_val = s->config;
            new_val = deposit64(old_val, shift, len, value);
            new_val = hpet_fixup_reg(new_val, old_val, HPET_CFG_WRITE_MASK);
            seqlock_write_begin(&s->state_version);
            s->config = new_val;
            if (activating_bit(old_val, new_val, HPET_CFG_ENABLE)) {
                /* Enable main counter and interrupt generation. */
                s->hpet_offset =
                    ticks_to_ns(s->hpet_counter) - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                for (i = 0; i < s->num_timers; i++) {
                    if (s->timer[i].isEnabled() && (s->isr & (1 << i))) {
                        s->updateIrq(&s->timer[i], 1);
                    }
                    s->setTimer(&s->timer[i]);
                }
            } else if (deactivating_bit(old_val, new_val, HPET_CFG_ENABLE)) {
                /* Halt main counter and disable interrupt generation. */
                s->hpet_counter = s->getTicks();
                for (i = 0; i < s->num_timers; i++) {
                    s->delTimer(&s->timer[i]);
                }
            }
            seqlock_write_end(&s->state_version);

            /* i8254 and RTC output pins are disabled
             * when HPET is in legacy mode */
            if (activating_bit(old_val, new_val, HPET_CFG_LEGACY)) {
                BQL_LOCK_GUARD();
                qemu_set_irq(s->pit_enabled, 0);
                qemu_irq_lower(s->irqs[0]);
                qemu_irq_lower(s->irqs[RTC_ISA_IRQ]);
            } else if (deactivating_bit(old_val, new_val, HPET_CFG_LEGACY)) {
                BQL_LOCK_GUARD();
                qemu_irq_lower(s->irqs[0]);
                qemu_set_irq(s->pit_enabled, 1);
                qemu_set_irq(s->irqs[RTC_ISA_IRQ], s->rtc_irq_level);
            }
            break;
        case HPET_STATUS:
            new_val = value << shift;
            cleared = new_val & s->isr;
            for (i = 0; i < s->num_timers; i++) {
                if (cleared & (1 << i)) {
                    s->updateIrq(&s->timer[i], 0);
                }
            }
            break;
        case HPET_COUNTER:
            if (s->isEnabled()) {
                trace_hpet_ram_write_counter_write_while_enabled();
            }
            s->hpet_counter = deposit64(s->hpet_counter, shift, len, value);
            break;
        default:
            trace_hpet_ram_write_invalid();
            break;
        }
    } else {
        uint8_t timer_id = (addr - 0x100) / 0x20;
        HPETTimer *timer = &s->timer[timer_id];

        trace_hpet_ram_write_timer_id(timer_id);
        if (timer_id > s->num_timers) {
            trace_hpet_timer_id_out_of_range(timer_id);
            return;
        }
        switch (addr & 0x18) {
        case HPET_TN_CFG:
            trace_hpet_ram_write_tn_cfg(addr & 4);
            old_val = timer->config;
            new_val = deposit64(old_val, shift, len, value);
            new_val = hpet_fixup_reg(new_val, old_val, HPET_TN_CFG_WRITE_MASK);
            if (deactivating_bit(old_val, new_val, HPET_TN_TYPE_LEVEL)) {
                /*
                 * Do this before changing timer->config; otherwise, if
                 * HPET_TN_FSB is set, updateIrq will not lower the qemu_irq.
                 */
                s->updateIrq(timer, 0);
            }
            timer->config = new_val;
            if (activating_bit(old_val, new_val, HPET_TN_ENABLE)
                && (s->isr & (1 << timer_id))) {
                s->updateIrq(timer, 1);
            }
            if (new_val & HPET_TN_32BIT) {
                timer->cmp = (uint32_t)timer->cmp;
                timer->period = (uint32_t)timer->period;
            }
            if (s->isEnabled()) {
                s->setTimer(timer);
            }
            break;
        case HPET_TN_CMP: // comparator register
            if (timer->config & HPET_TN_32BIT) {
                /* High 32-bits are zero, leave them untouched.  */
                if (shift) {
                    trace_hpet_ram_write_invalid_tn_cmp();
                    break;
                }
                len = 64;
                value = (uint32_t) value;
            }
            trace_hpet_ram_write_tn_cmp(addr & 4);
            if (!timer->isPeriodic()
                || (timer->config & HPET_TN_SETVAL)) {
                timer->cmp = deposit64(timer->cmp, shift, len, value);
            }
            if (timer->isPeriodic()) {
                timer->period = deposit64(timer->period, shift, len, value);
            }
            timer->config &= ~HPET_TN_SETVAL;
            if (s->isEnabled()) {
                s->setTimer(timer);
            }
            break;
        case HPET_TN_ROUTE:
            timer->fsb = deposit64(timer->fsb, shift, len, value);
            break;
        default:
            trace_hpet_ram_write_invalid();
            break;
        }
        return;
    }
}

static const MemoryRegionOps hpet_ram_ops = {
    .read = HPETState::mmioRead,
    .write = HPETState::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void hpet_reset_wrapper(DeviceState *d)
{
    HPETState *s = HPET(d);
    s->reset(d);
}

void HPETState::reset(DeviceState *d)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(d);
    int i;

    for (i = 0; i < num_timers; i++) {
        HPETTimer *t = &timer[i];

        delTimer(t);
        t->cmp = ~0ULL;
        t->config = HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP;
        if (flags & (1 << HPET_MSI_SUPPORT)) {
            t->config |= HPET_TN_FSB_CAP;
        }
        /* advertise availability of ioapic int */
        t->config |=  (uint64_t)intcap << 32;
        t->period = 0ULL;
        t->wrap_flag = 0;
    }

    qemu_set_irq(pit_enabled, 1);
    hpet_counter = 0ULL;
    hpet_offset = 0ULL;
    config = 0ULL;
    hpet_fw_cfg.hpet[hpet_id].event_timer_block_id = (uint32_t)capability;
    hpet_fw_cfg.hpet[hpet_id].address = sbd->mmio[0].addr;

    /* to document that the RTC lowers its output on reset as well */
    rtc_irq_level = 0;
}

void HPETState::handleLegacyIrq(void *opaque, int n, int level)
{
    HPETState *s = HPET(opaque);

    if (n == HPET_LEGACY_PIT_INT) {
        if (!s->inLegacyMode()) {
            BQL_LOCK_GUARD();
            qemu_set_irq(s->irqs[0], level);
        }
    } else {
        s->rtc_irq_level = level;
        if (!s->inLegacyMode()) {
            BQL_LOCK_GUARD();
            qemu_set_irq(s->irqs[RTC_ISA_IRQ], level);
        }
    }
}

static void hpet_init_wrapper(Object *obj)
{
    HPETState *s = HPET(obj);
    s->initInstance(obj);
}

void HPETState::initInstance(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qemu_mutex_init(&lock);
    seqlock_init(&state_version);
    /* HPET Area */
    memory_region_init_io(&iomem, obj, &hpet_ram_ops, this, "hpet", HPET_LEN);
    memory_region_enable_lockless_io(&iomem);
    sysbus_init_mmio(sbd, &iomem);
}

static void hpet_realize_wrapper(DeviceState *dev, Error **errp)
{
    HPETState *s = HPET(dev);
    s->realize(dev, errp);
}

void HPETState::realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;
    HPETTimer *t;

    if (num_timers < HPET_MIN_TIMERS || num_timers > HPET_MAX_TIMERS) {
        error_setg(errp, "hpet.num_timers must be between %d and %d",
                   HPET_MIN_TIMERS, HPET_MAX_TIMERS);
        return;
    }
    if (!intcap) {
        error_setg(errp, "hpet.hpet-intcap not initialized");
        return;
    }
    if (hpet_fw_cfg.count == UINT8_MAX) {
        /* first instance */
        hpet_fw_cfg.count = 0;
    }

    if (hpet_fw_cfg.count == 8) {
        error_setg(errp, "Only 8 instances of HPET are allowed");
        return;
    }

    hpet_id = hpet_fw_cfg.count++;

    for (i = 0; i < HPET_NUM_IRQ_ROUTES; i++) {
        sysbus_init_irq(sbd, &irqs[i]);
    }

    for (i = 0; i < HPET_MAX_TIMERS; i++) {
        t = &timer[i];
        t->qemu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      HPETState::timerCallback, t);
        t->tn = i;
        t->state = this;
    }

    /* 64-bit General Capabilities and ID Register; LegacyReplacementRoute. */
    capability = 0x8086a001ULL;
    capability |= (num_timers - 1) << HPET_ID_NUM_TIM_SHIFT;
    capability |= ((uint64_t)(HPET_CLK_PERIOD * FS_PER_NS) << 32);

    qdev_init_gpio_in(dev, HPETState::handleLegacyIrq, 2);
    qdev_init_gpio_out(dev, &pit_enabled, 1);
}

static const Property hpet_device_properties[] = {
    DEFINE_PROP_UINT8("timers", HPETState, num_timers, HPET_MIN_TIMERS),
    DEFINE_PROP_BIT("msi", HPETState, flags, HPET_MSI_SUPPORT, false),
    DEFINE_PROP_UINT32(HPET_INTCAP, HPETState, intcap, 0),
    DEFINE_PROP_BOOL("hpet-offset-saved", HPETState, hpet_offset_saved, true),
};

void HPETState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = hpet_realize_wrapper;
    device_class_set_legacy_reset(dc, hpet_reset_wrapper);
    dc->vmsd = &vmstate_hpet;
    device_class_set_props(dc, hpet_device_properties);
}

static const TypeInfo hpet_device_info = {
    .name          = TYPE_HPET,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HPETState),
    .instance_init = hpet_init_wrapper,
    .class_init    = HPETState::classInit,
};

static void hpet_register_types(void)
{
    type_register_static(&hpet_device_info);
}

type_init(hpet_register_types)
