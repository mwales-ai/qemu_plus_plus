/*
 * MAX78000 UART
 *
 * Copyright (c) 2025 Jackson Donaldson <jcksn@duck.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/max78000_uart.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qom/cpp/object.h"


static int max78000_uart_can_receive(void *opaque)
{
    Max78000UartState *s = static_cast<Max78000UartState *>(opaque);
    if (!(s->ctrl & UART_BCLKEN)) {
        return 0;
    }
    return fifo8_num_free(&s->rx_fifo);
}

static void max78000_update_irq(Max78000UartState *s)
{
    int interrupt_level;

    interrupt_level = s->int_fl & s->int_en;
    qemu_set_irq(s->irq, interrupt_level);
}

static void max78000_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Max78000UartState *s = static_cast<Max78000UartState *>(opaque);

    assert(size <= static_cast<int>(fifo8_num_free(&s->rx_fifo)));

    fifo8_push_all(&s->rx_fifo, buf, size);

    uint32_t rx_threshold = s->ctrl & 0xf;

    if (fifo8_num_used(&s->rx_fifo) >= rx_threshold) {
        s->int_fl |= UART_RX_THD;
    }

    max78000_update_irq(s);
}

void Max78000UartState::reset()
{
    ctrl = 0;
    status = UART_TX_EM | UART_RX_EM;
    int_en = 0;
    int_fl = 0;
    osr = 0;
    txpeek = 0;
    pnr = UART_RTS;
    fifo = 0;
    dma = 0;
    wken = 0;
    wkfl = 0;
    fifo8_reset(&rx_fifo);
}

static uint64_t max78000_uart_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    Max78000UartState *s = static_cast<Max78000UartState *>(opaque);
    uint64_t retvalue = 0;
    switch (addr) {
    case UART_CTRL:
        retvalue = s->ctrl;
        break;
    case UART_STATUS:
        retvalue = (fifo8_num_used(&s->rx_fifo) << UART_RX_LVL) |
                    UART_TX_EM |
                    (fifo8_is_empty(&s->rx_fifo) ? UART_RX_EM : 0);
        break;
    case UART_INT_EN:
        retvalue = s->int_en;
        break;
    case UART_INT_FL:
        retvalue = s->int_fl;
        break;
    case UART_CLKDIV:
        retvalue = s->clkdiv;
        break;
    case UART_OSR:
        retvalue = s->osr;
        break;
    case UART_TXPEEK:
        if (!fifo8_is_empty(&s->rx_fifo)) {
            retvalue = fifo8_peek(&s->rx_fifo);
        }
        break;
    case UART_PNR:
        retvalue = s->pnr;
        break;
    case UART_FIFO:
        if (!fifo8_is_empty(&s->rx_fifo)) {
            retvalue = fifo8_pop(&s->rx_fifo);
            max78000_update_irq(s);
        }
        break;
    case UART_DMA:
        /* DMA not implemented */
        retvalue = s->dma;
        break;
    case UART_WKEN:
        retvalue = s->wken;
        break;
    case UART_WKFL:
        retvalue = s->wkfl;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        break;
    }

    return retvalue;
}

static void max78000_uart_write(void *opaque, hwaddr addr,
                                  uint64_t val64, unsigned int size)
{
    Max78000UartState *s = static_cast<Max78000UartState *>(opaque);

    uint32_t value = val64;
    uint8_t data;

    switch (addr) {
    case UART_CTRL:
        if (value & UART_FLUSH_RX) {
            fifo8_reset(&s->rx_fifo);
        }
        if (value & UART_BCLKEN) {
            value = value | UART_BCLKRDY;
        }
        s->ctrl = value & ~(UART_FLUSH_RX | UART_FLUSH_TX);

        /*
         * Software can manage UART flow control manually by setting hfc_en
         * in UART_CTRL. This would require emulating uart at a lower level,
         * and is currently unimplemented.
         */

        return;
    case UART_STATUS:
        /* UART_STATUS is read only */
        return;
    case UART_INT_EN:
        s->int_en = value;
        return;
    case UART_INT_FL:
        s->int_fl = s->int_fl & ~(value);
        max78000_update_irq(s);
        return;
    case UART_CLKDIV:
        s->clkdiv = value;
        return;
    case UART_OSR:
        s->osr = value;
        return;
    case UART_PNR:
        s->pnr = value;
        return;
    case UART_FIFO:
        data = value & 0xff;
        /*
         * XXX this blocks entire thread. Rewrite to use
         * qemu_chr_fe_write and background I/O callbacks
         */
        qemu_chr_fe_write_all(&s->chr, &data, 1);

        /* TX is always empty */
        s->int_fl |= UART_TX_HE;
        max78000_update_irq(s);

        return;
    case UART_DMA:
        /* DMA not implemented */
        s->dma = value;
        return;
    case UART_WKEN:
        s->wken = value;
        return;
    case UART_WKFL:
        s->wkfl = value;
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps max78000_uart_ops = {
    .read = max78000_uart_read,
    .write = max78000_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const Property max78000_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", Max78000UartState, chr),
};

static const VMStateField vmstate_max78000_uart_fields[] = {
    VMSTATE_UINT32(ctrl, Max78000UartState),
    VMSTATE_UINT32(status, Max78000UartState),
    VMSTATE_UINT32(int_en, Max78000UartState),
    VMSTATE_UINT32(int_fl, Max78000UartState),
    VMSTATE_UINT32(clkdiv, Max78000UartState),
    VMSTATE_UINT32(osr, Max78000UartState),
    VMSTATE_UINT32(txpeek, Max78000UartState),
    VMSTATE_UINT32(pnr, Max78000UartState),
    VMSTATE_UINT32(fifo, Max78000UartState),
    VMSTATE_UINT32(dma, Max78000UartState),
    VMSTATE_UINT32(wken, Max78000UartState),
    VMSTATE_UINT32(wkfl, Max78000UartState),
    VMSTATE_FIFO8(rx_fifo, Max78000UartState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription max78000_uart_vmstate = {
    .name = TYPE_MAX78000_UART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_max78000_uart_fields,
};

void Max78000UartState::init()
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    fifo8_create(&rx_fifo, 8);

    sysbus_init_irq(sbd, &irq);

    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &max78000_uart_ops, this, TYPE_MAX78000_UART,
                          0x400);
    sysbus_init_mmio(sbd, &mmio);
}

void Max78000UartState::finalize()
{
    fifo8_destroy(&rx_fifo);
}

void Max78000UartState::realize(Error **errp)
{
    qemu_chr_fe_set_handlers(&chr, max78000_uart_can_receive,
                             max78000_uart_receive, NULL, NULL,
                             this, NULL, true);
}

void Max78000UartState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, max78000_uart_properties);
    dc->vmsd = &max78000_uart_vmstate;
}

REGISTER_QEMU_DEVICE(Max78000UartState, TYPE_MAX78000_UART, TYPE_SYS_BUS_DEVICE)
