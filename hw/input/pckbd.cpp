/*
 * QEMU PC keyboard emulation
 *
 * Copyright (c) 2003 Fabrice Bellard
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
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "hw/acpi/acpi_aml_interface.h"
#include "hw/input/ps2.h"
#include "hw/irq.h"
#include "hw/input/i8042.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "system/runstate.h"

#include "trace.h"

/* Keyboard Controller Commands */

/* Read mode bits */
#define KBD_CCMD_READ_MODE         0x20
/* Write mode bits */
#define KBD_CCMD_WRITE_MODE        0x60
/* Get controller version */
#define KBD_CCMD_GET_VERSION       0xA1
/* Disable mouse interface */
#define KBD_CCMD_MOUSE_DISABLE     0xA7
/* Enable mouse interface */
#define KBD_CCMD_MOUSE_ENABLE      0xA8
/* Mouse interface test */
#define KBD_CCMD_TEST_MOUSE        0xA9
/* Controller self test */
#define KBD_CCMD_SELF_TEST         0xAA
/* Keyboard interface test */
#define KBD_CCMD_KBD_TEST          0xAB
/* Keyboard interface disable */
#define KBD_CCMD_KBD_DISABLE       0xAD
/* Keyboard interface enable */
#define KBD_CCMD_KBD_ENABLE        0xAE
/* read input port */
#define KBD_CCMD_READ_INPORT       0xC0
/* read output port */
#define KBD_CCMD_READ_OUTPORT      0xD0
/* write output port */
#define KBD_CCMD_WRITE_OUTPORT     0xD1
#define KBD_CCMD_WRITE_OBUF        0xD2
/* Write to output buffer as if initiated by the auxiliary device */
#define KBD_CCMD_WRITE_AUX_OBUF    0xD3
/* Write the following byte to the mouse */
#define KBD_CCMD_WRITE_MOUSE       0xD4
/* HP vectra only ? */
#define KBD_CCMD_DISABLE_A20       0xDD
/* HP vectra only ? */
#define KBD_CCMD_ENABLE_A20        0xDF
/* Pulse bits 3-0 of the output port P2. */
#define KBD_CCMD_PULSE_BITS_3_0    0xF0
/* Pulse bit 0 of the output port P2 = CPU reset. */
#define KBD_CCMD_RESET             0xFE
/* Pulse no bits of the output port P2. */
#define KBD_CCMD_NO_OP             0xFF

/* Status Register Bits */

/* Keyboard output buffer full */
#define KBD_STAT_OBF           0x01
/* Keyboard input buffer full */
#define KBD_STAT_IBF           0x02
/* Self test successful */
#define KBD_STAT_SELFTEST      0x04
/* Last write was a command write (0=data) */
#define KBD_STAT_CMD           0x08
/* Zero if keyboard locked */
#define KBD_STAT_UNLOCKED      0x10
/* Mouse output buffer full */
#define KBD_STAT_MOUSE_OBF     0x20
/* General receive/xmit timeout */
#define KBD_STAT_GTO           0x40
/* Parity error */
#define KBD_STAT_PERR          0x80

/* Controller Mode Register Bits */

/* Keyboard data generate IRQ1 */
#define KBD_MODE_KBD_INT       0x01
/* Mouse data generate IRQ12 */
#define KBD_MODE_MOUSE_INT     0x02
/* The system flag (?) */
#define KBD_MODE_SYS           0x04
/* The keylock doesn't affect the keyboard if set */
#define KBD_MODE_NO_KEYLOCK    0x08
/* Disable keyboard interface */
#define KBD_MODE_DISABLE_KBD   0x10
/* Disable mouse interface */
#define KBD_MODE_DISABLE_MOUSE 0x20
/* Scan code conversion to PC format */
#define KBD_MODE_KCC           0x40
#define KBD_MODE_RFU           0x80

