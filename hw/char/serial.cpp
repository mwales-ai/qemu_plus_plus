/*
 * QEMU 16550A UART emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2008 Citrix Systems, Inc.
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
#include "qemu/bitops.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "chardev/char-serial.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"

#define UART_LCR_DLAB   0x80    /* Divisor latch access bit */

#define UART_IER_MSI    0x08    /* Enable Modem status interrupt */
#define UART_IER_RLSI   0x04    /* Enable receiver line status interrupt */
#define UART_IER_THRI   0x02    /* Enable Transmitter holding register int. */
#define UART_IER_RDI    0x01    /* Enable receiver data interrupt */

#define UART_IIR_NO_INT 0x01    /* No interrupts pending */
#define UART_IIR_ID     0x06    /* Mask for the interrupt ID */

#define UART_IIR_MSI    0x00    /* Modem status interrupt */
#define UART_IIR_THRI   0x02    /* Transmitter holding register empty */
#define UART_IIR_RDI    0x04    /* Receiver data interrupt */
#define UART_IIR_RLSI   0x06    /* Receiver line status interrupt */
#define UART_IIR_CTI    0x0C    /* Character Timeout Indication */

#define UART_IIR_FENF   0x80    /* Fifo enabled, but not functioning */
#define UART_IIR_FE     0xC0    /* Fifo enabled */

/*
 * These are the definitions for the Modem Control Register
 */
#define UART_MCR_LOOP   0x10    /* Enable loopback test mode */
#define UART_MCR_OUT2   0x08    /* Out2 complement */
#define UART_MCR_OUT1   0x04    /* Out1 complement */
#define UART_MCR_RTS    0x02    /* RTS complement */
#define UART_MCR_DTR    0x01    /* DTR complement */

/*
 * These are the definitions for the Modem Status Register
 */
#define UART_MSR_DCD        0x80    /* Data Carrier Detect */
#define UART_MSR_RI         0x40    /* Ring Indicator */
#define UART_MSR_DSR        0x20    /* Data Set Ready */
#define UART_MSR_CTS        0x10    /* Clear to Send */
#define UART_MSR_DDCD       0x08    /* Delta DCD */
#define UART_MSR_TERI       0x04    /* Trailing edge ring indicator */
#define UART_MSR_DDSR       0x02    /* Delta DSR */
#define UART_MSR_DCTS       0x01    /* Delta CTS */
#define UART_MSR_ANY_DELTA  0x0F    /* Any of the delta bits! */

#define UART_LSR_TEMT       0x40    /* Transmitter empty */
#define UART_LSR_THRE       0x20    /* Transmit-hold-register empty */
#define UART_LSR_BI         0x10    /* Break interrupt indicator */
#define UART_LSR_FE         0x08    /* Frame error indicator */
#define UART_LSR_PE         0x04    /* Parity error indicator */
#define UART_LSR_OE         0x02    /* Overrun error indicator */
#define UART_LSR_DR         0x01    /* Receiver data ready */
#define UART_LSR_INT_ANY    0x1E    /* Any of the lsr-interrupt-triggering status bits */

/* Interrupt trigger levels. The byte-counts are for 16550A - in newer UARTs the byte-count for each ITL is higher. */

#define UART_FCR_ITL_1      0x00 /* 1 byte ITL */
#define UART_FCR_ITL_2      0x40 /* 4 bytes ITL */
#define UART_FCR_ITL_3      0x80 /* 8 bytes ITL */
#define UART_FCR_ITL_4      0xC0 /* 14 bytes ITL */

#define UART_FCR_DMS        0x08    /* DMA Mode Select */
#define UART_FCR_XFR        0x04    /* XMIT Fifo Reset */
#define UART_FCR_RFR        0x02    /* RCVR Fifo Reset */
#define UART_FCR_FE         0x01    /* FIFO Enable */

#define MAX_XMIT_RETRY      4

