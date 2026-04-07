/*
 * Virtual hardware watchdog.
 *
 * Copyright (C) 2009 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * By Richard W.M. Jones (rjones@redhat.com).
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/watchdog.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qom/object.h"

/*#define I6300ESB_DEBUG 1*/

#ifdef I6300ESB_DEBUG
#define i6300esb_debug(fs,...) \
    fprintf(stderr,"i6300esb: %s: "fs,__func__,##__VA_ARGS__)
#else
#define i6300esb_debug(fs,...)
#endif

/* PCI configuration registers */
#define ESB_CONFIG_REG  0x60            /* Config register                   */
#define ESB_LOCK_REG    0x68            /* WDT lock register                 */

/* Memory mapped registers (offset from base address) */
#define ESB_TIMER1_REG  0x00            /* Timer1 value after each reset     */
#define ESB_TIMER2_REG  0x04            /* Timer2 value after each reset     */
#define ESB_GINTSR_REG  0x08            /* General Interrupt Status Register */
#define ESB_RELOAD_REG  0x0c            /* Reload register                   */

/* Lock register bits */
#define ESB_WDT_FUNC    (0x01 << 2)   /* Watchdog functionality            */
#define ESB_WDT_ENABLE  (0x01 << 1)   /* Enable WDT                        */
#define ESB_WDT_LOCK    (0x01 << 0)   /* Lock (nowayout)                   */

/* Config register bits */
#define ESB_WDT_REBOOT  (0x01 << 5)   /* Enable reboot on timeout          */
#define ESB_WDT_FREQ    (0x01 << 2)   /* Decrement frequency               */
#define ESB_WDT_INTTYPE (0x03 << 0)   /* Interrupt type on timer1 timeout  */

/* Reload register bits */
#define ESB_WDT_RELOAD  (0x01 << 8)    /* prevent timeout                   */

/* Magic constants */
#define ESB_UNLOCK1     0x80            /* Step 1 to unlock reset registers  */
#define ESB_UNLOCK2     0x86            /* Step 2 to unlock reset registers  */

/* Device state. */
struct I6300State {
    PCIDevice dev;
    MemoryRegion io_mem;

    int reboot_enabled;         /* "Reboot" on timer expiry.  The real action
                                 * performed depends on the -watchdog-action
                                 * param passed on QEMU command line.
                                 */
    int clock_scale;            /* Clock scale. */
#define CLOCK_SCALE_1KHZ 0
#define CLOCK_SCALE_1MHZ 1

    int int_type;               /* Interrupt type generated. */
#define INT_TYPE_IRQ 0          /* APIC 1, INT 10 */
#define INT_TYPE_SMI 2
#define INT_TYPE_DISABLED 3

    int free_run;               /* If true, reload timer on expiry. */
    int locked;                 /* If true, enabled field cannot be changed. */
    int enabled;                /* If true, watchdog is enabled. */

    QEMUTimer *timer;           /* The actual watchdog timer. */

    uint32_t timer1_preload;    /* Values preloaded into timer1, timer2. */
    uint32_t timer2_preload;
    int stage;                  /* Stage (1 or 2). */

    int unlock_state;           /* Guest writes 0x80, 0x86 to unlock the
                                 * registers, and we transition through
                                 * states 0 -> 1 -> 2 when this happens.
                                 */

    int previous_reboot_flag;   /* If the watchdog caused the previous
                                 * reboot, this flag will be set.
                                 */

    /* Instance methods */
    void restartTimer(int stage);
    void disableTimer();
    void reset(DeviceState *dev);
    void realize(PCIDevice *dev, Error **errp);
    void exit(PCIDevice *dev);
    void configWrite(PCIDevice *dev, uint32_t addr, uint32_t data, int len);
    uint32_t configRead(PCIDevice *dev, uint32_t addr, int len);
    uint32_t doMemReadb(hwaddr addr);
    uint32_t doMemReadw(hwaddr addr);
    uint32_t doMemReadl(hwaddr addr);
    void doMemWriteb(hwaddr addr, uint32_t val);
    void doMemWritew(hwaddr addr, uint32_t val);
    void doMemWritel(hwaddr addr, uint32_t val);

    /* Static timer callback */
    static void timerExpired(void *vp);

    /* Static MMIO callbacks */
    static uint64_t memReadfn(void *opaque, hwaddr addr, unsigned size);
    static void memWritefn(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size);

    /* Class init */
    static void classInit(ObjectClass *klass, const void *data);
};


#define TYPE_WATCHDOG_I6300ESB_DEVICE "i6300esb"
OBJECT_DECLARE_SIMPLE_TYPE(I6300State, WATCHDOG_I6300ESB_DEVICE)

/* This function is called when the watchdog has either been enabled
 * (hence it starts counting down) or has been keep-alived.
 */
void I6300State::restartTimer(int stage)
{
    int64_t timeout;

    if (!enabled)
        return;

    this->stage = stage;

    if (this->stage <= 1)
        timeout = timer1_preload;
    else
        timeout = timer2_preload;

    if (clock_scale == CLOCK_SCALE_1KHZ)
        timeout <<= 15;
    else
        timeout <<= 5;

    /* Get the timeout in nanoseconds. */

    timeout = timeout * 30; /* on a PCI bus, 1 tick is 30 ns*/

    i6300esb_debug("stage %d, timeout %" PRIi64 "\n", this->stage, timeout);

    timer_mod(timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + timeout);
}

/* This is called when the guest disables the watchdog. */
void I6300State::disableTimer()
{
    i6300esb_debug("timer disabled\n");

    timer_del(timer);
}

void I6300State::reset(DeviceState *dev)
{
    PCIDevice *pdev = reinterpret_cast<PCIDevice *>(dev);
    I6300State *d = reinterpret_cast<I6300State *>(pdev);

    i6300esb_debug("I6300State = %p\n", d);

    d->disableTimer();

    /* NB: Don't change d->previous_reboot_flag in this function. */

    d->reboot_enabled = 1;
    d->clock_scale = CLOCK_SCALE_1KHZ;
    d->int_type = INT_TYPE_IRQ;
    d->free_run = 0;
    d->locked = 0;
    d->enabled = 0;
    d->timer1_preload = 0xfffff;
    d->timer2_preload = 0xfffff;
    d->stage = 1;
    d->unlock_state = 0;
}

static void i6300esb_reset(DeviceState *dev)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    d->reset(dev);
}

