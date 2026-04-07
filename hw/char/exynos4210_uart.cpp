/*
 *  Exynos4210 UART Emulation
 *
 *  Copyright (C) 2011 Samsung Electronics Co Ltd.
 *    Maksim Kozlov, <m.kozlov@samsung.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"

#include "hw/arm/exynos4210.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"

#include "trace.h"
#include "qom/object.h"

/*
 *  Offsets for UART registers relative to SFR base address
 *  for UARTn
 *
 */
#define ULCON      0x0000 /* Line Control             */
#define UCON       0x0004 /* Control                  */
#define UFCON      0x0008 /* FIFO Control             */
#define UMCON      0x000C /* Modem Control            */
#define UTRSTAT    0x0010 /* Tx/Rx Status             */
#define UERSTAT    0x0014 /* UART Error Status        */
#define UFSTAT     0x0018 /* FIFO Status              */
#define UMSTAT     0x001C /* Modem Status             */
#define UTXH       0x0020 /* Transmit Buffer          */
#define URXH       0x0024 /* Receive Buffer           */
#define UBRDIV     0x0028 /* Baud Rate Divisor        */
#define UFRACVAL   0x002C /* Divisor Fractional Value */
#define UINTP      0x0030 /* Interrupt Pending        */
#define UINTSP     0x0034 /* Interrupt Source Pending */
#define UINTM      0x0038 /* Interrupt Mask           */

/*
 * for indexing register in the uint32_t array
 *
 * 'reg' - register offset (see offsets definitions above)
 *
 */
#define I_(reg) (reg / sizeof(uint32_t))

typedef struct Exynos4210UartReg {
    const char         *name; /* the only reason is the debug output */
    hwaddr  offset;
    uint32_t            reset_value;
} Exynos4210UartReg;

static const Exynos4210UartReg exynos4210_uart_regs[] = {
    {"ULCON",    ULCON,    0x00000000},
    {"UCON",     UCON,     0x00003000},
    {"UFCON",    UFCON,    0x00000000},
    {"UMCON",    UMCON,    0x00000000},
    {"UTRSTAT",  UTRSTAT,  0x00000006}, /* RO */
    {"UERSTAT",  UERSTAT,  0x00000000}, /* RO */
    {"UFSTAT",   UFSTAT,   0x00000000}, /* RO */
    {"UMSTAT",   UMSTAT,   0x00000000}, /* RO */
    {"UTXH",     UTXH,     0x5c5c5c5c}, /* WO, undefined reset value*/
    {"URXH",     URXH,     0x00000000}, /* RO */
    {"UBRDIV",   UBRDIV,   0x00000000},
    {"UFRACVAL", UFRACVAL, 0x00000000},
    {"UINTP",    UINTP,    0x00000000},
    {"UINTSP",   UINTSP,   0x00000000},
    {"UINTM",    UINTM,    0x00000000},
};

#define EXYNOS4210_UART_REGS_MEM_SIZE    0x3C

/* UART FIFO Control */
#define UFCON_FIFO_ENABLE                    0x1
#define UFCON_Rx_FIFO_RESET                  0x2
#define UFCON_Tx_FIFO_RESET                  0x4
#define UFCON_Tx_FIFO_TRIGGER_LEVEL_SHIFT    8
#define UFCON_Tx_FIFO_TRIGGER_LEVEL (7 << UFCON_Tx_FIFO_TRIGGER_LEVEL_SHIFT)
#define UFCON_Rx_FIFO_TRIGGER_LEVEL_SHIFT    4
#define UFCON_Rx_FIFO_TRIGGER_LEVEL (7 << UFCON_Rx_FIFO_TRIGGER_LEVEL_SHIFT)

/* Uart FIFO Status */
#define UFSTAT_Rx_FIFO_COUNT        0xff
#define UFSTAT_Rx_FIFO_FULL         0x100
#define UFSTAT_Rx_FIFO_ERROR        0x200
#define UFSTAT_Tx_FIFO_COUNT_SHIFT  16
#define UFSTAT_Tx_FIFO_COUNT        (0xff << UFSTAT_Tx_FIFO_COUNT_SHIFT)
#define UFSTAT_Tx_FIFO_FULL_SHIFT   24
#define UFSTAT_Tx_FIFO_FULL         (1 << UFSTAT_Tx_FIFO_FULL_SHIFT)

