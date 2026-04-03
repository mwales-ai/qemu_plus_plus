/*
 * QEMU SCI/SCIF serial port emulation
 *
 * Copyright (c) 2007 Magnus Damm
 *
 * Based on serial.c - QEMU 16450 UART emulation
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sh4/sh.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "trace.h"

#define SH_SERIAL_FLAG_TEND (1 << 0)
#define SH_SERIAL_FLAG_TDE  (1 << 1)
#define SH_SERIAL_FLAG_RDF  (1 << 2)
#define SH_SERIAL_FLAG_BRK  (1 << 3)
#define SH_SERIAL_FLAG_DR   (1 << 4)

#define SH_RX_FIFO_LENGTH (16)

OBJECT_DECLARE_SIMPLE_TYPE(SHSerialState, SH_SERIAL)

struct SHSerialState {
    SysBusDevice parent;
    uint8_t smr;
    uint8_t brr;
    uint8_t scr;
    uint8_t dr; /* ftdr / tdr */
    uint8_t sr; /* fsr / ssr */
    uint16_t fcr;
    uint8_t sptr;

    uint8_t rx_fifo[SH_RX_FIFO_LENGTH]; /* frdr / rdr */
    uint8_t rx_cnt;
    uint8_t rx_tail;
    uint8_t rx_head;

    uint8_t feat;
    int flags;
    int rtrg;

    CharFrontend chr;
    QEMUTimer fifo_timeout_timer;
    uint64_t etu; /* Elementary Time Unit (ns) */

    qemu_irq eri;
    qemu_irq rxi;
    qemu_irq txi;
    qemu_irq tei;
    qemu_irq bri;

    void clearFifo();
    static void mmioWrite(void *opaque, hwaddr offs, uint64_t val,
                          unsigned size);
    static uint64_t mmioRead(void *opaque, hwaddr offs, unsigned size);
    int canReceive();
    void receiveBreak();
    static int canReceive1(void *opaque);
    static void timeoutInt(void *opaque);
    static void receive1(void *opaque, const uint8_t *buf, int size);
    static void chrEvent(void *opaque, QEMUChrEvent event);
    void reset();
    void realize(Error **errp);
    void unrealize();
    static void resetWrapper(DeviceState *dev);
    static void realizeWrapper(DeviceState *d, Error **errp);
    static void unrealizeWrapper(DeviceState *dev);
    static void classInit(ObjectClass *oc, const void *data);
};

void SHSerialState::clearFifo()
{
    memset(rx_fifo, 0, SH_RX_FIFO_LENGTH);
    rx_cnt = 0;
    rx_head = 0;
    rx_tail = 0;
}