/* Forward declarations for static callbacks */
static void serial_receive1(void *opaque, const uint8_t *buf, int size);
static void serial_reset_cb(void *opaque);

void SerialState::recvFifoPut(uint8_t chr)
{
    /* Receive overruns do not overwrite FIFO contents. */
    if (!fifo8_is_full(&recv_fifo)) {
        fifo8_push(&recv_fifo, chr);
    } else {
        lsr |= UART_LSR_OE;
    }
}

void SerialState::updateIrq()
{
    uint8_t tmp_iir = UART_IIR_NO_INT;

    if ((ier & UART_IER_RLSI) && (lsr & UART_LSR_INT_ANY)) {
        tmp_iir = UART_IIR_RLSI;
    } else if ((ier & UART_IER_RDI) && timeout_ipending) {
        /* Note that(ier & UART_IER_RDI) can mask this interrupt,
         * this is not in the specification but is observed on existing
         * hardware.  */
        tmp_iir = UART_IIR_CTI;
    } else if ((ier & UART_IER_RDI) && (lsr & UART_LSR_DR) &&
               (!(fcr & UART_FCR_FE) ||
                recv_fifo.num >= recv_fifo_itl)) {
        tmp_iir = UART_IIR_RDI;
    } else if ((ier & UART_IER_THRI) && thr_ipending) {
        tmp_iir = UART_IIR_THRI;
    } else if ((ier & UART_IER_MSI) && (msr & UART_MSR_ANY_DELTA)) {
        tmp_iir = UART_IIR_MSI;
    }

    iir = tmp_iir | (iir & 0xF0);

    if (tmp_iir != UART_IIR_NO_INT) {
        qemu_irq_raise(irq);
    } else {
        qemu_irq_lower(irq);
    }
}

void SerialState::updateParameters()
{
    float speed;
    int parity, data_bits, stop_bits, frame_size;
    QEMUSerialSetParams ssp;

    /* Start bit. */
    frame_size = 1;
    if (lcr & 0x08) {
        /* Parity bit. */
        frame_size++;
        if (lcr & 0x10)
            parity = 'E';
        else
            parity = 'O';
    } else {
            parity = 'N';
    }
    if (lcr & 0x04) {
        stop_bits = 2;
    } else {
        stop_bits = 1;
    }

    data_bits = (lcr & 0x03) + 5;
    frame_size += data_bits + stop_bits;
    /* Zero divisor should give about 3500 baud */
    speed = (divider == 0) ? 3500 : (float) baudbase / divider;
    ssp.speed = speed;
    ssp.parity = parity;
    ssp.data_bits = data_bits;
    ssp.stop_bits = stop_bits;
    char_transmit_time = (NANOSECONDS_PER_SECOND / speed) * frame_size;
    qemu_chr_fe_ioctl(&chr, CHR_IOCTL_SERIAL_SET_PARAMS, &ssp);
    trace_serial_update_parameters(speed, parity, data_bits, stop_bits);
}

void SerialState::updateMsl()
{
    uint8_t omsr;
    int flags;

    timer_del(modem_status_poll);

    if (qemu_chr_fe_ioctl(&chr, CHR_IOCTL_SERIAL_GET_TIOCM,
                          &flags) == -ENOTSUP) {
        poll_msl = -1;
        return;
    }

    omsr = msr;

    msr = (flags & CHR_TIOCM_CTS) ? msr | UART_MSR_CTS : msr & ~UART_MSR_CTS;
    msr = (flags & CHR_TIOCM_DSR) ? msr | UART_MSR_DSR : msr & ~UART_MSR_DSR;
    msr = (flags & CHR_TIOCM_CAR) ? msr | UART_MSR_DCD : msr & ~UART_MSR_DCD;
    msr = (flags & CHR_TIOCM_RI) ? msr | UART_MSR_RI : msr & ~UART_MSR_RI;

    if (msr != omsr) {
         /* Set delta bits */
         msr = msr | ((msr >> 4) ^ (omsr >> 4));
         /* UART_MSR_TERI only if change was from 1 -> 0 */
         if ((msr & UART_MSR_TERI) && !(omsr & UART_MSR_RI))
             msr &= ~UART_MSR_TERI;
         updateIrq();
    }

    /* The real 16550A apparently has a 250ns response latency to line status changes.
       We'll be lazy and poll only every 10ms, and only poll it at all if MSI interrupts are turned on */

    if (poll_msl) {
        timer_mod(modem_status_poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / 100);
    }
}

