/*
 * AVR USART
 *
 * Copyright (c) 2018 University of Kent
 * Author: Sarah Harris
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
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */

#include "qemu/osdep.h"
#include "hw/char/avr_usart.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qom/cpp/object.h"

static int avr_usart_can_receive(void *opaque)
{
    AVRUsartState *usart = static_cast<AVRUsartState *>(opaque);

    if (usart->data_valid || !(usart->csrb & USART_CSRB_RXEN)) {
        return 0;
    }
    return 1;
}

static void avr_usart_receive(void *opaque, const uint8_t *buffer, int size)
{
    AVRUsartState *usart = static_cast<AVRUsartState *>(opaque);
    assert(size == 1);
    assert(!usart->data_valid);
    usart->data = buffer[0];
    usart->data_valid = true;
    usart->csra |= USART_CSRA_RXC;
    if (usart->csrb & USART_CSRB_RXCIE) {
        qemu_set_irq(usart->rxc_irq, 1);
    }
}

static void update_char_mask(AVRUsartState *usart)
{
    uint8_t mode = ((usart->csrc & USART_CSRC_CSZ0) ? 1 : 0) |
        ((usart->csrc & USART_CSRC_CSZ1) ? 2 : 0) |
        ((usart->csrb & USART_CSRB_CSZ2) ? 4 : 0);
    switch (mode) {
    case 0:
        usart->char_mask = 0b11111;
        break;
    case 1:
        usart->char_mask = 0b111111;
        break;
    case 2:
        usart->char_mask = 0b1111111;
        break;
    case 3:
        usart->char_mask = 0b11111111;
        break;
    case 4:
        /* Fallthrough. */
    case 5:
        /* Fallthrough. */
    case 6:
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: Reserved character size 0x%x\n",
            __func__,
            mode);
        break;
    case 7:
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: Nine bit character size not supported (forcing eight)\n",
            __func__);
        usart->char_mask = 0b11111111;
        break;
    default:
        g_assert_not_reached();
    }
}

void AVRUsartState::reset()
{
    data_valid = false;
    csra = 0b00100000;
    csrb = 0b00000000;
    csrc = 0b00000110;
    brrl = 0;
    brrh = 0;
    update_char_mask(this);
    qemu_set_irq(rxc_irq, 0);
    qemu_set_irq(txc_irq, 0);
    qemu_set_irq(dre_irq, 0);
}

static uint64_t avr_usart_read(void *opaque, hwaddr addr, unsigned int size)
{
    AVRUsartState *usart = static_cast<AVRUsartState *>(opaque);
    uint8_t data;
    assert(size == 1);

    if (!usart->enabled) {
        return 0;
    }

    switch (addr) {
    case USART_DR:
        if (!(usart->csrb & USART_CSRB_RXEN)) {
            /* Receiver disabled, ignore. */
            return 0;
        }
        if (usart->data_valid) {
            data = usart->data & usart->char_mask;
            usart->data_valid = false;
        } else {
            data = 0;
        }
        usart->csra &= 0xff ^ USART_CSRA_RXC;
        qemu_set_irq(usart->rxc_irq, 0);
        qemu_chr_fe_accept_input(&usart->chr);
        return data;
    case USART_CSRA:
        return usart->csra;
    case USART_CSRB:
        return usart->csrb;
    case USART_CSRC:
        return usart->csrc;
    case USART_BRRL:
        return usart->brrl;
    case USART_BRRH:
        return usart->brrh;
    default:
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: Bad offset 0x%" HWADDR_PRIx "\n",
            __func__,
            addr);
    }
    return 0;
}