/* Output Port Bits */
#define KBD_OUT_RESET           0x01    /* 1=normal mode, 0=reset */
#define KBD_OUT_A20             0x02    /* x86 only */
#define KBD_OUT_OBF             0x10    /* Keyboard output buffer full */
#define KBD_OUT_MOUSE_OBF       0x20    /* Mouse output buffer full */

/*
 * OSes typically write 0xdd/0xdf to turn the A20 line off and on.
 * We make the default value of the outport include these four bits,
 * so that the subsection is rarely necessary.
 */
#define KBD_OUT_ONES            0xcc

#define KBD_PENDING_KBD_COMPAT  0x01
#define KBD_PENDING_AUX_COMPAT  0x02
#define KBD_PENDING_CTRL_KBD    0x04
#define KBD_PENDING_CTRL_AUX    0x08
#define KBD_PENDING_KBD         KBD_MODE_DISABLE_KBD    /* 0x10 */
#define KBD_PENDING_AUX         KBD_MODE_DISABLE_MOUSE  /* 0x20 */

#define KBD_MIGR_TIMER_PENDING  0x1

#define KBD_OBSRC_KBD           0x01
#define KBD_OBSRC_MOUSE         0x02
#define KBD_OBSRC_CTRL          0x04


/* KBDState methods */

/*
 * XXX: not generating the irqs if KBD_MODE_DISABLE_KBD is set may be
 * incorrect, but it avoids having to simulate exact delays
 */
void KBDState::updateIrqLines()
{
    int irq_kbd_level, irq_mouse_level;

    irq_kbd_level = 0;
    irq_mouse_level = 0;

    if (status & KBD_STAT_OBF) {
        if (status & KBD_STAT_MOUSE_OBF) {
            if (mode & KBD_MODE_MOUSE_INT) {
                irq_mouse_level = 1;
            }
        } else {
            if ((mode & KBD_MODE_KBD_INT) &&
                !(mode & KBD_MODE_DISABLE_KBD)) {
                irq_kbd_level = 1;
            }
        }
    }
    qemu_set_irq(irqs[I8042_KBD_IRQ], irq_kbd_level);
    qemu_set_irq(irqs[I8042_MOUSE_IRQ], irq_mouse_level);
}

void KBDState::deassertIrq()
{
    status &= ~(KBD_STAT_OBF | KBD_STAT_MOUSE_OBF);
    outport &= ~(KBD_OUT_OBF | KBD_OUT_MOUSE_OBF);
    updateIrqLines();
}

uint8_t KBDState::getPending()
{
    if (extended_state) {
        return pending & (~mode | ~(KBD_PENDING_KBD | KBD_PENDING_AUX));
    } else {
        return pending;
    }
}

/* update irq and KBD_STAT_[MOUSE_]OBF */
void KBDState::updateIrq()
{
    uint8_t pend = getPending();

    status &= ~(KBD_STAT_OBF | KBD_STAT_MOUSE_OBF);
    outport &= ~(KBD_OUT_OBF | KBD_OUT_MOUSE_OBF);
    if (pend) {
        status |= KBD_STAT_OBF;
        outport |= KBD_OUT_OBF;
        if (pend & KBD_PENDING_CTRL_KBD) {
            obsrc = KBD_OBSRC_CTRL;
        } else if (pend & KBD_PENDING_CTRL_AUX) {
            status |= KBD_STAT_MOUSE_OBF;
            outport |= KBD_OUT_MOUSE_OBF;
            obsrc = KBD_OBSRC_CTRL;
        } else if (pend & KBD_PENDING_KBD) {
            obsrc = KBD_OBSRC_KBD;
        } else {
            status |= KBD_STAT_MOUSE_OBF;
            outport |= KBD_OUT_MOUSE_OBF;
            obsrc = KBD_OBSRC_MOUSE;
        }
    }
    updateIrqLines();
}

void KBDState::safeUpdateIrq()
{
    /*
     * with KBD_STAT_OBF set, a call to readData() will eventually call
     * updateIrq()
     */
    if (status & KBD_STAT_OBF) {
        return;
    }
    /* the throttle timer is pending and will call updateIrq() */
    if (throttle_timer && timer_pending(throttle_timer)) {
        return;
    }
    if (getPending()) {
        updateIrq();
    }
}