/* Timer callback wrapper for updateMsl */
static void serial_update_msl_cb(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    s->updateMsl();
}

static gboolean serial_watch_cb(void *do_not_use, GIOCondition cond,
                                void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    s->watch_tag = 0;
    s->xmitFifoGet();
    return G_SOURCE_REMOVE;
}

void SerialState::xmitFifoGet()
{
    do {
        assert(!(lsr & UART_LSR_TEMT));
        if (tsr_retry == 0) {
            assert(!(lsr & UART_LSR_THRE));

            if (fcr & UART_FCR_FE) {
                assert(!fifo8_is_empty(&xmit_fifo));
                tsr = fifo8_pop(&xmit_fifo);
                if (!xmit_fifo.num) {
                    lsr |= UART_LSR_THRE;
                }
            } else {
                tsr = thr;
                lsr |= UART_LSR_THRE;
            }
            if ((lsr & UART_LSR_THRE) && !thr_ipending) {
                thr_ipending = 1;
                updateIrq();
            }
        }

        if (mcr & UART_MCR_LOOP) {
            /* in loopback mode, say that we just received a char */
            serial_receive1(this, &tsr, 1);
        } else {
            int rc = qemu_chr_fe_write(&chr, &tsr, 1);

            if ((rc == 0 ||
                 (rc == -1 && errno == EAGAIN)) &&
                tsr_retry < MAX_XMIT_RETRY) {
                assert(watch_tag == 0);
                watch_tag =
                    qemu_chr_fe_add_watch(&chr, static_cast<GIOCondition>(G_IO_OUT | G_IO_HUP),
                                          serial_watch_cb, this);
                if (watch_tag > 0) {
                    tsr_retry++;
                    return;
                }
            }
        }
        tsr_retry = 0;

        /* Transmit another byte if it is already available. It is only
           possible when FIFO is enabled and not empty. */
    } while (!(lsr & UART_LSR_THRE));

    last_xmit_ts = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    lsr |= UART_LSR_TEMT;
}

/* Setter for FCR.
   is_load flag means, that value is set while loading VM state
   and interrupt should not be invoked */
static void serial_write_fcr(SerialState *s, uint8_t val)
{
    /* Set fcr - val only has the bits that are supposed to "stick" */
    s->fcr = val;

    if (val & UART_FCR_FE) {
        s->iir |= UART_IIR_FE;
        /* Set recv_fifo trigger Level */
        switch (val & 0xC0) {
        case UART_FCR_ITL_1:
            s->recv_fifo_itl = 1;
            break;
        case UART_FCR_ITL_2:
            s->recv_fifo_itl = 4;
            break;
        case UART_FCR_ITL_3:
            s->recv_fifo_itl = 8;
            break;
        case UART_FCR_ITL_4:
            s->recv_fifo_itl = 14;
            break;
        }
    } else {
        s->iir &= ~UART_IIR_FE;
    }
}

static void serial_update_tiocm(SerialState *s)
{
    int flags;

    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_GET_TIOCM, &flags);

    flags &= ~(CHR_TIOCM_RTS | CHR_TIOCM_DTR);

    if (s->mcr & UART_MCR_RTS) {
        flags |= CHR_TIOCM_RTS;
    }
    if (s->mcr & UART_MCR_DTR) {
        flags |= CHR_TIOCM_DTR;
    }

    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_TIOCM, &flags);
}

