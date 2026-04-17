/*
 * Block model of SPI controller present in
 * Microsemi's SmartFusion2 and SmartFusion SoCs.
 *
 * Copyright (C) 2017 Subbaraya Sundeep <sundeep.lkml@gmail.com>
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
#include "hw/irq.h"
#include "hw/ssi/mss-spi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qom/cpp/object.h"

#ifndef MSS_SPI_ERR_DEBUG
#define MSS_SPI_ERR_DEBUG   0
#endif

#define DB_PRINT_L(lvl, fmt, args...) do { \
    if (MSS_SPI_ERR_DEBUG >= lvl) { \
        qemu_log("%s: " fmt "\n", __func__, ## args); \
    } \
} while (0)

#define DB_PRINT(fmt, args...) DB_PRINT_L(1, fmt, ## args)

#define FIFO_CAPACITY         32

#define R_SPI_CONTROL         0
#define R_SPI_DFSIZE          1
#define R_SPI_STATUS          2
#define R_SPI_INTCLR          3
#define R_SPI_RX              4
#define R_SPI_TX              5
#define R_SPI_CLKGEN          6
#define R_SPI_SS              7
#define R_SPI_MIS             8
#define R_SPI_RIS             9

#define S_TXDONE             (1 << 0)
#define S_RXRDY              (1 << 1)
#define S_RXCHOVRF           (1 << 2)
#define S_RXFIFOFUL          (1 << 4)
#define S_RXFIFOFULNXT       (1 << 5)
#define S_RXFIFOEMP          (1 << 6)
#define S_RXFIFOEMPNXT       (1 << 7)
#define S_TXFIFOFUL          (1 << 8)
#define S_TXFIFOFULNXT       (1 << 9)
#define S_TXFIFOEMP          (1 << 10)
#define S_TXFIFOEMPNXT       (1 << 11)
#define S_FRAMESTART         (1 << 12)
#define S_SSEL               (1 << 13)
#define S_ACTIVE             (1 << 14)

#define C_ENABLE             (1 << 0)
#define C_MODE               (1 << 1)
#define C_INTRXDATA          (1 << 4)
#define C_INTTXDATA          (1 << 5)
#define C_INTRXOVRFLO        (1 << 6)
#define C_SPS                (1 << 26)
#define C_BIGFIFO            (1 << 29)
#define C_RESET              (1 << 31)

#define FRAMESZ_MASK         0x3F
#define FMCOUNT_MASK         0x00FFFF00
#define FMCOUNT_SHIFT        8
#define FRAMESZ_MAX          32

static void txfifo_reset(MSSSpiState *s)
{
    fifo32_reset(&s->tx_fifo);

    s->regs[R_SPI_STATUS] &= ~S_TXFIFOFUL;
    s->regs[R_SPI_STATUS] |= S_TXFIFOEMP;
}

static void rxfifo_reset(MSSSpiState *s)
{
    fifo32_reset(&s->rx_fifo);

    s->regs[R_SPI_STATUS] &= ~S_RXFIFOFUL;
    s->regs[R_SPI_STATUS] |= S_RXFIFOEMP;
}

static void set_fifodepth(MSSSpiState *s)
{
    unsigned int size = s->regs[R_SPI_DFSIZE] & FRAMESZ_MASK;

    if (size <= 8) {
        s->fifo_depth = 32;
    } else if (size <= 16) {
        s->fifo_depth = 16;
    } else {
        s->fifo_depth = 8;
    }
}

static void update_mis(MSSSpiState *s)
{
    uint32_t reg = s->regs[R_SPI_CONTROL];
    uint32_t tmp;

    /*
     * form the Control register interrupt enable bits
     * same as RIS, MIS and Interrupt clear registers for simplicity
     */
    tmp = ((reg & C_INTRXOVRFLO) >> 4) | ((reg & C_INTRXDATA) >> 3) |
           ((reg & C_INTTXDATA) >> 5);
    s->regs[R_SPI_MIS] |= tmp & s->regs[R_SPI_RIS];
}

static void spi_update_irq(MSSSpiState *s)
{
    int irq;

    update_mis(s);
    irq = !!(s->regs[R_SPI_MIS]);

    qemu_set_irq(s->irq, irq);
}