/* UART Interrupt Source Pending */
#define UINTSP_RXD      0x1 /* Receive interrupt  */
#define UINTSP_ERROR    0x2 /* Error interrupt    */
#define UINTSP_TXD      0x4 /* Transmit interrupt */
#define UINTSP_MODEM    0x8 /* Modem interrupt    */

/* UART Line Control */
#define ULCON_IR_MODE_SHIFT   6
#define ULCON_PARITY_SHIFT    3
#define ULCON_STOP_BIT_SHIFT  1

/* UART Tx/Rx Status */
#define UTRSTAT_Rx_TIMEOUT              0x8
#define UTRSTAT_TRANSMITTER_EMPTY       0x4
#define UTRSTAT_Tx_BUFFER_EMPTY         0x2
#define UTRSTAT_Rx_BUFFER_DATA_READY    0x1

/* UART Error Status */
#define UERSTAT_OVERRUN  0x1
#define UERSTAT_PARITY   0x2
#define UERSTAT_FRAME    0x4
#define UERSTAT_BREAK    0x8

typedef struct {
    uint8_t    *data;
    uint32_t    sp, rp; /* store and retrieve pointers */
    uint32_t    size;
} Exynos4210UartFIFO;

#define TYPE_EXYNOS4210_UART "exynos4210.uart"
OBJECT_DECLARE_SIMPLE_TYPE(Exynos4210UartState, EXYNOS4210_UART)

struct Exynos4210UartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t             reg[EXYNOS4210_UART_REGS_MEM_SIZE / sizeof(uint32_t)];
    Exynos4210UartFIFO   rx;
    Exynos4210UartFIFO   tx;

    QEMUTimer *fifo_timeout_timer;
    uint64_t wordtime;        /* word time in ns */

    CharFrontend       chr;
    qemu_irq          irq;
    qemu_irq          dmairq;

    uint32_t channel;

    /* instance methods */
    void updateDmabusy();
    void updateIrq();
    void updateParameters();
    void rxTimeoutSet();
    uint32_t txFifoTriggerLevel() const;
    uint32_t rxFifoTriggerLevel() const;
    void reset();
    void initfn();
    void realize(Error **errp);

    /* static callbacks */
    static void timeoutInt(void *opaque);
    static void writeReg(void *opaque, hwaddr offset,
                         uint64_t val, unsigned size);
    static uint64_t readReg(void *opaque, hwaddr offset, unsigned size);
    static int canReceive(void *opaque);
    static void receive(void *opaque, const uint8_t *buf, int size);
    static void event(void *opaque, QEMUChrEvent event);
    static int postLoad(void *opaque, int version_id);

    /* static QOM wrappers */
    static void resetWrapper(DeviceState *dev);
    static void initWrapper(Object *obj);
    static void realizeWrapper(DeviceState *dev, Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
};


/* Used only for tracing */
static const char *exynos4210_uart_regname(hwaddr  offset)
{

    size_t i;

    for (i = 0; i < ARRAY_SIZE(exynos4210_uart_regs); i++) {
        if (offset == exynos4210_uart_regs[i].offset) {
            return exynos4210_uart_regs[i].name;
        }
    }

    return NULL;
}


static void fifo_store(Exynos4210UartFIFO *q, uint8_t ch)
{
    q->data[q->sp] = ch;
    q->sp = (q->sp + 1) % q->size;
}

static uint8_t fifo_retrieve(Exynos4210UartFIFO *q)
{
    uint8_t ret = q->data[q->rp];
    q->rp = (q->rp + 1) % q->size;
    return  ret;
}

static int fifo_elements_number(const Exynos4210UartFIFO *q)
{
    if (q->sp < q->rp) {
        return q->size - q->rp + q->sp;
    }

    return q->sp - q->rp;
}

static int fifo_empty_elements_number(const Exynos4210UartFIFO *q)
{
    return q->size - fifo_elements_number(q);
}

static void fifo_reset(Exynos4210UartFIFO *q)
{
    g_free(q->data);
    q->data = NULL;

    q->data = static_cast<uint8_t *>(g_malloc0(q->size));

    q->sp = 0;
    q->rp = 0;
}