void SHSerialState::mmioWrite(void *opaque, hwaddr offs,
                               uint64_t val, unsigned size)
{
    SHSerialState *s = static_cast<SHSerialState *>(opaque);
    DeviceState *d = DEVICE(s);
    unsigned char ch;

    trace_sh_serial_write(d->id, size, offs, val);
    switch (offs) {
    case 0x00: /* SMR */
        s->smr = val & ((s->feat & SH_SERIAL_FEAT_SCIF) ? 0x7b : 0xff);
        return;
    case 0x04: /* BRR */
        s->brr = val;
        return;
    case 0x08: /* SCR */
        /* TODO : For SH7751, SCIF mask should be 0xfb. */
        s->scr = val & ((s->feat & SH_SERIAL_FEAT_SCIF) ? 0xfa : 0xff);
        if (!(val & (1 << 5))) {
            s->flags |= SH_SERIAL_FLAG_TEND;
        }
        if ((s->feat & SH_SERIAL_FEAT_SCIF) && s->txi) {
            qemu_set_irq(s->txi, val & (1 << 7));
        }
        if (!(val & (1 << 6))) {
            qemu_set_irq(s->rxi, 0);
        }
        return;
    case 0x0c: /* FTDR / TDR */
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            ch = val;
            /*
             * XXX this blocks entire thread. Rewrite to use
             * qemu_chr_fe_write and background I/O callbacks
             */
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        s->dr = val;
        s->flags &= ~SH_SERIAL_FLAG_TDE;
        return;
#if 0
    case 0x14: /* FRDR / RDR */
        ret = 0;
        break;
#endif
    }
    if (s->feat & SH_SERIAL_FEAT_SCIF) {
        switch (offs) {
        case 0x10: /* FSR */
            if (!(val & (1 << 6))) {
                s->flags &= ~SH_SERIAL_FLAG_TEND;
            }
            if (!(val & (1 << 5))) {
                s->flags &= ~SH_SERIAL_FLAG_TDE;
            }
            if (!(val & (1 << 4))) {
                s->flags &= ~SH_SERIAL_FLAG_BRK;
            }
            if (!(val & (1 << 1))) {
                s->flags &= ~SH_SERIAL_FLAG_RDF;
            }
            if (!(val & (1 << 0))) {
                s->flags &= ~SH_SERIAL_FLAG_DR;
            }

            if (!(val & (1 << 1)) || !(val & (1 << 0))) {
                if (s->rxi) {
                    qemu_set_irq(s->rxi, 0);
                }
            }
            return;
        case 0x18: /* FCR */
            s->fcr = val;
            switch ((val >> 6) & 3) {
            case 0:
                s->rtrg = 1;
                break;
            case 1:
                s->rtrg = 4;
                break;
            case 2:
                s->rtrg = 8;
                break;
            case 3:
                s->rtrg = 14;
                break;
            }
            if (val & (1 << 1)) {
                s->clearFifo();
                s->sr &= ~(1 << 1);
            }

            return;
        case 0x20: /* SPTR */
            s->sptr = val & 0xf3;
            return;
        case 0x24: /* LSR */
            return;
        }
    } else {
        switch (offs) {
#if 0
        case 0x0c:
            ret = s->dr;
            break;
        case 0x10:
            ret = 0;
            break;
#endif
        case 0x1c:
            s->sptr = val & 0x8f;
            return;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: unsupported write to 0x%02" HWADDR_PRIx "\n",
                  __func__, offs);
}

uint64_t SHSerialState::mmioRead(void *opaque, hwaddr offs,
                                  unsigned size)
{
    SHSerialState *s = static_cast<SHSerialState *>(opaque);
    DeviceState *d = DEVICE(s);
    uint32_t ret = UINT32_MAX;

#if 0
    switch (offs) {
    case 0x00:
        ret = s->smr;
        break;
    case 0x04:
        ret = s->brr;
        break;
    case 0x08:
        ret = s->scr;
        break;
    case 0x14:
        ret = 0;
        break;
    }
#endif
    if (s->feat & SH_SERIAL_FEAT_SCIF) {
        switch (offs) {
        case 0x00: /* SMR */
            ret = s->smr;
            break;
        case 0x08: /* SCR */
            ret = s->scr;
            break;
        case 0x10: /* FSR */
            ret = 0;
            if (s->flags & SH_SERIAL_FLAG_TEND) {
                ret |= (1 << 6);
            }
            if (s->flags & SH_SERIAL_FLAG_TDE) {
                ret |= (1 << 5);
            }
            if (s->flags & SH_SERIAL_FLAG_BRK) {
                ret |= (1 << 4);
            }
            if (s->flags & SH_SERIAL_FLAG_RDF) {
                ret |= (1 << 1);
            }
            if (s->flags & SH_SERIAL_FLAG_DR) {
                ret |= (1 << 0);
            }

            if (s->scr & (1 << 5)) {
                s->flags |= SH_SERIAL_FLAG_TDE | SH_SERIAL_FLAG_TEND;
            }

            break;
        case 0x14:
            if (s->rx_cnt > 0) {
                ret = s->rx_fifo[s->rx_tail++];
                s->rx_cnt--;
                if (s->rx_tail == SH_RX_FIFO_LENGTH) {
                    s->rx_tail = 0;
                }
                if (s->rx_cnt < s->rtrg) {
                    s->flags &= ~SH_SERIAL_FLAG_RDF;
                }
            }
            break;
        case 0x18:
            ret = s->fcr;
            break;
        case 0x1c:
            ret = s->rx_cnt;
            break;
        case 0x20:
            ret = s->sptr;
            break;
        case 0x24:
            ret = 0;
            break;
        }
    } else {
        switch (offs) {
#if 0
        case 0x0c:
            ret = s->dr;
            break;
        case 0x10:
            ret = 0;
            break;
        case 0x14:
            ret = s->rx_fifo[0];
            break;
#endif
        case 0x1c:
            ret = s->sptr;
            break;
        }
    }
    trace_sh_serial_read(d->id, size, offs, ret);

    if (ret > UINT16_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unsupported read from 0x%02" HWADDR_PRIx "\n",
                      __func__, offs);
        ret = 0;
    }

    return ret;
}

int SHSerialState::canReceive()
{
    return scr & (1 << 4) ? SH_RX_FIFO_LENGTH - rx_head : 0;
}

void SHSerialState::receiveBreak()
{
    if (feat & SH_SERIAL_FEAT_SCIF) {
        sr |= (1 << 4);
    }
}

int SHSerialState::canReceive1(void *opaque)
{
    SHSerialState *s = static_cast<SHSerialState *>(opaque);
    return s->canReceive();
}