void MSSSpiState::reset()
{
    memset(regs, 0, sizeof regs);
    regs[R_SPI_CONTROL] = 0x80000102;
    regs[R_SPI_DFSIZE] = 0x4;
    regs[R_SPI_STATUS] = S_SSEL | S_TXFIFOEMP | S_RXFIFOEMP;
    regs[R_SPI_CLKGEN] = 0x7;
    regs[R_SPI_RIS] = 0x0;

    fifo_depth = 4;
    frame_count = 1;
    enabled = false;

    rxfifo_reset(this);
    txfifo_reset(this);
}

static uint64_t
spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    MSSSpiState *s = static_cast<MSSSpiState *>(opaque);
    uint32_t ret = 0;

    addr >>= 2;
    switch (addr) {
    case R_SPI_RX:
        s->regs[R_SPI_STATUS] &= ~S_RXFIFOFUL;
        s->regs[R_SPI_STATUS] &= ~S_RXCHOVRF;
        if (fifo32_is_empty(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Reading empty RX_FIFO\n",
                          __func__);
        } else {
            ret = fifo32_pop(&s->rx_fifo);
        }
        if (fifo32_is_empty(&s->rx_fifo)) {
            s->regs[R_SPI_STATUS] |= S_RXFIFOEMP;
        }
        break;

    case R_SPI_MIS:
        update_mis(s);
        ret = s->regs[R_SPI_MIS];
        break;

    default:
        if (addr < ARRAY_SIZE(s->regs)) {
            ret = s->regs[addr];
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                         addr * 4);
            return ret;
        }
        break;
    }

    DB_PRINT("addr=0x%" HWADDR_PRIx " = 0x%" PRIx32, addr * 4, ret);
    spi_update_irq(s);
    return ret;
}

static void assert_cs(MSSSpiState *s)
{
    qemu_set_irq(s->cs_line, 0);
}

static void deassert_cs(MSSSpiState *s)
{
    qemu_set_irq(s->cs_line, 1);
}

static void spi_flush_txfifo(MSSSpiState *s)
{
    uint32_t tx;
    uint32_t rx;
    bool sps = !!(s->regs[R_SPI_CONTROL] & C_SPS);

    /*
     * Chip Select(CS) is automatically controlled by this controller.
     * If SPS bit is set in Control register then CS is asserted
     * until all the frames set in frame count of Control register are
     * transferred. If SPS is not set then CS pulses between frames.
     * Note that Slave Select register specifies which of the CS line
     * has to be controlled automatically by controller. Bits SS[7:1] are for
     * masters in FPGA fabric since we model only Microcontroller subsystem
     * of Smartfusion2 we control only one CS(SS[0]) line.
     */
    while (!fifo32_is_empty(&s->tx_fifo) && s->frame_count) {
        assert_cs(s);

        s->regs[R_SPI_STATUS] &= ~(S_TXDONE | S_RXRDY);

        tx = fifo32_pop(&s->tx_fifo);
        DB_PRINT("data tx:0x%" PRIx32, tx);
        rx = ssi_transfer(s->spi, tx);
        DB_PRINT("data rx:0x%" PRIx32, rx);

        if (fifo32_num_used(&s->rx_fifo) == s->fifo_depth) {
            s->regs[R_SPI_STATUS] |= S_RXCHOVRF;
            s->regs[R_SPI_RIS] |= S_RXCHOVRF;
        } else {
            fifo32_push(&s->rx_fifo, rx);
            s->regs[R_SPI_STATUS] &= ~S_RXFIFOEMP;
            if (fifo32_num_used(&s->rx_fifo) == (s->fifo_depth - 1)) {
                s->regs[R_SPI_STATUS] |= S_RXFIFOFULNXT;
            } else if (fifo32_num_used(&s->rx_fifo) == s->fifo_depth) {
                s->regs[R_SPI_STATUS] |= S_RXFIFOFUL;
            }
        }
        s->frame_count--;
        if (!sps) {
            deassert_cs(s);
        }
    }

    if (!s->frame_count) {
        s->frame_count = (s->regs[R_SPI_CONTROL] & FMCOUNT_MASK) >>
                            FMCOUNT_SHIFT;
        deassert_cs(s);
        s->regs[R_SPI_RIS] |= S_TXDONE | S_RXRDY;
        s->regs[R_SPI_STATUS] |= S_TXDONE | S_RXRDY;
   }
}