static uint32_t exynos4210_uart_FIFO_trigger_level(uint32_t channel,
                                                   uint32_t reg)
{
    uint32_t level;

    switch (channel) {
    case 0:
        level = reg * 32;
        break;
    case 1:
    case 4:
        level = reg * 8;
        break;
    case 2:
    case 3:
        level = reg * 2;
        break;
    default:
        level = 0;
        trace_exynos_uart_channel_error(channel);
        break;
    }
    return level;
}

uint32_t Exynos4210UartState::txFifoTriggerLevel() const
{
    uint32_t r;

    r = (reg[I_(UFCON)] & UFCON_Tx_FIFO_TRIGGER_LEVEL) >>
            UFCON_Tx_FIFO_TRIGGER_LEVEL_SHIFT;

    return exynos4210_uart_FIFO_trigger_level(channel, r);
}

uint32_t Exynos4210UartState::rxFifoTriggerLevel() const
{
    uint32_t r;

    r = ((reg[I_(UFCON)] & UFCON_Rx_FIFO_TRIGGER_LEVEL) >>
            UFCON_Rx_FIFO_TRIGGER_LEVEL_SHIFT) + 1;

    return exynos4210_uart_FIFO_trigger_level(channel, r);
}

/*
 * Update Rx DMA busy signal if Rx DMA is enabled.
 */
void Exynos4210UartState::updateDmabusy()
{
    bool rx_dma_enabled = (reg[I_(UCON)] & 0x03) == 0x02;
    uint32_t count = fifo_elements_number(&rx);

    if (rx_dma_enabled && !count) {
        qemu_irq_raise(dmairq);
        trace_exynos_uart_dmabusy(channel);
    } else {
        qemu_irq_lower(dmairq);
        trace_exynos_uart_dmaready(channel);
    }
}

void Exynos4210UartState::updateIrq()
{
    /*
     * The Tx interrupt is always requested if the number of data in the
     * transmit FIFO is smaller than the trigger level.
     */
    if (reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
        uint32_t count = (reg[I_(UFSTAT)] & UFSTAT_Tx_FIFO_COUNT) >>
                UFSTAT_Tx_FIFO_COUNT_SHIFT;

        if (count <= txFifoTriggerLevel()) {
            reg[I_(UINTSP)] |= UINTSP_TXD;
        }

        /*
         * Rx interrupt if trigger level is reached or if rx timeout
         * interrupt is disabled and there is data in the receive buffer
         */
        count = fifo_elements_number(&rx);
        if ((count && !(reg[I_(UCON)] & 0x80)) ||
            count >= rxFifoTriggerLevel()) {
            updateDmabusy();
            reg[I_(UINTSP)] |= UINTSP_RXD;
            timer_del(fifo_timeout_timer);
        }
    } else if (reg[I_(UTRSTAT)] & UTRSTAT_Rx_BUFFER_DATA_READY) {
        updateDmabusy();
        reg[I_(UINTSP)] |= UINTSP_RXD;
    }

    reg[I_(UINTP)] = reg[I_(UINTSP)] & ~reg[I_(UINTM)];

    if (reg[I_(UINTP)]) {
        qemu_irq_raise(irq);
        trace_exynos_uart_irq_raised(channel, reg[I_(UINTP)]);
    } else {
        qemu_irq_lower(irq);
        trace_exynos_uart_irq_lowered(channel);
    }
}

void Exynos4210UartState::timeoutInt(void *opaque)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);

    trace_exynos_uart_rx_timeout(s->channel, s->reg[I_(UTRSTAT)],
                                 s->reg[I_(UINTSP)]);

    if ((s->reg[I_(UTRSTAT)] & UTRSTAT_Rx_BUFFER_DATA_READY) ||
        (s->reg[I_(UCON)] & (1 << 11))) {
        s->reg[I_(UINTSP)] |= UINTSP_RXD;
        s->reg[I_(UTRSTAT)] |= UTRSTAT_Rx_TIMEOUT;
        s->updateDmabusy();
        s->updateIrq();
    }
}

