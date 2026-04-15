/*
 * Copyright (c) 2018, Impinj, Inc.
 *
 * i.MX2 Watchdog IP block
 *
 * Author: Andrey Smirnov <andrew.smirnov@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "qemu/bitops.h"
#include "qemu/module.h"
#include "system/watchdog.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "hw/watchdog/wdt_imx2.h"
#include "qom/cpp/object.h"

#include "trace.h"

static void imx2_wdt_interrupt(void *opaque)
{
    IMX2WdtState *s = IMX2_WDT(opaque);

    trace_imx2_wdt_interrupt();

    s->wicr |= IMX2_WDT_WICR_WTIS;
    qemu_set_irq(s->irq, 1);
}

static void imx2_wdt_expired(void *opaque)
{
    IMX2WdtState *s = IMX2_WDT(opaque);

    trace_imx2_wdt_expired();

    s->wrsr = IMX2_WDT_WRSR_TOUT;

    /* Perform watchdog action if watchdog is enabled */
    if (s->wcr & IMX2_WDT_WCR_WDE) {
        watchdog_perform_action();
    }
}

void IMX2WdtState::reset()
{
    ptimer_transaction_begin(timer);
    ptimer_stop(timer);
    ptimer_transaction_commit(timer);

    if (pretimeout_support) {
        ptimer_transaction_begin(itimer);
        ptimer_stop(itimer);
        ptimer_transaction_commit(itimer);
    }

    wicr_locked = false;
    wcr_locked = false;
    wcr_wde_locked = false;

    wcr = IMX2_WDT_WCR_WDA | IMX2_WDT_WCR_SRS;
    wsr = 0;
    wrsr &= ~(IMX2_WDT_WRSR_TOUT | IMX2_WDT_WRSR_SFTW);
    wicr = IMX2_WDT_WICR_WICT_DEF;
    wmcr = IMX2_WDT_WMCR_PDE;
}

static uint64_t imx2_wdt_read(void *opaque, hwaddr addr, unsigned int size)
{
    IMX2WdtState *s = IMX2_WDT(opaque);
    uint16_t value = 0;

    switch (addr) {
    case IMX2_WDT_WCR:
        value = s->wcr;
        break;
    case IMX2_WDT_WSR:
        value = s->wsr;
        break;
    case IMX2_WDT_WRSR:
        value = s->wrsr;
        break;
    case IMX2_WDT_WICR:
        value = s->wicr;
        break;
    case IMX2_WDT_WMCR:
        value = s->wmcr;
        break;
    }

    trace_imx2_wdt_read(addr, value);

    return value;
}

static void imx_wdt2_update_itimer(IMX2WdtState *s, bool start)
{
    bool running = (s->wcr & IMX2_WDT_WCR_WDE) && (s->wcr & IMX2_WDT_WCR_WT);
    bool enabled = s->wicr & IMX2_WDT_WICR_WIE;

    ptimer_transaction_begin(s->itimer);
    if (start || !enabled) {
        ptimer_stop(s->itimer);
    }
    if (running && enabled) {
        int count = ptimer_get_count(s->timer);
        int pretimeout = s->wicr & IMX2_WDT_WICR_WICT;

        if (count > pretimeout) {
            ptimer_set_count(s->itimer, count - pretimeout);
            if (start) {
                ptimer_run(s->itimer, 1);
            }
        }
    }
    ptimer_transaction_commit(s->itimer);
}

static void imx_wdt2_update_timer(IMX2WdtState *s, bool start)
{
    ptimer_transaction_begin(s->timer);
    if (start) {
        ptimer_stop(s->timer);
    }
    if ((s->wcr & IMX2_WDT_WCR_WDE) && (s->wcr & IMX2_WDT_WCR_WT)) {
        int count = (s->wcr & IMX2_WDT_WCR_WT) >> 8;

        /* A value of 0 reflects one period (0.5s). */
        ptimer_set_count(s->timer, count + 1);
        if (start) {
            ptimer_run(s->timer, 1);
        }
    }
    ptimer_transaction_commit(s->timer);
    if (s->pretimeout_support) {
        imx_wdt2_update_itimer(s, start);
    }
}