void KBDState::updateKbdIrq(void *opaque, int level)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (level) {
        s->pending |= KBD_PENDING_KBD;
    } else {
        s->pending &= ~KBD_PENDING_KBD;
    }
    s->safeUpdateIrq();
}

void KBDState::updateAuxIrq(void *opaque, int level)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (level) {
        s->pending |= KBD_PENDING_AUX;
    } else {
        s->pending &= ~KBD_PENDING_AUX;
    }
    s->safeUpdateIrq();
}

void KBDState::throttleTimeout(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (s->getPending()) {
        s->updateIrq();
    }
}

uint64_t KBDState::readStatus(void *opaque, hwaddr addr,
                               unsigned size)
{
    KBDState *s = static_cast<KBDState *>(opaque);
    int val;
    val = s->status;
    trace_pckbd_kbd_read_status(val);
    return val;
}

void KBDState::queueCmd(int b, int aux)
{
    if (extended_state) {
        cbdata = b;
        pending &= ~KBD_PENDING_CTRL_KBD & ~KBD_PENDING_CTRL_AUX;
        pending |= aux ? KBD_PENDING_CTRL_AUX : KBD_PENDING_CTRL_KBD;
        safeUpdateIrq();
    } else {
        ps2_queue(aux ? PS2_DEVICE(&ps2mouse) : PS2_DEVICE(&ps2kbd), b);
    }
}

uint8_t KBDState::dequeue()
{
    uint8_t b = cbdata;

    pending &= ~KBD_PENDING_CTRL_KBD & ~KBD_PENDING_CTRL_AUX;
    if (getPending()) {
        updateIrq();
    }
    return b;
}

void KBDState::outportWrite(uint32_t val)
{
    trace_pckbd_outport_write(val);
    outport = val;
    qemu_set_irq(a20_out, (val >> 1) & 1);
    if (!(val & 1)) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

void KBDState::writeCommand(void *opaque, hwaddr addr,
                             uint64_t val, unsigned size)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    trace_pckbd_kbd_write_command(val);

    /*
     * Bits 3-0 of the output port P2 of the keyboard controller may be pulsed
     * low for approximately 6 micro seconds. Bits 3-0 of the KBD_CCMD_PULSE
     * command specify the output port bits to be pulsed.
     * 0: Bit should be pulsed. 1: Bit should not be modified.
     * The only useful version of this command is pulsing bit 0,
     * which does a CPU reset.
     */
    if ((val & KBD_CCMD_PULSE_BITS_3_0) == KBD_CCMD_PULSE_BITS_3_0) {
        if (!(val & 1)) {
            val = KBD_CCMD_RESET;
        } else {
            val = KBD_CCMD_NO_OP;
        }
    }

    switch (val) {
    case KBD_CCMD_READ_MODE:
        s->queueCmd(s->mode, 0);
        break;
    case KBD_CCMD_WRITE_MODE:
    case KBD_CCMD_WRITE_OBUF:
    case KBD_CCMD_WRITE_AUX_OBUF:
    case KBD_CCMD_WRITE_MOUSE:
    case KBD_CCMD_WRITE_OUTPORT:
        s->write_cmd = val;
        break;
    case KBD_CCMD_MOUSE_DISABLE:
        s->mode |= KBD_MODE_DISABLE_MOUSE;
        break;
    case KBD_CCMD_MOUSE_ENABLE:
        s->mode &= ~KBD_MODE_DISABLE_MOUSE;
        s->safeUpdateIrq();
        break;
    case KBD_CCMD_TEST_MOUSE:
        s->queueCmd(0x00, 0);
        break;
    case KBD_CCMD_SELF_TEST:
        s->status |= KBD_STAT_SELFTEST;
        s->queueCmd(0x55, 0);
        break;
    case KBD_CCMD_KBD_TEST:
        s->queueCmd(0x00, 0);
        break;
    case KBD_CCMD_KBD_DISABLE:
        s->mode |= KBD_MODE_DISABLE_KBD;
        break;
    case KBD_CCMD_KBD_ENABLE:
        s->mode &= ~KBD_MODE_DISABLE_KBD;
        s->safeUpdateIrq();
        break;
    case KBD_CCMD_READ_INPORT:
        s->queueCmd(0x80, 0);
        break;
    case KBD_CCMD_READ_OUTPORT:
        s->queueCmd(s->outport, 0);
        break;
    case KBD_CCMD_ENABLE_A20:
        qemu_irq_raise(s->a20_out);
        s->outport |= KBD_OUT_A20;
        break;
    case KBD_CCMD_DISABLE_A20:
        qemu_irq_lower(s->a20_out);
        s->outport &= ~KBD_OUT_A20;
        break;
    case KBD_CCMD_RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    case KBD_CCMD_NO_OP:
        /* ignore that */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "unsupported keyboard cmd=0x%02" PRIx64 "\n", val);
        break;
    }
}