void Exynos4210UartState::updateParameters()
{
    int speed, parity, data_bits, stop_bits;
    QEMUSerialSetParams ssp;
    uint64_t uclk_rate;

    if (reg[I_(UBRDIV)] == 0) {
        return;
    }

    if (reg[I_(ULCON)] & 0x20) {
        if (reg[I_(ULCON)] & 0x28) {
            parity = 'E';
        } else {
            parity = 'O';
        }
    } else {
        parity = 'N';
    }

    if (reg[I_(ULCON)] & 0x4) {
        stop_bits = 2;
    } else {
        stop_bits = 1;
    }

    data_bits = (reg[I_(ULCON)] & 0x3) + 5;

    uclk_rate = 24000000;

    speed = uclk_rate / ((16 * (reg[I_(UBRDIV)]) & 0xffff) +
            (reg[I_(UFRACVAL)] & 0x7) + 16);

    ssp.speed     = speed;
    ssp.parity    = parity;
    ssp.data_bits = data_bits;
    ssp.stop_bits = stop_bits;

    wordtime = NANOSECONDS_PER_SECOND * (data_bits + stop_bits + 1) / speed;

    qemu_chr_fe_ioctl(&chr, CHR_IOCTL_SERIAL_SET_PARAMS, &ssp);

    trace_exynos_uart_update_params(
                channel, speed, parity, data_bits, stop_bits, wordtime);
}

void Exynos4210UartState::rxTimeoutSet()
{
    if (reg[I_(UCON)] & 0x80) {
        uint32_t timeout = ((reg[I_(UCON)] >> 12) & 0x0f) * wordtime;

        timer_mod(fifo_timeout_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + timeout);
    } else {
        timer_del(fifo_timeout_timer);
    }
}

void Exynos4210UartState::writeReg(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);
    uint8_t ch;

    trace_exynos_uart_write(s->channel, offset,
                            exynos4210_uart_regname(offset), val);

    switch (offset) {
    case ULCON:
    case UBRDIV:
    case UFRACVAL:
        s->reg[I_(offset)] = val;
        s->updateParameters();
        break;
    case UFCON:
        s->reg[I_(UFCON)] = val;
        if (val & UFCON_Rx_FIFO_RESET) {
            fifo_reset(&s->rx);
            s->reg[I_(UFCON)] &= ~UFCON_Rx_FIFO_RESET;
            trace_exynos_uart_rx_fifo_reset(s->channel);
        }
        if (val & UFCON_Tx_FIFO_RESET) {
            fifo_reset(&s->tx);
            s->reg[I_(UFCON)] &= ~UFCON_Tx_FIFO_RESET;
            trace_exynos_uart_tx_fifo_reset(s->channel);
        }
        break;

    case UTXH:
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            s->reg[I_(UTRSTAT)] &= ~(UTRSTAT_TRANSMITTER_EMPTY |
                    UTRSTAT_Tx_BUFFER_EMPTY);
            ch = (uint8_t)val;
            /* XXX this blocks entire thread. Rewrite to use
             * qemu_chr_fe_write and background I/O callbacks */
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
            trace_exynos_uart_tx(s->channel, ch);
            s->reg[I_(UTRSTAT)] |= UTRSTAT_TRANSMITTER_EMPTY |
                    UTRSTAT_Tx_BUFFER_EMPTY;
            s->reg[I_(UINTSP)]  |= UINTSP_TXD;
            s->updateIrq();
        }
        break;

    case UINTP:
        s->reg[I_(UINTP)] &= ~val;
        s->reg[I_(UINTSP)] &= ~val;
        trace_exynos_uart_intclr(s->channel, s->reg[I_(UINTP)]);
        s->updateIrq();
        break;
    case UTRSTAT:
        if (val & UTRSTAT_Rx_TIMEOUT) {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_Rx_TIMEOUT;
        }
        break;
    case UERSTAT:
    case UFSTAT:
    case UMSTAT:
    case URXH:
        trace_exynos_uart_ro_write(
                    s->channel, exynos4210_uart_regname(offset), offset);
        break;
    case UINTSP:
        s->reg[I_(UINTSP)]  &= ~val;
        break;
    case UINTM:
        s->reg[I_(UINTM)] = val;
        s->updateIrq();
        break;
    case UCON:
    case UMCON:
    default:
        s->reg[I_(offset)] = val;
        break;
    }
}