void SerialState::writeReg(hwaddr addr, uint64_t val)
{
    assert(addr < 8);
    trace_serial_write(addr, val);
    switch(addr) {
    default:
    case 0:
        if (lcr & UART_LCR_DLAB) {
            divider = deposit32(divider, 8 * addr, 8, val);
            updateParameters();
        } else {
            thr = (uint8_t) val;
            if(fcr & UART_FCR_FE) {
                /* xmit overruns overwrite data, so make space if needed */
                if (fifo8_is_full(&xmit_fifo)) {
                    fifo8_pop(&xmit_fifo);
                }
                fifo8_push(&xmit_fifo, thr);
            }
            thr_ipending = 0;
            lsr &= ~UART_LSR_THRE;
            lsr &= ~UART_LSR_TEMT;
            updateIrq();
            if (tsr_retry == 0) {
                xmitFifoGet();
            }
        }
        break;
    case 1:
        if (lcr & UART_LCR_DLAB) {
            divider = deposit32(divider, 8 * addr, 8, val);
            updateParameters();
        } else {
            uint8_t changed = (ier ^ val) & 0x0f;
            ier = val & 0x0f;
            /* If the backend device is a real serial port, turn polling of the modem
             * status lines on physical port on or off depending on UART_IER_MSI state.
             */
            if ((changed & UART_IER_MSI) && poll_msl >= 0) {
                if (ier & UART_IER_MSI) {
                     poll_msl = 1;
                     updateMsl();
                } else {
                     timer_del(modem_status_poll);
                     poll_msl = 0;
                }
            }

            /* Turning on the THRE interrupt on IER can trigger the interrupt
             * if LSR.THRE=1, even if it had been masked before by reading IIR.
             * This is not in the datasheet, but Windows relies on it.  It is
             * unclear if THRE has to be resampled every time THRI becomes
             * 1, or only on the rising edge.  Bochs does the latter, and Windows
             * always toggles IER to all zeroes and back to all ones, so do the
             * same.
             *
             * If IER.THRI is zero, thr_ipending is not used.  Set it to zero
             * so that the thr_ipending subsection is not migrated.
             */
            if (changed & UART_IER_THRI) {
                if ((ier & UART_IER_THRI) && (lsr & UART_LSR_THRE)) {
                    thr_ipending = 1;
                } else {
                    thr_ipending = 0;
                }
            }

            if (changed) {
                updateIrq();
            }
        }
        break;
    case 2:
        /* Did the enable/disable flag change? If so, make sure FIFOs get flushed */
        if ((val ^ fcr) & UART_FCR_FE) {
            val |= UART_FCR_XFR | UART_FCR_RFR;
        }

        /* FIFO clear */

        if (val & UART_FCR_RFR) {
            lsr &= ~(UART_LSR_DR | UART_LSR_BI);
            timer_del(fifo_timeout_timer);
            timeout_ipending = 0;
            fifo8_reset(&recv_fifo);
        }

        if (val & UART_FCR_XFR) {
            lsr |= UART_LSR_THRE;
            thr_ipending = 1;
            fifo8_reset(&xmit_fifo);
        }

        serial_write_fcr(this, val & 0xC9);
        updateIrq();
        break;
    case 3:
        {
            int break_enable;
            lcr = val;
            updateParameters();
            break_enable = (val >> 6) & 1;
            if (break_enable != last_break_enable) {
                last_break_enable = break_enable;
                qemu_chr_fe_ioctl(&chr, CHR_IOCTL_SERIAL_SET_BREAK,
                                  &break_enable);
            }
        }
        break;
    case 4:
        {
            int old_mcr = mcr;
            mcr = val & 0x1f;
            if (val & UART_MCR_LOOP)
                break;

            if (poll_msl >= 0 && old_mcr != mcr) {
                serial_update_tiocm(this);
                /* Update the modem status after a one-character-send wait-time, since there may be a response
                   from the device/computer at the other end of the serial line */
                timer_mod(modem_status_poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + char_transmit_time);
            }
        }
        break;
    case 5:
        break;
    case 6:
        break;
    case 7:
        scr = val;
        break;
    }
}