uint64_t KBDState::readData(void *opaque, hwaddr addr,
                             unsigned size)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (s->status & KBD_STAT_OBF) {
        s->deassertIrq();
        if (s->obsrc & KBD_OBSRC_KBD) {
            if (s->throttle_timer) {
                timer_mod(s->throttle_timer,
                          qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + 1000);
            }
            s->obdata = ps2_read_data(PS2_DEVICE(&s->ps2kbd));
        } else if (s->obsrc & KBD_OBSRC_MOUSE) {
            s->obdata = ps2_read_data(PS2_DEVICE(&s->ps2mouse));
        } else if (s->obsrc & KBD_OBSRC_CTRL) {
            s->obdata = s->dequeue();
        }
    }

    trace_pckbd_kbd_read_data(s->obdata);
    return s->obdata;
}

void KBDState::writeData(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    trace_pckbd_kbd_write_data(val);

    switch (s->write_cmd) {
    case 0:
        ps2_write_keyboard(&s->ps2kbd, val);
        /* sending data to the keyboard reenables PS/2 communication */
        s->mode &= ~KBD_MODE_DISABLE_KBD;
        s->safeUpdateIrq();
        break;
    case KBD_CCMD_WRITE_MODE:
        s->mode = val;
        ps2_keyboard_set_translation(&s->ps2kbd,
                                     (s->mode & KBD_MODE_KCC) != 0);
        /*
         * a write to the mode byte interrupt enable flags directly updates
         * the irq lines
         */
        s->updateIrqLines();
        /*
         * a write to the mode byte disable interface flags may raise
         * an irq if there is pending data in the PS/2 queues.
         */
        s->safeUpdateIrq();
        break;
    case KBD_CCMD_WRITE_OBUF:
        s->queueCmd(val, 0);
        break;
    case KBD_CCMD_WRITE_AUX_OBUF:
        s->queueCmd(val, 1);
        break;
    case KBD_CCMD_WRITE_OUTPORT:
        s->outportWrite(val);
        break;
    case KBD_CCMD_WRITE_MOUSE:
        ps2_write_mouse(&s->ps2mouse, val);
        /* sending data to the mouse reenables PS/2 communication */
        s->mode &= ~KBD_MODE_DISABLE_MOUSE;
        s->safeUpdateIrq();
        break;
    default:
        break;
    }
    s->write_cmd = 0;
}

void KBDState::reset()
{
    mode = KBD_MODE_KBD_INT | KBD_MODE_MOUSE_INT;
    status = KBD_STAT_CMD | KBD_STAT_UNLOCKED;
    outport = KBD_OUT_RESET | KBD_OUT_A20 | KBD_OUT_ONES;
    pending = 0;
    deassertIrq();
    if (throttle_timer) {
        timer_del(throttle_timer);
    }
}

uint8_t KBDState::outportDefault()
{
    return KBD_OUT_RESET | KBD_OUT_A20 | KBD_OUT_ONES
           | (status & KBD_STAT_OBF ? KBD_OUT_OBF : 0)
           | (status & KBD_STAT_MOUSE_OBF ? KBD_OUT_MOUSE_OBF : 0);
}