uint64_t Exynos4210UartState::readReg(void *opaque, hwaddr offset,
                                  unsigned size)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);
    uint32_t res;

    switch (offset) {
    case UERSTAT: /* Read Only */
        res = s->reg[I_(UERSTAT)];
        s->reg[I_(UERSTAT)] = 0;
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset), res);
        return res;
    case UFSTAT: /* Read Only */
        s->reg[I_(UFSTAT)] = fifo_elements_number(&s->rx) & 0xff;
        if (fifo_empty_elements_number(&s->rx) == 0) {
            s->reg[I_(UFSTAT)] |= UFSTAT_Rx_FIFO_FULL;
            s->reg[I_(UFSTAT)] &= ~0xff;
        }
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset),
                               s->reg[I_(UFSTAT)]);
        return s->reg[I_(UFSTAT)];
    case URXH:
        if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
            if (fifo_elements_number(&s->rx)) {
                res = fifo_retrieve(&s->rx);
                trace_exynos_uart_rx(s->channel, res);
                if (!fifo_elements_number(&s->rx)) {
                    s->reg[I_(UTRSTAT)] &= ~UTRSTAT_Rx_BUFFER_DATA_READY;
                } else {
                    s->reg[I_(UTRSTAT)] |= UTRSTAT_Rx_BUFFER_DATA_READY;
                }
            } else {
                trace_exynos_uart_rx_error(s->channel);
                s->reg[I_(UINTSP)] |= UINTSP_ERROR;
                s->updateIrq();
                res = 0;
            }
        } else {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_Rx_BUFFER_DATA_READY;
            res = s->reg[I_(URXH)];
        }
        qemu_chr_fe_accept_input(&s->chr);
        s->updateDmabusy();
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset), res);
        return res;
    case UTXH:
        trace_exynos_uart_wo_read(s->channel, exynos4210_uart_regname(offset),
                                  offset);
        break;
    default:
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset),
                               s->reg[I_(offset)]);
        return s->reg[I_(offset)];
    }

    trace_exynos_uart_read(s->channel, offset, exynos4210_uart_regname(offset),
                           0);
    return 0;
}

static const MemoryRegionOps exynos4210_uart_ops = {
    .read = Exynos4210UartState::readReg,
    .write = Exynos4210UartState::writeReg,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .max_access_size = 4,
        .unaligned = false
    },
};

int Exynos4210UartState::canReceive(void *opaque)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);

    if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
        return fifo_empty_elements_number(&s->rx);
    } else {
        return !(s->reg[I_(UTRSTAT)] & UTRSTAT_Rx_BUFFER_DATA_READY);
    }
}

void Exynos4210UartState::receive(void *opaque, const uint8_t *buf, int size)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);
    int i;

    if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
        if (fifo_empty_elements_number(&s->rx) < size) {
            size = fifo_empty_elements_number(&s->rx);
            s->reg[I_(UINTSP)] |= UINTSP_ERROR;
        }
        for (i = 0; i < size; i++) {
            fifo_store(&s->rx, buf[i]);
        }
        s->rxTimeoutSet();
    } else {
        s->reg[I_(URXH)] = buf[0];
    }
    s->reg[I_(UTRSTAT)] |= UTRSTAT_Rx_BUFFER_DATA_READY;

    s->updateIrq();
}


void Exynos4210UartState::event(void *opaque, QEMUChrEvent event)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);

    if (event == CHR_EVENT_BREAK) {
        /* When the RxDn is held in logic 0, then a null byte is pushed into the
         * fifo */
        fifo_store(&s->rx, '\0');
        s->reg[I_(UERSTAT)] |= UERSTAT_BREAK;
        s->updateIrq();
    }
}


void Exynos4210UartState::reset()
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(exynos4210_uart_regs); i++) {
        reg[I_(exynos4210_uart_regs[i].offset)] =
                exynos4210_uart_regs[i].reset_value;
    }

    fifo_reset(&rx);
    fifo_reset(&tx);

    trace_exynos_uart_rxsize(channel, rx.size);
}

int Exynos4210UartState::postLoad(void *opaque, int version_id)
{
    Exynos4210UartState *s = static_cast<Exynos4210UartState *>(opaque);

    s->updateParameters();
    s->rxTimeoutSet();

    return 0;
}