static void spi_write(void *opaque, hwaddr addr,
            uint64_t val64, unsigned int size)
{
    MSSSpiState *s = static_cast<MSSSpiState *>(opaque);
    uint32_t value = val64;

    DB_PRINT("addr=0x%" HWADDR_PRIx " =0x%" PRIx32, addr, value);
    addr >>= 2;

    switch (addr) {
    case R_SPI_TX:
        /* adding to already full FIFO */
        if (fifo32_num_used(&s->tx_fifo) == s->fifo_depth) {
            break;
        }
        s->regs[R_SPI_STATUS] &= ~S_TXFIFOEMP;
        fifo32_push(&s->tx_fifo, value);
        if (fifo32_num_used(&s->tx_fifo) == (s->fifo_depth - 1)) {
            s->regs[R_SPI_STATUS] |= S_TXFIFOFULNXT;
        } else if (fifo32_num_used(&s->tx_fifo) == s->fifo_depth) {
            s->regs[R_SPI_STATUS] |= S_TXFIFOFUL;
        }
        if (s->enabled) {
            spi_flush_txfifo(s);
        }
        break;

    case R_SPI_CONTROL:
        s->regs[R_SPI_CONTROL] = value;
        if (value & C_BIGFIFO) {
            set_fifodepth(s);
        } else {
            s->fifo_depth = 4;
        }
        s->enabled = value & C_ENABLE;
        s->frame_count = (value & FMCOUNT_MASK) >> FMCOUNT_SHIFT;
        if (value & C_RESET) {
            s->reset();
        }
        break;

    case R_SPI_DFSIZE:
        if (s->enabled) {
            break;
        }
        /*
         * [31:6] bits are reserved bits and for future use.
         * [5:0] are for frame size. Only [5:0] bits are validated
         * during write, [31:6] bits are untouched.
         */
        if ((value & FRAMESZ_MASK) > FRAMESZ_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Incorrect size %u provided."
                         "Maximum frame size is %u\n",
                         __func__, value & FRAMESZ_MASK, FRAMESZ_MAX);
            break;
        }
        s->regs[R_SPI_DFSIZE] = value;
        break;

    case R_SPI_INTCLR:
        s->regs[R_SPI_INTCLR] = value;
        if (value & S_TXDONE) {
            s->regs[R_SPI_RIS] &= ~S_TXDONE;
        }
        if (value & S_RXRDY) {
            s->regs[R_SPI_RIS] &= ~S_RXRDY;
        }
        if (value & S_RXCHOVRF) {
            s->regs[R_SPI_RIS] &= ~S_RXCHOVRF;
        }
        break;

    case R_SPI_MIS:
    case R_SPI_STATUS:
    case R_SPI_RIS:
            qemu_log_mask(LOG_GUEST_ERROR,
                         "%s: Write to read only register 0x%" HWADDR_PRIx "\n",
                         __func__, addr * 4);
        break;

    default:
        if (addr < ARRAY_SIZE(s->regs)) {
            s->regs[addr] = value;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                         addr * 4);
        }
        break;
    }

    spi_update_irq(s);
}

static const MemoryRegionOps spi_ops = {
    .read = spi_read,
    .write = spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void MSSSpiState::realize(Error **errp)
{
    spi = ssi_create_bus(DEVICE(this), "spi");

    sysbus_init_irq(SYS_BUS_DEVICE(this), &irq);
    sysbus_init_irq(SYS_BUS_DEVICE(this), &cs_line);

    memory_region_init_io(&mmio, OBJECT(this), &spi_ops, this,
                          TYPE_MSS_SPI, R_SPI_MAX * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &mmio);

    fifo32_create(&tx_fifo, FIFO_CAPACITY);
    fifo32_create(&rx_fifo, FIFO_CAPACITY);
}

static const VMStateField vmstate_mss_spi_fields[] = {
    VMSTATE_FIFO32(tx_fifo, MSSSpiState),
    VMSTATE_FIFO32(rx_fifo, MSSSpiState),
    VMSTATE_UINT32_ARRAY(regs, MSSSpiState, R_SPI_MAX),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_mss_spi = {
    .name = TYPE_MSS_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_mss_spi_fields,
};

void MSSSpiState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_mss_spi;
}

REGISTER_QEMU_DEVICE(MSSSpiState, TYPE_MSS_SPI, TYPE_SYS_BUS_DEVICE)