/* VMState callbacks - these remain as free functions taking void* opaque */

static int kbd_outport_post_load(void *opaque, int version_id)
{
    KBDState *s = static_cast<KBDState *>(opaque);
    s->outport_present = true;
    return 0;
}

static bool kbd_outport_needed(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);
    return s->outport != s->outportDefault();
}

static const VMStateDescription vmstate_kbd_outport = {
    .name = "pckbd_outport",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = kbd_outport_post_load,
    .needed = kbd_outport_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(outport, KBDState),
        VMSTATE_END_OF_LIST()
    }
};

static int kbd_extended_state_pre_save(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    s->migration_flags = 0;
    if (s->throttle_timer && timer_pending(s->throttle_timer)) {
        s->migration_flags |= KBD_MIGR_TIMER_PENDING;
    }

    return 0;
}

static int kbd_extended_state_post_load(void *opaque, int version_id)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (s->migration_flags & KBD_MIGR_TIMER_PENDING) {
        KBDState::throttleTimeout(s);
    }
    s->extended_state_loaded = true;

    return 0;
}

static bool kbd_extended_state_needed(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    return s->extended_state;
}

static const VMStateDescription vmstate_kbd_extended_state = {
    .name = "pckbd/extended_state",
    .post_load = kbd_extended_state_post_load,
    .pre_save = kbd_extended_state_pre_save,
    .needed = kbd_extended_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(migration_flags, KBDState),
        VMSTATE_UINT32(obsrc, KBDState),
        VMSTATE_UINT8(obdata, KBDState),
        VMSTATE_UINT8(cbdata, KBDState),
        VMSTATE_END_OF_LIST()
    }
};

static int kbd_pre_save(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (s->extended_state) {
        s->pending_tmp = s->pending;
    } else {
        s->pending_tmp = 0;
        if (s->pending & KBD_PENDING_KBD) {
            s->pending_tmp |= KBD_PENDING_KBD_COMPAT;
        }
        if (s->pending & KBD_PENDING_AUX) {
            s->pending_tmp |= KBD_PENDING_AUX_COMPAT;
        }
    }
    return 0;
}

static int kbd_pre_load(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    s->outport_present = false;
    s->extended_state_loaded = false;
    return 0;
}

static int kbd_post_load(void *opaque, int version_id)
{
    KBDState *s = static_cast<KBDState *>(opaque);
    if (!s->outport_present) {
        s->outport = s->outportDefault();
    }
    s->pending = s->pending_tmp;
    if (!s->extended_state_loaded) {
        s->obsrc = s->status & KBD_STAT_OBF ?
            (s->status & KBD_STAT_MOUSE_OBF ? KBD_OBSRC_MOUSE : KBD_OBSRC_KBD) :
            0;
        if (s->pending & KBD_PENDING_KBD_COMPAT) {
            s->pending |= KBD_PENDING_KBD;
        }
        if (s->pending & KBD_PENDING_AUX_COMPAT) {
            s->pending |= KBD_PENDING_AUX;
        }
    }
    /* clear all unused flags */
    s->pending &= KBD_PENDING_CTRL_KBD | KBD_PENDING_CTRL_AUX |
                  KBD_PENDING_KBD | KBD_PENDING_AUX;
    return 0;
}

static const VMStateDescription vmstate_kbd = {
    .name = "pckbd",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_load = kbd_pre_load,
    .post_load = kbd_post_load,
    .pre_save = kbd_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(write_cmd, KBDState),
        VMSTATE_UINT8(status, KBDState),
        VMSTATE_UINT8(mode, KBDState),
        VMSTATE_UINT8(pending_tmp, KBDState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_kbd_outport,
        &vmstate_kbd_extended_state,
        NULL
    }
};