/* This function is called when the watchdog expires.  Note that
 * the hardware has two timers, and so expiry happens in two stages.
 * If d->stage == 1 then we perform the first stage action (usually,
 * sending an interrupt) and then restart the timer again for the
 * second stage.  If the second stage expires then the watchdog
 * really has run out.
 */
void I6300State::timerExpired(void *vp)
{
    I6300State *d = static_cast<I6300State *>(vp);

    i6300esb_debug("stage %d\n", d->stage);

    if (d->stage == 1) {
        /* What to do at the end of stage 1? */
        switch (d->int_type) {
        case INT_TYPE_IRQ:
            fprintf(stderr, "i6300esb_timer_expired: I would send APIC 1 INT 10 here if I knew how (XXX)\n");
            break;
        case INT_TYPE_SMI:
            fprintf(stderr, "i6300esb_timer_expired: I would send SMI here if I knew how (XXX)\n");
            break;
        }

        /* Start the second stage. */
        d->restartTimer(2);
    } else {
        /* Second stage expired, reboot for real. */
        if (d->reboot_enabled) {
            d->previous_reboot_flag = 1;
            watchdog_perform_action(); /* This reboots, exits, etc */
            i6300esb_reset(reinterpret_cast<DeviceState *>(d));
        }

        /* In "free running mode" we start stage 1 again. */
        if (d->free_run)
            d->restartTimer(1);
    }
}