static void avr_usart_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    AVRUsartState *usart = static_cast<AVRUsartState *>(opaque);
    uint8_t mask;
    uint8_t data;
    assert((value & 0xff) == value);
    assert(size == 1);

    if (!usart->enabled) {
        return;
    }

    switch (addr) {
    case USART_DR:
        if (!(usart->csrb & USART_CSRB_TXEN)) {
            /* Transmitter disabled, ignore. */
            return;
        }
        usart->csra |= USART_CSRA_TXC;
        usart->csra |= USART_CSRA_DRE;
        if (usart->csrb & USART_CSRB_TXCIE) {
            qemu_set_irq(usart->txc_irq, 1);
            usart->csra &= 0xff ^ USART_CSRA_TXC;
        }
        if (usart->csrb & USART_CSRB_DREIE) {
            qemu_set_irq(usart->dre_irq, 1);
        }
        data = value;
        qemu_chr_fe_write_all(&usart->chr, &data, 1);
        break;
    case USART_CSRA:
        mask = 0b01000011;
        /* Mask read-only bits. */
        value = (value & mask) | (usart->csra & (0xff ^ mask));
        usart->csra = value;
        if (value & USART_CSRA_TXC) {
            usart->csra ^= USART_CSRA_TXC;
            qemu_set_irq(usart->txc_irq, 0);
        }
        if (value & USART_CSRA_MPCM) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: MPCM not supported by USART\n",
                __func__);
        }
        break;
    case USART_CSRB:
        mask = 0b11111101;
        /* Mask read-only bits. */
        value = (value & mask) | (usart->csrb & (0xff ^ mask));
        usart->csrb = value;
        if (!(value & USART_CSRB_RXEN)) {
            /* Receiver disabled, flush input buffer. */
            usart->data_valid = false;
        }
        qemu_set_irq(usart->rxc_irq,
            ((value & USART_CSRB_RXCIE) &&
            (usart->csra & USART_CSRA_RXC)) ? 1 : 0);
        qemu_set_irq(usart->txc_irq,
            ((value & USART_CSRB_TXCIE) &&
            (usart->csra & USART_CSRA_TXC)) ? 1 : 0);
        qemu_set_irq(usart->dre_irq,
            ((value & USART_CSRB_DREIE) &&
            (usart->csra & USART_CSRA_DRE)) ? 1 : 0);
        update_char_mask(usart);
        break;
    case USART_CSRC:
        usart->csrc = value;
        if ((value & USART_CSRC_MSEL1) && (value & USART_CSRC_MSEL0)) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: SPI mode not supported by USART\n",
                __func__);
        }
        if ((value & USART_CSRC_MSEL1) && !(value & USART_CSRC_MSEL0)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad USART mode\n", __func__);
        }
        if (!(value & USART_CSRC_PM1) && (value & USART_CSRC_PM0)) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: Bad USART parity mode\n",
                __func__);
        }
        update_char_mask(usart);
        break;
    case USART_BRRL:
        usart->brrl = value;
        break;
    case USART_BRRH:
        usart->brrh = value & 0b00001111;
        break;
    default:
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: Bad offset 0x%" HWADDR_PRIx "\n",
            __func__,
            addr);
    }
}

static const MemoryRegionOps avr_usart_ops = {
    .read = avr_usart_read,
    .write = avr_usart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {.min_access_size = 1, .max_access_size = 1}
};

static const Property avr_usart_properties[] = {
    DEFINE_PROP_CHR("chardev", AVRUsartState, chr),
};

static void avr_usart_pr(void *opaque, int irq, int level)
{
    AVRUsartState *s = static_cast<AVRUsartState *>(opaque);

    s->enabled = !level;

    if (!s->enabled) {
        s->reset();
    }
}

void AVRUsartState::init()
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);
    sysbus_init_irq(sbd, &rxc_irq);
    sysbus_init_irq(sbd, &dre_irq);
    sysbus_init_irq(sbd, &txc_irq);
    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &avr_usart_ops, this, TYPE_AVR_USART, 7);
    sysbus_init_mmio(sbd, &mmio);
    qdev_init_gpio_in(reinterpret_cast<DeviceState *>(this), avr_usart_pr, 1);
    enabled = true;
}

void AVRUsartState::realize(Error **errp)
{
    qemu_chr_fe_set_handlers(&chr, avr_usart_can_receive,
                             avr_usart_receive, NULL, NULL,
                             this, NULL, true);
    reset();
}

void AVRUsartState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, avr_usart_properties);
}

REGISTER_QEMU_DEVICE(AVRUsartState, TYPE_AVR_USART, TYPE_SYS_BUS_DEVICE)