/* Memory mapped interface */
static uint64_t kbd_mm_readfn(void *opaque, hwaddr addr, unsigned size)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (addr & s->mask) {
        return KBDState::readStatus(s, 0, 1) & 0xff;
    } else {
        return KBDState::readData(s, 0, 1) & 0xff;
    }
}

static void kbd_mm_writefn(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    if (addr & s->mask) {
        KBDState::writeCommand(s, 0, value & 0xff, 1);
    } else {
        KBDState::writeData(s, 0, value & 0xff, 1);
    }
}


static const MemoryRegionOps i8042_mmio_ops = {
    .read = kbd_mm_readfn,
    .write = kbd_mm_writefn,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
};

static void i8042_mmio_set_kbd_irq(void *opaque, int n, int level)
{
    MMIOKBDState *s = reinterpret_cast<MMIOKBDState *>(opaque);
    KBDState *ks = &s->kbd;

    KBDState::updateKbdIrq(ks, level);
}

static void i8042_mmio_set_mouse_irq(void *opaque, int n, int level)
{
    MMIOKBDState *s = reinterpret_cast<MMIOKBDState *>(opaque);
    KBDState *ks = &s->kbd;

    KBDState::updateAuxIrq(ks, level);
}

static void i8042_mmio_reset_impl(MMIOKBDState *s)
{
    KBDState *ks = &s->kbd;
    ks->reset();
}

static void i8042_mmio_reset(DeviceState *dev)
{
    MMIOKBDState *s = reinterpret_cast<MMIOKBDState *>(dev);
    i8042_mmio_reset_impl(s);
}

static void i8042_mmio_realize_impl(MMIOKBDState *s, DeviceState *dev, Error **errp)
{
    KBDState *ks = &s->kbd;

    memory_region_init_io(&s->region, reinterpret_cast<Object *>(dev), &i8042_mmio_ops, ks,
                          "i8042", s->size);

    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(dev), &s->region);

    if (!sysbus_realize(reinterpret_cast<SysBusDevice *>(&ks->ps2kbd), errp)) {
        return;
    }

    if (!sysbus_realize(reinterpret_cast<SysBusDevice *>(&ks->ps2mouse), errp)) {
        return;
    }

    qdev_connect_gpio_out(reinterpret_cast<DeviceState *>(&ks->ps2kbd), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-kbd-input-irq",
                                                 0));

    qdev_connect_gpio_out(reinterpret_cast<DeviceState *>(&ks->ps2mouse), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-mouse-input-irq",
                                                 0));
}

void MMIOKBDState::init()
{
    KBDState *ks = &kbd;

    ks->extended_state = true;

    object_initialize_child(reinterpret_cast<Object *>(this), "ps2kbd", &ks->ps2kbd, TYPE_PS2_KBD_DEVICE);
    object_initialize_child(reinterpret_cast<Object *>(this), "ps2mouse", &ks->ps2mouse,
                            TYPE_PS2_MOUSE_DEVICE);

    qdev_init_gpio_out(reinterpret_cast<DeviceState *>(this), ks->irqs, 2);
    qdev_init_gpio_in_named(reinterpret_cast<DeviceState *>(this), i8042_mmio_set_kbd_irq,
                            "ps2-kbd-input-irq", 1);
    qdev_init_gpio_in_named(reinterpret_cast<DeviceState *>(this), i8042_mmio_set_mouse_irq,
                            "ps2-mouse-input-irq", 1);
}


static const Property i8042_mmio_properties[] = {
    DEFINE_PROP_UINT64("mask", MMIOKBDState, kbd.mask, UINT64_MAX),
    DEFINE_PROP_UINT32("size", MMIOKBDState, size, -1),
};

static const VMStateDescription vmstate_kbd_mmio = {
    .name = "pckbd-mmio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(kbd, MMIOKBDState, 0, vmstate_kbd, KBDState),
        VMSTATE_END_OF_LIST()
    }
};

static void i8042_mmio_realize(DeviceState *dev, Error **errp)
{
    MMIOKBDState *s = reinterpret_cast<MMIOKBDState *>(dev);
    i8042_mmio_realize_impl(s, dev, errp);
}