void SHSerialState::timeoutInt(void *opaque)
{
    SHSerialState *s = static_cast<SHSerialState *>(opaque);

    s->flags |= SH_SERIAL_FLAG_RDF;
    if (s->scr & (1 << 6) && s->rxi) {
        qemu_set_irq(s->rxi, 1);
    }
}

void SHSerialState::receive1(void *opaque, const uint8_t *buf, int size)
{
    SHSerialState *s = static_cast<SHSerialState *>(opaque);

    if (s->feat & SH_SERIAL_FEAT_SCIF) {
        int i;
        for (i = 0; i < size; i++) {
            s->rx_fifo[s->rx_head++] = buf[i];
            if (s->rx_head == SH_RX_FIFO_LENGTH) {
                s->rx_head = 0;
            }
            s->rx_cnt++;
            if (s->rx_cnt >= s->rtrg) {
                s->flags |= SH_SERIAL_FLAG_RDF;
                if (s->scr & (1 << 6) && s->rxi) {
                    timer_del(&s->fifo_timeout_timer);
                    qemu_set_irq(s->rxi, 1);
                }
            } else {
                timer_mod(&s->fifo_timeout_timer,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 15 * s->etu);
            }
        }
    } else {
        s->rx_fifo[0] = buf[0];
    }
}

void SHSerialState::chrEvent(void *opaque, QEMUChrEvent event)
{
    SHSerialState *s = static_cast<SHSerialState *>(opaque);
    if (event == CHR_EVENT_BREAK) {
        s->receiveBreak();
    }
}

static const MemoryRegionOps sh_serial_ops = {
    .read = SHSerialState::mmioRead,
    .write = SHSerialState::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void SHSerialState::resetWrapper(DeviceState *dev)
{
    SHSerialState *s = SH_SERIAL(dev);
    s->reset();
}

void SHSerialState::reset()
{
    flags = SH_SERIAL_FLAG_TEND | SH_SERIAL_FLAG_TDE;
    rtrg = 1;

    smr = 0;
    brr = 0xff;
    scr = 1 << 5; /* pretend that TX is enabled so early printk works */
    sptr = 0;

    if (feat & SH_SERIAL_FEAT_SCIF) {
        fcr = 0;
    } else {
        dr = 0xff;
    }

    clearFifo();
}

void SHSerialState::realizeWrapper(DeviceState *d, Error **errp)
{
    SHSerialState *s = SH_SERIAL(d);
    s->realize(errp);
}

void SHSerialState::realize(Error **errp)
{
    DeviceState *d = DEVICE(this);
    MemoryRegion *iomem = static_cast<MemoryRegion *>(g_malloc(sizeof(*iomem)));

    assert(d->id);
    memory_region_init_io(iomem, OBJECT(this), &sh_serial_ops, this, d->id, 0x28);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), iomem);
    qdev_init_gpio_out_named(d, &eri, "eri", 1);
    qdev_init_gpio_out_named(d, &rxi, "rxi", 1);
    qdev_init_gpio_out_named(d, &txi, "txi", 1);
    qdev_init_gpio_out_named(d, &tei, "tei", 1);
    qdev_init_gpio_out_named(d, &bri, "bri", 1);

    if (qemu_chr_fe_backend_connected(&chr)) {
        qemu_chr_fe_set_handlers(&chr, SHSerialState::canReceive1,
                                 SHSerialState::receive1,
                                 SHSerialState::chrEvent, NULL, this, NULL, true);
    }

    timer_init_ns(&fifo_timeout_timer, QEMU_CLOCK_VIRTUAL,
                  SHSerialState::timeoutInt, this);
    etu = NANOSECONDS_PER_SECOND / 9600;
}

void SHSerialState::unrealizeWrapper(DeviceState *dev)
{
    SHSerialState *s = SH_SERIAL(dev);
    s->unrealize();
}

void SHSerialState::unrealize()
{
    timer_del(&fifo_timeout_timer);
}

static const Property sh_serial_properties[] = {
    DEFINE_PROP_CHR("chardev", SHSerialState, chr),
    DEFINE_PROP_UINT8("features", SHSerialState, feat, 0),
};

void SHSerialState::classInit(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, sh_serial_properties);
    dc->realize = SHSerialState::realizeWrapper;
    dc->unrealize = SHSerialState::unrealizeWrapper;
    device_class_set_legacy_reset(dc, SHSerialState::resetWrapper);
    /* Reason: part of SuperH CPU/SoC, needs to be wired up */
    dc->user_creatable = false;
}

static const TypeInfo sh_serial_types[] = {
    {
        .name           = TYPE_SH_SERIAL,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(SHSerialState),
        .class_init     = SHSerialState::classInit,
    },
};

DEFINE_TYPES(sh_serial_types)