uint64_t SerialState::readReg(hwaddr addr)
{
    uint32_t ret;

    assert(addr < 8);
    switch(addr) {
    default:
    case 0:
        if (lcr & UART_LCR_DLAB) {
            ret = extract16(divider, 8 * addr, 8);
        } else {
            if(fcr & UART_FCR_FE) {
                ret = fifo8_is_empty(&recv_fifo) ?
                            0 : fifo8_pop(&recv_fifo);
                if (recv_fifo.num == 0) {
                    lsr &= ~(UART_LSR_DR | UART_LSR_BI);
                } else {
                    timer_mod(fifo_timeout_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + char_transmit_time * 4);
                }
                timeout_ipending = 0;
            } else {
                ret = rbr;
                lsr &= ~(UART_LSR_DR | UART_LSR_BI);
            }
            updateIrq();
            if (!(mcr & UART_MCR_LOOP)) {
                /* in loopback mode, don't receive any data */
                qemu_chr_fe_accept_input(&chr);
            }
        }
        break;
    case 1:
        if (lcr & UART_LCR_DLAB) {
            ret = extract16(divider, 8 * addr, 8);
        } else {
            ret = ier;
        }
        break;
    case 2:
        ret = iir;
        if ((ret & UART_IIR_ID) == UART_IIR_THRI) {
            thr_ipending = 0;
            updateIrq();
        }
        break;
    case 3:
        ret = lcr;
        break;
    case 4:
        ret = mcr;
        break;
    case 5:
        ret = lsr;
        /* Clear break and overrun interrupts */
        if (lsr & (UART_LSR_BI|UART_LSR_OE)) {
            lsr &= ~(UART_LSR_BI|UART_LSR_OE);
            updateIrq();
        }
        break;
    case 6:
        if (mcr & UART_MCR_LOOP) {
            /* in loopback, the modem output pins are connected to the
               inputs */
            ret = (mcr & 0x0c) << 4;
            ret |= (mcr & 0x02) << 3;
            ret |= (mcr & 0x01) << 5;
        } else {
            if (poll_msl >= 0)
                updateMsl();
            ret = msr;
            /* Clear delta bits & msr int after read, if they were set */
            if (msr & UART_MSR_ANY_DELTA) {
                msr &= 0xF0;
                updateIrq();
            }
        }
        break;
    case 7:
        ret = scr;
        break;
    }
    trace_serial_read(addr, ret);
    return ret;
}

/* MMIO callback wrappers that delegate to methods */
static void serial_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    s->writeReg(addr, val);
}

static uint64_t serial_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    return s->readReg(addr);
}

static int serial_can_receive(SerialState *s)
{
    if(s->fcr & UART_FCR_FE) {
        if (s->recv_fifo.num < UART_FIFO_LENGTH) {
            /*
             * Advertise (fifo.itl - fifo.count) bytes when count < ITL, and 1
             * if above. If UART_FIFO_LENGTH - fifo.count is advertised the
             * effect will be to almost always fill the fifo completely before
             * the guest has a chance to respond, effectively overriding the ITL
             * that the guest has set.
             */
            return (s->recv_fifo.num <= s->recv_fifo_itl) ?
                        s->recv_fifo_itl - s->recv_fifo.num : 1;
        } else {
            return 0;
        }
    } else {
        return !(s->lsr & UART_LSR_DR);
    }
}

static void serial_receive_break(SerialState *s)
{
    s->rbr = 0;
    /* When the LSR_DR is set a null byte is pushed into the fifo */
    s->recvFifoPut('\0');
    s->lsr |= UART_LSR_BI | UART_LSR_DR;
    s->updateIrq();
}