void MMIOKBDState::classInit(DeviceClass *dc)
{
    dc->realize = i8042_mmio_realize;
    device_class_set_legacy_reset(dc, i8042_mmio_reset);
    dc->vmsd = &vmstate_kbd_mmio;
    device_class_set_props(dc, i8042_mmio_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

void i8042_isa_mouse_fake_event(ISAKBDState *isa)
{
    KBDState *s = &isa->kbd;

    ps2_mouse_fake_event(&s->ps2mouse);
}

static const VMStateDescription vmstate_kbd_isa = {
    .name = "pckbd",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(kbd, ISAKBDState, 0, vmstate_kbd, KBDState),
        VMSTATE_END_OF_LIST()
    }
};

static const MemoryRegionOps i8042_data_ops = {
    .read = KBDState::readData,
    .write = KBDState::writeData,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps i8042_cmd_ops = {
    .read = KBDState::readStatus,
    .write = KBDState::writeCommand,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void i8042_set_kbd_irq(void *opaque, int n, int level)
{
    ISAKBDState *s = reinterpret_cast<ISAKBDState *>(opaque);
    KBDState *ks = &s->kbd;

    KBDState::updateKbdIrq(ks, level);
}

static void i8042_set_mouse_irq(void *opaque, int n, int level)
{
    ISAKBDState *s = reinterpret_cast<ISAKBDState *>(opaque);
    KBDState *ks = &s->kbd;

    KBDState::updateAuxIrq(ks, level);
}


static void i8042_reset_impl(ISAKBDState *s)
{
    KBDState *ks = &s->kbd;
    ks->reset();
}

static void i8042_reset(DeviceState *dev)
{
    ISAKBDState *s = reinterpret_cast<ISAKBDState *>(dev);
    i8042_reset_impl(s);
}

void ISAKBDState::init()
{
    KBDState *s = &kbd;

    memory_region_init_io(io + 0, reinterpret_cast<Object *>(this), &i8042_data_ops, s,
                          "i8042-data", 1);
    memory_region_init_io(io + 1, reinterpret_cast<Object *>(this), &i8042_cmd_ops, s,
                          "i8042-cmd", 1);

    object_initialize_child(reinterpret_cast<Object *>(this), "ps2kbd", &s->ps2kbd, TYPE_PS2_KBD_DEVICE);
    object_initialize_child(reinterpret_cast<Object *>(this), "ps2mouse", &s->ps2mouse,
                            TYPE_PS2_MOUSE_DEVICE);

    qdev_init_gpio_out_named(reinterpret_cast<DeviceState *>(this), &s->a20_out, I8042_A20_LINE, 1);

    qdev_init_gpio_out(reinterpret_cast<DeviceState *>(this), s->irqs, 2);
    qdev_init_gpio_in_named(reinterpret_cast<DeviceState *>(this), i8042_set_kbd_irq,
                            "ps2-kbd-input-irq", 1);
    qdev_init_gpio_in_named(reinterpret_cast<DeviceState *>(this), i8042_set_mouse_irq,
                            "ps2-mouse-input-irq", 1);
}


static void i8042_realizefn_impl(ISAKBDState *isa_s, DeviceState *dev, Error **errp)
{
    ISADevice *isadev = reinterpret_cast<ISADevice *>(dev);
    KBDState *s = &isa_s->kbd;

    if (isa_s->kbd_irq >= ISA_NUM_IRQS) {
        error_setg(errp, "Maximum value for \"kbd-irq\" is: %u",
                   ISA_NUM_IRQS - 1);
        return;
    }

    if (isa_s->mouse_irq >= ISA_NUM_IRQS) {
        error_setg(errp, "Maximum value for \"mouse-irq\" is: %u",
                   ISA_NUM_IRQS - 1);
        return;
    }

    isa_connect_gpio_out(isadev, I8042_KBD_IRQ, isa_s->kbd_irq);
    isa_connect_gpio_out(isadev, I8042_MOUSE_IRQ, isa_s->mouse_irq);

    isa_register_ioport(isadev, isa_s->io + 0, 0x60);
    isa_register_ioport(isadev, isa_s->io + 1, 0x64);

    if (!sysbus_realize(reinterpret_cast<SysBusDevice *>(&s->ps2kbd), errp)) {
        return;
    }

    qdev_connect_gpio_out(reinterpret_cast<DeviceState *>(&s->ps2kbd), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-kbd-input-irq",
                                                 0));

    if (!sysbus_realize(reinterpret_cast<SysBusDevice *>(&s->ps2mouse), errp)) {
        return;
    }

    qdev_connect_gpio_out(reinterpret_cast<DeviceState *>(&s->ps2mouse), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-mouse-input-irq",
                                                 0));

    if (isa_s->kbd_throttle && !isa_s->kbd.extended_state) {
        warn_report(TYPE_I8042 ": can't enable kbd-throttle without"
                    " extended-state, disabling kbd-throttle");
    } else if (isa_s->kbd_throttle) {
        s->throttle_timer = timer_new_us(QEMU_CLOCK_VIRTUAL,
                                         KBDState::throttleTimeout, s);
    }
}

static void i8042_build_aml(AcpiDevAmlIf *adev, Aml *scope)
{
    ISAKBDState *isa_s = reinterpret_cast<ISAKBDState *>(adev);
    Aml *kbd;
    Aml *mou;
    Aml *crs;

    crs = aml_resource_template();
    aml_append(crs, aml_io(AML_DECODE16, 0x0060, 0x0060, 0x01, 0x01));
    aml_append(crs, aml_io(AML_DECODE16, 0x0064, 0x0064, 0x01, 0x01));
    aml_append(crs, aml_irq_no_flags(isa_s->kbd_irq));

    kbd = aml_device("KBD");
    aml_append(kbd, aml_name_decl("_HID", aml_eisaid("PNP0303")));
    aml_append(kbd, aml_name_decl("_STA", aml_int(0xf)));
    aml_append(kbd, aml_name_decl("_CRS", crs));

    crs = aml_resource_template();
    aml_append(crs, aml_irq_no_flags(isa_s->mouse_irq));

    mou = aml_device("MOU");
    aml_append(mou, aml_name_decl("_HID", aml_eisaid("PNP0F13")));
    aml_append(mou, aml_name_decl("_STA", aml_int(0xf)));
    aml_append(mou, aml_name_decl("_CRS", crs));

    aml_append(scope, kbd);
    aml_append(scope, mou);
}

static const Property i8042_properties[] = {
    DEFINE_PROP_BOOL("extended-state", ISAKBDState, kbd.extended_state, true),
    DEFINE_PROP_BOOL("kbd-throttle", ISAKBDState, kbd_throttle, false),
    DEFINE_PROP_UINT8("kbd-irq", ISAKBDState, kbd_irq, 1),
    DEFINE_PROP_UINT8("mouse-irq", ISAKBDState, mouse_irq, 12),
};

static void i8042_realizefn(DeviceState *dev, Error **errp)
{
    ISAKBDState *isa_s = reinterpret_cast<ISAKBDState *>(dev);
    i8042_realizefn_impl(isa_s, dev, errp);
}

void ISAKBDState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    AcpiDevAmlIfClass *adevc = reinterpret_cast<AcpiDevAmlIfClass *>(klass);

    device_class_set_props(dc, i8042_properties);
    device_class_set_legacy_reset(dc, i8042_reset);
    dc->realize = i8042_realizefn;
    dc->vmsd = &vmstate_kbd_isa;
    adevc->build_dev_aml = i8042_build_aml;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const InterfaceInfo i8042_interfaces[] = {
    { TYPE_ACPI_DEV_AML_IF },
    { },
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(ISAKBDState, TYPE_I8042, TYPE_ISA_DEVICE,
                             i8042_interfaces)
REGISTER_QEMU_DEVICE(MMIOKBDState, TYPE_I8042_MMIO, TYPE_SYS_BUS_DEVICE)