static const VMStateField vmstate_exynos4210_uart_fifo_fields[] = {
    VMSTATE_UINT32(sp, Exynos4210UartFIFO),
    VMSTATE_UINT32(rp, Exynos4210UartFIFO),
    VMSTATE_VBUFFER_UINT32(data, Exynos4210UartFIFO, 1, NULL, size),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_exynos4210_uart_fifo = {
    .name = "exynos4210.uart.fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_exynos4210_uart_fifo_fields,
};

static const VMStateField vmstate_exynos4210_uart_fields[] = {
    VMSTATE_STRUCT(rx, Exynos4210UartState, 1,
                   vmstate_exynos4210_uart_fifo, Exynos4210UartFIFO),
    VMSTATE_UINT32_ARRAY(reg, Exynos4210UartState,
                         EXYNOS4210_UART_REGS_MEM_SIZE / sizeof(uint32_t)),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_exynos4210_uart = {
    .name = "exynos4210.uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = Exynos4210UartState::postLoad,
    .fields = vmstate_exynos4210_uart_fields,
};

DeviceState *exynos4210_uart_create(hwaddr addr,
                                    int fifo_size,
                                    int channel,
                                    Chardev *chr,
                                    qemu_irq irq)
{
    DeviceState  *dev;
    SysBusDevice *bus;

    dev = qdev_new(TYPE_EXYNOS4210_UART);

    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_prop_set_uint32(dev, "channel", channel);
    qdev_prop_set_uint32(dev, "rx-size", fifo_size);
    qdev_prop_set_uint32(dev, "tx-size", fifo_size);

    bus = reinterpret_cast<SysBusDevice *>(dev);
    sysbus_realize_and_unref(bus, &error_fatal);
    if (addr != (hwaddr)-1) {
        sysbus_mmio_map(bus, 0, addr);
    }
    sysbus_connect_irq(bus, 0, irq);

    return dev;
}

void Exynos4210UartState::initfn()
{
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(this);

    wordtime = NANOSECONDS_PER_SECOND * 10 / 9600;

    /* memory mapping */
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this), &exynos4210_uart_ops, this,
                          "exynos4210.uart", EXYNOS4210_UART_REGS_MEM_SIZE);
    sysbus_init_mmio(dev, &iomem);

    sysbus_init_irq(dev, &irq);
    sysbus_init_irq(dev, &dmairq);
}

void Exynos4210UartState::realize(Error **errp)
{
    fifo_timeout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      Exynos4210UartState::timeoutInt, this);

    qemu_chr_fe_set_handlers(&chr, Exynos4210UartState::canReceive,
                             Exynos4210UartState::receive,
                             Exynos4210UartState::event,
                             NULL, this, NULL, true);
}

/* static QOM wrappers */
void Exynos4210UartState::resetWrapper(DeviceState *dev)
{
    Exynos4210UartState *s = reinterpret_cast<Exynos4210UartState *>(dev);
    s->reset();
}

void Exynos4210UartState::initWrapper(Object *obj)
{
    Exynos4210UartState *s = reinterpret_cast<Exynos4210UartState *>(obj);
    s->initfn();
}

void Exynos4210UartState::realizeWrapper(DeviceState *dev, Error **errp)
{
    Exynos4210UartState *s = reinterpret_cast<Exynos4210UartState *>(dev);
    s->realize(errp);
}

static const Property exynos4210_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", Exynos4210UartState, chr),
    DEFINE_PROP_UINT32("channel", Exynos4210UartState, channel, 0),
    DEFINE_PROP_UINT32("rx-size", Exynos4210UartState, rx.size, 16),
    DEFINE_PROP_UINT32("tx-size", Exynos4210UartState, tx.size, 16),
};

void Exynos4210UartState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = Exynos4210UartState::realizeWrapper;
    device_class_set_legacy_reset(dc, Exynos4210UartState::resetWrapper);
    device_class_set_props(dc, exynos4210_uart_properties);
    dc->vmsd = &vmstate_exynos4210_uart;
}

static const TypeInfo exynos4210_uart_info = {
    .name          = TYPE_EXYNOS4210_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Exynos4210UartState),
    .instance_init = Exynos4210UartState::initWrapper,
    .class_init    = Exynos4210UartState::classInit,
};

static void exynos4210_uart_register(void)
{
    type_register_static(&exynos4210_uart_info);
}

type_init(exynos4210_uart_register)