/* There's data in recv_fifo and s->rbr has not been read for 4 char transmit times */
static void fifo_timeout_int (void *opaque) {
    SerialState *s = static_cast<SerialState *>(opaque);
    if (s->recv_fifo.num) {
        s->timeout_ipending = 1;
        s->updateIrq();
    }
}

static int serial_can_receive1(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    return serial_can_receive(s);
}

static void serial_receive1(void *opaque, const uint8_t *buf, int size)
{
    SerialState *s = static_cast<SerialState *>(opaque);

    if (s->wakeup) {
        qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);
    }
    if(s->fcr & UART_FCR_FE) {
        int i;
        for (i = 0; i < size; i++) {
            s->recvFifoPut(buf[i]);
        }
        s->lsr |= UART_LSR_DR;
        /* call the timeout receive callback in 4 char transmit time */
        timer_mod(s->fifo_timeout_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->char_transmit_time * 4);
    } else {
        if (s->lsr & UART_LSR_DR)
            s->lsr |= UART_LSR_OE;
        s->rbr = buf[0];
        s->lsr |= UART_LSR_DR;
    }
    s->updateIrq();
}

static void serial_event(void *opaque, QEMUChrEvent event)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    if (event == CHR_EVENT_BREAK)
        serial_receive_break(s);
}

static int serial_pre_save(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    s->fcr_vmstate = s->fcr;

    return 0;
}

static int serial_pre_load(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    s->thr_ipending = -1;
    s->poll_msl = -1;
    return 0;
}

static int serial_post_load(void *opaque, int version_id)
{
    SerialState *s = static_cast<SerialState *>(opaque);

    if (version_id < 3) {
        s->fcr_vmstate = 0;
    }
    if (s->thr_ipending == -1) {
        s->thr_ipending = ((s->iir & UART_IIR_ID) == UART_IIR_THRI);
    }

    if (s->tsr_retry > 0) {
        /* tsr_retry > 0 implies LSR.TEMT = 0 (transmitter not empty).  */
        if (s->lsr & UART_LSR_TEMT) {
            error_report("inconsistent state in serial device "
                         "(tsr empty, tsr_retry=%d", s->tsr_retry);
            return -1;
        }

        if (s->tsr_retry > MAX_XMIT_RETRY) {
            s->tsr_retry = MAX_XMIT_RETRY;
        }

        assert(s->watch_tag == 0);
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, static_cast<GIOCondition>(G_IO_OUT | G_IO_HUP),
                                             serial_watch_cb, s);
    } else {
        /* tsr_retry == 0 implies LSR.TEMT = 1 (transmitter empty).  */
        if (!(s->lsr & UART_LSR_TEMT)) {
            error_report("inconsistent state in serial device "
                         "(tsr not empty, tsr_retry=0");
            return -1;
        }
    }

    s->last_break_enable = (s->lcr >> 6) & 1;
    /* Initialize fcr via setter to perform essential side-effects */
    serial_write_fcr(s, s->fcr_vmstate);
    s->updateParameters();
    return 0;
}

static bool serial_thr_ipending_needed(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);

    if (s->ier & UART_IER_THRI) {
        bool expected_value = ((s->iir & UART_IIR_ID) == UART_IIR_THRI);
        return s->thr_ipending != expected_value;
    } else {
        /* LSR.THRE will be sampled again when the interrupt is
         * enabled.  thr_ipending is not used in this case, do
         * not migrate it.
         */
        return false;
    }
}