static void imx2_wdt_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned int size)
{
    IMX2WdtState *s = IMX2_WDT(opaque);

    trace_imx2_wdt_write(addr, value);

    switch (addr) {
    case IMX2_WDT_WCR:
        if (s->wcr_locked) {
            value &= ~IMX2_WDT_WCR_LOCK_MASK;
            value |= (s->wicr & IMX2_WDT_WCR_LOCK_MASK);
        }
        s->wcr_locked = true;
        if (s->wcr_wde_locked) {
            value &= ~IMX2_WDT_WCR_WDE;
            value |= (s->wicr & ~IMX2_WDT_WCR_WDE);
        } else if (value & IMX2_WDT_WCR_WDE) {
            s->wcr_wde_locked = true;
        }
        if (s->wcr_wdt_locked) {
            value &= ~IMX2_WDT_WCR_WDT;
            value |= (s->wicr & ~IMX2_WDT_WCR_WDT);
        } else if (value & IMX2_WDT_WCR_WDT) {
            s->wcr_wdt_locked = true;
        }

        s->wcr = value;
        if (!(value & IMX2_WDT_WCR_SRS)) {
            s->wrsr = IMX2_WDT_WRSR_SFTW;
        }
        if (!(value & (IMX2_WDT_WCR_WDA | IMX2_WDT_WCR_SRS)) ||
            (!(value & IMX2_WDT_WCR_WT) && (value & IMX2_WDT_WCR_WDE))) {
            watchdog_perform_action();
        }
        s->wcr |= IMX2_WDT_WCR_SRS;
        imx_wdt2_update_timer(s, true);
        break;
    case IMX2_WDT_WSR:
        if (s->wsr == IMX2_WDT_SEQ1 && value == IMX2_WDT_SEQ2) {
            imx_wdt2_update_timer(s, false);
        }
        s->wsr = value;
        break;
    case IMX2_WDT_WRSR:
        break;
    case IMX2_WDT_WICR:
        if (!s->pretimeout_support) {
            return;
        }
        value &= IMX2_WDT_WICR_LOCK_MASK | IMX2_WDT_WICR_WTIS;
        if (s->wicr_locked) {
            value &= IMX2_WDT_WICR_WTIS;
            value |= (s->wicr & IMX2_WDT_WICR_LOCK_MASK);
        }
        s->wicr = value | (s->wicr & IMX2_WDT_WICR_WTIS);
        if (value & IMX2_WDT_WICR_WTIS) {
            s->wicr &= ~IMX2_WDT_WICR_WTIS;
            qemu_set_irq(s->irq, 0);
        }
        imx_wdt2_update_itimer(s, true);
        s->wicr_locked = true;
        break;
    case IMX2_WDT_WMCR:
        s->wmcr = value & IMX2_WDT_WMCR_PDE;
        break;
    }
}

static MemoryRegionOps imx2_wdt_ops;

static void __attribute__((constructor)) init_imx2_wdt_ops(void)
{
    memset(&imx2_wdt_ops, 0, sizeof(imx2_wdt_ops));
    imx2_wdt_ops.read  = imx2_wdt_read;
    imx2_wdt_ops.write = imx2_wdt_write;
    imx2_wdt_ops.endianness = DEVICE_NATIVE_ENDIAN;
    imx2_wdt_ops.impl.min_access_size = 2;
    imx2_wdt_ops.impl.max_access_size = 2;
    imx2_wdt_ops.impl.unaligned = false;
}

static const VMStateField vmstate_imx2_wdt_fields[] = {
    VMSTATE_PTIMER(timer, IMX2WdtState),
    VMSTATE_PTIMER(itimer, IMX2WdtState),
    VMSTATE_BOOL(wicr_locked, IMX2WdtState),
    VMSTATE_BOOL(wcr_locked, IMX2WdtState),
    VMSTATE_BOOL(wcr_wde_locked, IMX2WdtState),
    VMSTATE_BOOL(wcr_wdt_locked, IMX2WdtState),
    VMSTATE_UINT16(wcr, IMX2WdtState),
    VMSTATE_UINT16(wsr, IMX2WdtState),
    VMSTATE_UINT16(wrsr, IMX2WdtState),
    VMSTATE_UINT16(wmcr, IMX2WdtState),
    VMSTATE_UINT16(wicr, IMX2WdtState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_imx2_wdt = {
    .name = "imx2.wdt",
    .fields = vmstate_imx2_wdt_fields,
};

void IMX2WdtState::realize(Error **errp)
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &imx2_wdt_ops, this,
                          TYPE_IMX2_WDT,
                          IMX2_WDT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &mmio);
    sysbus_init_irq(sbd, &irq);

    timer = ptimer_init(imx2_wdt_expired, this,
                        PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                        PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                        PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
    ptimer_transaction_begin(timer);
    ptimer_set_freq(timer, 2);
    ptimer_set_limit(timer, 0xff, 1);
    ptimer_transaction_commit(timer);
    if (pretimeout_support) {
        itimer = ptimer_init(imx2_wdt_interrupt, this,
                             PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                             PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                             PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
        ptimer_transaction_begin(itimer);
        ptimer_set_freq(itimer, 2);
        ptimer_set_limit(itimer, 0xff, 1);
        ptimer_transaction_commit(itimer);
    }
}

static const Property imx2_wdt_properties[] = {
    DEFINE_PROP_BOOL("pretimeout-support", IMX2WdtState, pretimeout_support,
                     false),
};

void IMX2WdtState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, imx2_wdt_properties);
    dc->vmsd = &vmstate_imx2_wdt;
    dc->desc = "i.MX2 watchdog timer";
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
}

REGISTER_QEMU_DEVICE(IMX2WdtState, TYPE_IMX2_WDT, TYPE_SYS_BUS_DEVICE)