void I6300State::configWrite(PCIDevice *dev, uint32_t addr,
                              uint32_t data, int len)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    int old;

    i6300esb_debug("addr = %x, data = %x, len = %d\n", addr, data, len);

    if (addr == ESB_CONFIG_REG && len == 2) {
        d->reboot_enabled = (data & ESB_WDT_REBOOT) == 0;
        d->clock_scale =
            (data & ESB_WDT_FREQ) != 0 ? CLOCK_SCALE_1MHZ : CLOCK_SCALE_1KHZ;
        d->int_type = (data & ESB_WDT_INTTYPE);
    } else if (addr == ESB_LOCK_REG && len == 1) {
        if (!d->locked) {
            d->locked = (data & ESB_WDT_LOCK) != 0;
            d->free_run = (data & ESB_WDT_FUNC) != 0;
            old = d->enabled;
            d->enabled = (data & ESB_WDT_ENABLE) != 0;
            if (!old && d->enabled) /* Enabled transitioned from 0 -> 1 */
                d->restartTimer(1);
            else if (!d->enabled)
                d->disableTimer();
        }
    } else {
        pci_default_write_config(dev, addr, data, len);
    }
}

static void i6300esb_config_write(PCIDevice *dev, uint32_t addr,
                                  uint32_t data, int len)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    d->configWrite(dev, addr, data, len);
}

uint32_t I6300State::configRead(PCIDevice *dev, uint32_t addr, int len)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    uint32_t data;

    i6300esb_debug ("addr = %x, len = %d\n", addr, len);

    if (addr == ESB_CONFIG_REG && len == 2) {
        data =
            (d->reboot_enabled ? 0 : ESB_WDT_REBOOT) |
            (d->clock_scale == CLOCK_SCALE_1MHZ ? ESB_WDT_FREQ : 0) |
            d->int_type;
        return data;
    } else if (addr == ESB_LOCK_REG && len == 1) {
        data =
            (d->free_run ? ESB_WDT_FUNC : 0) |
            (d->locked ? ESB_WDT_LOCK : 0) |
            (d->enabled ? ESB_WDT_ENABLE : 0);
        return data;
    } else {
        return pci_default_read_config(dev, addr, len);
    }
}

static uint32_t i6300esb_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    return d->configRead(dev, addr, len);
}

uint32_t I6300State::doMemReadb(hwaddr addr)
{
    i6300esb_debug ("addr = %x\n", (int) addr);

    return 0;
}

uint32_t I6300State::doMemReadw(hwaddr addr)
{
    uint32_t data = 0;

    i6300esb_debug("addr = %x\n", (int) addr);

    if (addr == 0xc) {
        /* The previous reboot flag is really bit 9, but there is
         * a bug in the Linux driver where it thinks it's bit 12.
         * Set both.
         */
        data = previous_reboot_flag ? 0x1200 : 0;
    }

    return data;
}

uint32_t I6300State::doMemReadl(hwaddr addr)
{
    i6300esb_debug("addr = %x\n", (int) addr);

    return 0;
}

void I6300State::doMemWriteb(hwaddr addr, uint32_t val)
{
    i6300esb_debug("addr = %x, val = %x\n", (int) addr, val);

    if (addr == 0xc && val == 0x80)
        unlock_state = 1;
    else if (addr == 0xc && val == 0x86 && unlock_state == 1)
        unlock_state = 2;
}

void I6300State::doMemWritew(hwaddr addr, uint32_t val)
{
    i6300esb_debug("addr = %x, val = %x\n", (int) addr, val);

    if (addr == 0xc && val == 0x80)
        unlock_state = 1;
    else if (addr == 0xc && val == 0x86 && unlock_state == 1)
        unlock_state = 2;
    else {
        if (unlock_state == 2) {
            if (addr == 0xc) {
                if ((val & 0x100) != 0)
                    /* This is the "ping" from the userspace watchdog in
                     * the guest ...
                     */
                    restartTimer(1);

                /* Setting bit 9 resets the previous reboot flag.
                 * There's a bug in the Linux driver where it sets
                 * bit 12 instead.
                 */
                if ((val & 0x200) != 0 || (val & 0x1000) != 0) {
                    previous_reboot_flag = 0;
                }
            }

            unlock_state = 0;
        }
    }
}

void I6300State::doMemWritel(hwaddr addr, uint32_t val)
{
    i6300esb_debug ("addr = %x, val = %x\n", (int) addr, val);

    if (addr == 0xc && val == 0x80)
        unlock_state = 1;
    else if (addr == 0xc && val == 0x86 && unlock_state == 1)
        unlock_state = 2;
    else {
        if (unlock_state == 2) {
            if (addr == 0)
                timer1_preload = val & 0xfffff;
            else if (addr == 4)
                timer2_preload = val & 0xfffff;

            unlock_state = 0;
        }
    }
}

