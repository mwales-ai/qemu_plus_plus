/*
 * BCM2835 SPI Master Controller
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
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
#include "qemu/fifo8.h"
#include "hw/ssi/bcm2835_spi.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/cpp/object.h"

static void bcm2835_spi_update_int(BCM2835SPIState *s)
{
    int do_interrupt = 0;

    if (s->cs & BCM2835_SPI_CS_INTD && s->cs & BCM2835_SPI_CS_DONE) {
        do_interrupt = 1;
    }
    if (s->cs & BCM2835_SPI_CS_INTR && s->cs & BCM2835_SPI_CS_RXR) {
        do_interrupt = 1;
    }
    qemu_set_irq(s->irq, do_interrupt);
}

static void bcm2835_spi_update_rx_flags(BCM2835SPIState *s)
{
    if (!fifo8_is_empty(&s->rx_fifo)) {
        s->cs |= BCM2835_SPI_CS_RXD;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXD;
    }

    if (fifo8_is_full(&s->rx_fifo)) {
        s->cs |= BCM2835_SPI_CS_RXF;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXF;
    }

    if (fifo8_num_used(&s->rx_fifo) >= FIFO_SIZE_3_4) {
        s->cs |= BCM2835_SPI_CS_RXR;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXR;
    }
}

static void bcm2835_spi_update_tx_flags(BCM2835SPIState *s)
{
    if (fifo8_is_full(&s->tx_fifo)) {
        s->cs &= ~BCM2835_SPI_CS_TXD;
    } else {
        s->cs |= BCM2835_SPI_CS_TXD;
    }

    if (fifo8_is_empty(&s->tx_fifo) && s->cs & BCM2835_SPI_CS_TA) {
        s->cs |= BCM2835_SPI_CS_DONE;
    } else {
        s->cs &= ~BCM2835_SPI_CS_DONE;
    }
}

static void bcm2835_spi_flush_tx_fifo(BCM2835SPIState *s)
{
    uint8_t tx_byte, rx_byte;

    while (!fifo8_is_empty(&s->tx_fifo) && !fifo8_is_full(&s->rx_fifo)) {
        tx_byte = fifo8_pop(&s->tx_fifo);
        rx_byte = ssi_transfer(s->bus, tx_byte);
        fifo8_push(&s->rx_fifo, rx_byte);
    }

    bcm2835_spi_update_tx_flags(s);
    bcm2835_spi_update_rx_flags(s);
}

static uint64_t bcm2835_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2835SPIState *s = static_cast<BCM2835SPIState *>(opaque);
    uint32_t readval = 0;

    switch (addr) {
    case BCM2835_SPI_CS:
        readval = s->cs & 0xffffffff;
        break;
    case BCM2835_SPI_FIFO:
        bcm2835_spi_flush_tx_fifo(s);
        if (s->cs & BCM2835_SPI_CS_RXD) {
            readval = fifo8_pop(&s->rx_fifo);
            bcm2835_spi_update_rx_flags(s);
        }

        bcm2835_spi_update_int(s);
        break;
    case BCM2835_SPI_CLK:
        readval = s->clk & 0xffff;
        break;
    case BCM2835_SPI_DLEN:
        readval = s->dlen & 0xffff;
        break;
    case BCM2835_SPI_LTOH:
        readval = s->ltoh & 0xf;
        break;
    case BCM2835_SPI_DC:
        readval = s->dc & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
    return readval;
}

static void bcm2835_spi_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    BCM2835SPIState *s = static_cast<BCM2835SPIState *>(opaque);

    switch (addr) {
    case BCM2835_SPI_CS:
        s->cs = (value & ~RO_MASK) | (s->cs & RO_MASK);
        if (!(s->cs & BCM2835_SPI_CS_TA)) {
            s->cs &= ~(BCM2835_SPI_CS_DONE);
            s->cs &= ~(BCM2835_SPI_CS_RXR);
        }

        if (s->cs & BCM2835_SPI_CLEAR_RX) {
            fifo8_reset(&s->rx_fifo);
            bcm2835_spi_update_rx_flags(s);
        }

        if (s->cs & BCM2835_SPI_CLEAR_TX) {
            fifo8_reset(&s->tx_fifo);
            bcm2835_spi_update_tx_flags(s);
        }

        if (s->cs & BCM2835_SPI_CS_TA) {
            bcm2835_spi_update_tx_flags(s);
        }

        if (s->cs & BCM2835_SPI_CS_DMAEN) {
            qemu_log_mask(LOG_UNIMP, "%s: " \
                          "DMA not supported\n", __func__);
        }

        if (s->cs & BCM2835_SPI_CS_LEN) {
            qemu_log_mask(LOG_UNIMP, "%s: " \
                          "LoSSI not supported\n", __func__);
        }

        bcm2835_spi_update_int(s);
        break;
    case BCM2835_SPI_FIFO:
        if (s->cs & BCM2835_SPI_CS_TA) {
            if (s->cs & BCM2835_SPI_CS_TXD) {
                fifo8_push(&s->tx_fifo, value & 0xff);
                bcm2835_spi_update_tx_flags(s);
            }

            bcm2835_spi_flush_tx_fifo(s);
            bcm2835_spi_update_int(s);
        }
        break;
    case BCM2835_SPI_CLK:
        s->clk = value & 0xffff;
        break;
    case BCM2835_SPI_DLEN:
        s->dlen = value & 0xffff;
        break;
    case BCM2835_SPI_LTOH:
        s->ltoh = value & 0xf;
        break;
    case BCM2835_SPI_DC:
        s->dc = value & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps bcm2835_spi_ops = {
    .read = bcm2835_spi_read,
    .write = bcm2835_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void BCM2835SPIState::realize(Error **errp)
{
    bus = ssi_create_bus(DEVICE(this), "spi");

    memory_region_init_io(&iomem, OBJECT(this), &bcm2835_spi_ops, this,
                          TYPE_BCM2835_SPI, 0x18);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(this), &irq);

    fifo8_create(&tx_fifo, FIFO_SIZE);
    fifo8_create(&rx_fifo, FIFO_SIZE);
}

void BCM2835SPIState::reset()
{
    fifo8_reset(&tx_fifo);
    fifo8_reset(&rx_fifo);

    cs = BCM2835_SPI_CS_TXD | BCM2835_SPI_CS_REN;
    clk = 0;
    dlen = 0;
    ltoh = 0x1;
    dc = 0x30201020;
}

static const VMStateField vmstate_bcm2835_spi_fields[] = {
    VMSTATE_FIFO8(tx_fifo, BCM2835SPIState),
    VMSTATE_FIFO8(rx_fifo, BCM2835SPIState),
    VMSTATE_UINT32(cs, BCM2835SPIState),
    VMSTATE_UINT32(clk, BCM2835SPIState),
    VMSTATE_UINT32(dlen, BCM2835SPIState),
    VMSTATE_UINT32(ltoh, BCM2835SPIState),
    VMSTATE_UINT32(dc, BCM2835SPIState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_bcm2835_spi = {
    .name = TYPE_BCM2835_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_bcm2835_spi_fields,
};

void BCM2835SPIState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_bcm2835_spi;
}

REGISTER_QEMU_DEVICE(BCM2835SPIState, TYPE_BCM2835_SPI, TYPE_SYS_BUS_DEVICE)