static const VMStateField vmstate_serial_thr_ipending_fields[] = {
    VMSTATE_INT32(thr_ipending, SerialState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_thr_ipending = {
    .name = "serial/thr_ipending",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_thr_ipending_needed,
    .fields = vmstate_serial_thr_ipending_fields,
};

static bool serial_tsr_needed(void *opaque)
{
    SerialState *s = (SerialState *)opaque;
    return s->tsr_retry != 0;
}

static const VMStateField vmstate_serial_tsr_fields[] = {
    VMSTATE_UINT32(tsr_retry, SerialState),
    VMSTATE_UINT8(thr, SerialState),
    VMSTATE_UINT8(tsr, SerialState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_tsr = {
    .name = "serial/tsr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_tsr_needed,
    .fields = vmstate_serial_tsr_fields,
};

static bool serial_recv_fifo_needed(void *opaque)
{
    SerialState *s = (SerialState *)opaque;
    return !fifo8_is_empty(&s->recv_fifo);

}

static const VMStateField vmstate_serial_recv_fifo_fields[] = {
    VMSTATE_STRUCT(recv_fifo, SerialState, 1, vmstate_fifo8, Fifo8),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_recv_fifo = {
    .name = "serial/recv_fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_recv_fifo_needed,
    .fields = vmstate_serial_recv_fifo_fields,
};

static bool serial_xmit_fifo_needed(void *opaque)
{
    SerialState *s = (SerialState *)opaque;
    return !fifo8_is_empty(&s->xmit_fifo);
}

static const VMStateField vmstate_serial_xmit_fifo_fields[] = {
    VMSTATE_STRUCT(xmit_fifo, SerialState, 1, vmstate_fifo8, Fifo8),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_xmit_fifo = {
    .name = "serial/xmit_fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_xmit_fifo_needed,
    .fields = vmstate_serial_xmit_fifo_fields,
};

static bool serial_fifo_timeout_timer_needed(void *opaque)
{
    SerialState *s = (SerialState *)opaque;
    return timer_pending(s->fifo_timeout_timer);
}

static const VMStateField vmstate_serial_fifo_timeout_timer_fields[] = {
    VMSTATE_TIMER_PTR(fifo_timeout_timer, SerialState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_fifo_timeout_timer = {
    .name = "serial/fifo_timeout_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_fifo_timeout_timer_needed,
    .fields = vmstate_serial_fifo_timeout_timer_fields,
};

static bool serial_timeout_ipending_needed(void *opaque)
{
    SerialState *s = (SerialState *)opaque;
    return s->timeout_ipending != 0;
}

static const VMStateField vmstate_serial_timeout_ipending_fields[] = {
    VMSTATE_INT32(timeout_ipending, SerialState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_timeout_ipending = {
    .name = "serial/timeout_ipending",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_timeout_ipending_needed,
    .fields = vmstate_serial_timeout_ipending_fields,
};

static bool serial_poll_needed(void *opaque)
{
    SerialState *s = (SerialState *)opaque;
    return s->poll_msl >= 0;
}

static const VMStateField vmstate_serial_poll_fields[] = {
    VMSTATE_INT32(poll_msl, SerialState),
    VMSTATE_TIMER_PTR(modem_status_poll, SerialState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_serial_poll = {
    .name = "serial/poll",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serial_poll_needed,
    .fields = vmstate_serial_poll_fields,
};

static const VMStateField vmstate_serial_fields[] = {
    VMSTATE_UINT16_V(divider, SerialState, 2),
    VMSTATE_UINT8(rbr, SerialState),
    VMSTATE_UINT8(ier, SerialState),
    VMSTATE_UINT8(iir, SerialState),
    VMSTATE_UINT8(lcr, SerialState),
    VMSTATE_UINT8(mcr, SerialState),
    VMSTATE_UINT8(lsr, SerialState),
    VMSTATE_UINT8(msr, SerialState),
    VMSTATE_UINT8(scr, SerialState),
    VMSTATE_UINT8_V(fcr_vmstate, SerialState, 3),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription * const vmstate_serial_subsections[] = {
    &vmstate_serial_thr_ipending,
    &vmstate_serial_tsr,
    &vmstate_serial_recv_fifo,
    &vmstate_serial_xmit_fifo,
    &vmstate_serial_fifo_timeout_timer,
    &vmstate_serial_timeout_ipending,
    &vmstate_serial_poll,
    NULL
};

const VMStateDescription vmstate_serial = {
    .name = "serial",
    .version_id = 3,
    .minimum_version_id = 2,
    .pre_load = serial_pre_load,
    .post_load = serial_post_load,
    .pre_save = serial_pre_save,
    .fields = vmstate_serial_fields,
    .subsections = vmstate_serial_subsections,
};

void SerialState::reset()
{
    if (watch_tag > 0) {
        g_source_remove(watch_tag);
        watch_tag = 0;
    }

    rbr = 0;
    ier = 0;
    iir = UART_IIR_NO_INT;
    lcr = 0;
    lsr = UART_LSR_TEMT | UART_LSR_THRE;
    msr = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS;
    /* Default to 9600 baud, 1 start bit, 8 data bits, 1 stop bit, no parity. */
    divider = 0x0C;
    mcr = UART_MCR_OUT2;
    scr = 0;
    tsr_retry = 0;
    char_transmit_time = (NANOSECONDS_PER_SECOND / 9600) * 10;
    poll_msl = 0;

    timeout_ipending = 0;
    timer_del(fifo_timeout_timer);
    timer_del(modem_status_poll);

    fifo8_reset(&recv_fifo);
    fifo8_reset(&xmit_fifo);

    last_xmit_ts = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    thr_ipending = 0;
    last_break_enable = 0;
    qemu_irq_lower(irq);

    updateMsl();
    msr &= ~UART_MSR_ANY_DELTA;
}

/* QOM reset callback wrapper */
static void serial_reset_cb(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);
    s->reset();
}

static int serial_be_change(void *opaque)
{
    SerialState *s = static_cast<SerialState *>(opaque);

    qemu_chr_fe_set_handlers(&s->chr, serial_can_receive1, serial_receive1,
                             serial_event, serial_be_change, s, NULL, true);

    s->updateParameters();

    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_BREAK,
                      &s->last_break_enable);

    s->poll_msl = (s->ier & UART_IER_MSI) ? 1 : 0;
    s->updateMsl();

    if (s->poll_msl >= 0 && !(s->mcr & UART_MCR_LOOP)) {
        serial_update_tiocm(s);
    }

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, static_cast<GIOCondition>(G_IO_OUT | G_IO_HUP),
                                             serial_watch_cb, s);
    }

    return 0;
}

void SerialState::realize(Error **errp)
{
    modem_status_poll = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     serial_update_msl_cb, this);

    fifo_timeout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      fifo_timeout_int, this);
    qemu_register_reset(serial_reset_cb, this);

    qemu_chr_fe_set_handlers(&chr, serial_can_receive1, serial_receive1,
                             serial_event, serial_be_change, this, NULL, true);
    fifo8_create(&recv_fifo, UART_FIFO_LENGTH);
    fifo8_create(&xmit_fifo, UART_FIFO_LENGTH);
    reset();
}

/* QOM realize callback wrapper */
static void serial_unrealize(DeviceState *dev)
{
    SerialState *s = reinterpret_cast<SerialState *>(dev);

    qemu_chr_fe_deinit(&s->chr, false);

    timer_free(s->modem_status_poll);

    timer_free(s->fifo_timeout_timer);

    fifo8_destroy(&s->recv_fifo);
    fifo8_destroy(&s->xmit_fifo);

    qemu_unregister_reset(serial_reset_cb, s);
}

const MemoryRegionOps serial_io_ops = {
    .read = serial_ioport_read,
    .write = serial_ioport_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .unaligned = 1,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const Property serial_properties[] = {
    DEFINE_PROP_CHR("chardev", SerialState, chr),
    DEFINE_PROP_UINT32("baudbase", SerialState, baudbase, 115200),
    DEFINE_PROP_BOOL("wakeup", SerialState, wakeup, false),
};

void SerialState::classInit(DeviceClass *dc)
{
    dc->user_creatable = false;
    dc->unrealize = serial_unrealize;
    device_class_set_props(dc, serial_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(SerialState, TYPE_SERIAL, TYPE_DEVICE)