uint64_t I6300State::memReadfn(void *opaque, hwaddr addr, unsigned size)
{
    I6300State *s = static_cast<I6300State *>(opaque);

    switch (size) {
    case 1:
        return s->doMemReadb(addr);
    case 2:
        return s->doMemReadw(addr);
    case 4:
        return s->doMemReadl(addr);
    default:
        g_assert_not_reached();
    }
}

void I6300State::memWritefn(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    I6300State *s = static_cast<I6300State *>(opaque);

    switch (size) {
    case 1:
        s->doMemWriteb(addr, value);
        break;
    case 2:
        s->doMemWritew(addr, value);
        break;
    case 4:
        s->doMemWritel(addr, value);
        break;
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps i6300esb_ops = {
    .read = I6300State::memReadfn,
    .write = I6300State::memWritefn,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const VMStateField vmstate_i6300esb_fields[] = {
    VMSTATE_PCI_DEVICE(dev, I6300State),
    VMSTATE_INT32(reboot_enabled, I6300State),
    VMSTATE_INT32(clock_scale, I6300State),
    VMSTATE_INT32(int_type, I6300State),
    VMSTATE_INT32(free_run, I6300State),
    VMSTATE_INT32(locked, I6300State),
    VMSTATE_INT32(enabled, I6300State),
    VMSTATE_TIMER_PTR(timer, I6300State),
    VMSTATE_UINT32(timer1_preload, I6300State),
    VMSTATE_UINT32(timer2_preload, I6300State),
    VMSTATE_INT32(stage, I6300State),
    VMSTATE_INT32(unlock_state, I6300State),
    VMSTATE_INT32(previous_reboot_flag, I6300State),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_i6300esb = {
    .name = "i6300esb_wdt",
    /* With this VMSD's introduction, version_id/minimum_version_id were
     * erroneously set to sizeof(I6300State), causing a somewhat random
     * version_id to be set for every build. This eventually broke
     * migration.
     *
     * To correct this without breaking old->new migration for older
     * versions of QEMU, we've set version_id to a value high enough
     * to exceed all past values of sizeof(I6300State) across various
     * build environments, and have reset minimum_version_id to 1,
     * since this VMSD has never changed and thus can accept all past
     * versions.
     *
     * For future changes we can treat these values as we normally would.
     */
    .version_id = 10000,
    .minimum_version_id = 1,
    .fields = vmstate_i6300esb_fields
};

void I6300State::realize(PCIDevice *dev, Error **errp)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);

    i6300esb_debug("I6300State = %p\n", d);

    d->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, I6300State::timerExpired, d);
    d->previous_reboot_flag = 0;

    memory_region_init_io(&d->io_mem, reinterpret_cast<Object *>(d), &i6300esb_ops, d,
                          "i6300esb", 0x10);
    pci_register_bar(&d->dev, 0, 0, &d->io_mem);
}

static void i6300esb_realize(PCIDevice *dev, Error **errp)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    d->realize(dev, errp);
}

void I6300State::exit(PCIDevice *dev)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);

    timer_free(d->timer);
}

static void i6300esb_exit(PCIDevice *dev)
{
    I6300State *d = reinterpret_cast<I6300State *>(dev);
    d->exit(dev);
}

void I6300State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->config_read = i6300esb_config_read;
    k->config_write = i6300esb_config_write;
    k->realize = i6300esb_realize;
    k->exit = i6300esb_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_ESB_9;
    k->class_id = PCI_CLASS_SYSTEM_OTHER;
    device_class_set_legacy_reset(dc, i6300esb_reset);
    dc->vmsd = &vmstate_i6300esb;
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
    dc->desc = "Intel 6300ESB";
}

static const InterfaceInfo i6300esb_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo i6300esb_info = {
    .name          = TYPE_WATCHDOG_I6300ESB_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(I6300State),
    .class_init    = I6300State::classInit,
    .interfaces = i6300esb_interfaces,
};

static void i6300esb_register_types(void)
{
    type_register_static(&i6300esb_info);
}

type_init(i6300esb_register_types)
