/*
 * QEMU M48T59 and M48T08 NVRAM emulation for PPC PREP and Sparc platforms
 *
 * Copyright (c) 2003-2005, 2007, 2017 Jocelyn Mayer
 * Copyright (c) 2013 Hervé Poussineau
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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/rtc/m48t59.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "system/rtc.h"
#include "system/system.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/bcd.h"
#include "qemu/module.h"
#include "trace.h"
#include "system/watchdog.h"

#include "m48t59-internal.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_M48TXX_SYS_BUS "sysbus-m48txx"
typedef struct M48txxSysBusDeviceClass M48txxSysBusDeviceClass;
typedef struct M48txxSysBusState M48txxSysBusState;
DECLARE_OBJ_CHECKERS(M48txxSysBusState, M48txxSysBusDeviceClass,
                     M48TXX_SYS_BUS, TYPE_M48TXX_SYS_BUS)

/*
 * Chipset docs:
 * http://www.st.com/stonline/products/literature/ds/2410/m48t02.pdf
 * http://www.st.com/stonline/products/literature/ds/2411/m48t08.pdf
 * http://www.st.com/stonline/products/literature/od/7001/m48t59y.pdf
 */

struct M48txxSysBusState {
    SysBusDevice parent_obj;
    M48t59State state;
    MemoryRegion io;

    /* methods */
    void init();
    void realize(Error **errp);
    void resetSysbus();
    uint32_t nvramRead(uint32_t addr);
    void nvramWrite(uint32_t addr, uint32_t val);
    void nvramToggleLock(int lock);

    static void classInit(ObjectClass *klass, const void *data);
    static void concreteClassInit(ObjectClass *klass, const void *data);
};

struct M48txxSysBusDeviceClass {
    SysBusDeviceClass parent_class;
    M48txxInfo info;
};

static M48txxInfo m48txx_sysbus_info[] = {
    {
        .bus_name = "sysbus-m48t02",
        .model = 2,
        .size = 0x800,
    },{
        .bus_name = "sysbus-m48t08",
        .model = 8,
        .size = 0x2000,
    },{
        .bus_name = "sysbus-m48t59",
        .model = 59,
        .size = 0x2000,
    }
};


/* Fake timer functions */

/* Alarm management */
static void alarm_cb(void *opaque)
{
    M48t59State *NVRAM = static_cast<M48t59State *>(opaque);
    NVRAM->alarmCb();
}

void M48t59State::alarmCb()
{
    struct tm tm;
    uint64_t next_time;

    qemu_set_irq(this->IRQ, 1);
    if ((this->buffer[0x1FF5] & 0x80) == 0 &&
        (this->buffer[0x1FF4] & 0x80) == 0 &&
        (this->buffer[0x1FF3] & 0x80) == 0 &&
        (this->buffer[0x1FF2] & 0x80) == 0) {
        /* Repeat once a month */
        qemu_get_timedate(&tm, this->time_offset);
        tm.tm_mon++;
        if (tm.tm_mon == 13) {
            tm.tm_mon = 1;
            tm.tm_year++;
        }
        next_time = qemu_timedate_diff(&tm) - this->time_offset;
    } else if ((this->buffer[0x1FF5] & 0x80) != 0 &&
               (this->buffer[0x1FF4] & 0x80) == 0 &&
               (this->buffer[0x1FF3] & 0x80) == 0 &&
               (this->buffer[0x1FF2] & 0x80) == 0) {
        /* Repeat once a day */
        next_time = 24 * 60 * 60;
    } else if ((this->buffer[0x1FF5] & 0x80) != 0 &&
               (this->buffer[0x1FF4] & 0x80) != 0 &&
               (this->buffer[0x1FF3] & 0x80) == 0 &&
               (this->buffer[0x1FF2] & 0x80) == 0) {
        /* Repeat once an hour */
        next_time = 60 * 60;
    } else if ((this->buffer[0x1FF5] & 0x80) != 0 &&
               (this->buffer[0x1FF4] & 0x80) != 0 &&
               (this->buffer[0x1FF3] & 0x80) != 0 &&
               (this->buffer[0x1FF2] & 0x80) == 0) {
        /* Repeat once a minute */
        next_time = 60;
    } else {
        /* Repeat once a second */
        next_time = 1;
    }
    timer_mod(this->alrm_timer, qemu_clock_get_ns(rtc_clock) +
                    next_time * 1000);
    qemu_set_irq(this->IRQ, 0);
}

void M48t59State::setAlarm()
{
    int64_t diff;
    if (this->alrm_timer != NULL) {
        timer_del(this->alrm_timer);
        diff = qemu_timedate_diff(&this->alarm) - this->time_offset;
        if (diff > 0)
            timer_mod(this->alrm_timer, diff * 1000);
    }
}

/* RTC management helpers */
void M48t59State::getTime(struct tm *tm)
{
    qemu_get_timedate(tm, this->time_offset);
}

void M48t59State::setTime(struct tm *tm)
{
    this->time_offset = qemu_timedate_diff(tm);
    this->setAlarm();
}

/* Watchdog management */
static void watchdog_cb(void *opaque)
{
    M48t59State *NVRAM = static_cast<M48t59State *>(opaque);
    NVRAM->watchdogCb();
}

void M48t59State::watchdogCb()
{
    this->buffer[0x1FF0] |= 0x80;
    if (this->buffer[0x1FF7] & 0x80) {
        this->buffer[0x1FF7] = 0x00;
        this->buffer[0x1FFC] &= ~0x40;
        watchdog_perform_action();
    } else {
        qemu_set_irq(this->IRQ, 1);
        qemu_set_irq(this->IRQ, 0);
    }
}

void M48t59State::setUpWatchdog(uint8_t value)
{
    uint64_t interval; /* in 1/16 seconds */

    this->buffer[0x1FF0] &= ~0x80;
    if (this->wd_timer != NULL) {
        timer_del(this->wd_timer);
        if (value != 0) {
            interval = (1 << (2 * (value & 0x03))) * ((value >> 2) & 0x1F);
            timer_mod(this->wd_timer, ((uint64_t)time(NULL) * 1000) +
                           ((interval * 1000) >> 4));
        }
    }
}

/* Direct access to NVRAM */
void m48t59_write(M48t59State *NVRAM, uint32_t addr, uint32_t val)
{
    struct tm tm;
    int tmp;

    trace_m48txx_nvram_mem_write(addr, val);

    /* check for NVRAM access */
    if ((NVRAM->model == 2 && addr < 0x7f8) ||
        (NVRAM->model == 8 && addr < 0x1ff8) ||
        (NVRAM->model == 59 && addr < 0x1ff0)) {
        goto do_write;
    }

    /* TOD access */
    switch (addr) {
    case 0x1FF0:
        /* flags register : read-only */
        break;
    case 0x1FF1:
        /* unused */
        break;
    case 0x1FF2:
        /* alarm seconds */
        tmp = from_bcd(val & 0x7F);
        if (tmp >= 0 && tmp <= 59) {
            NVRAM->alarm.tm_sec = tmp;
            NVRAM->buffer[0x1FF2] = val;
            NVRAM->setAlarm();
        }
        break;
    case 0x1FF3:
        /* alarm minutes */
        tmp = from_bcd(val & 0x7F);
        if (tmp >= 0 && tmp <= 59) {
            NVRAM->alarm.tm_min = tmp;
            NVRAM->buffer[0x1FF3] = val;
            NVRAM->setAlarm();
        }
        break;
    case 0x1FF4:
        /* alarm hours */
        tmp = from_bcd(val & 0x3F);
        if (tmp >= 0 && tmp <= 23) {
            NVRAM->alarm.tm_hour = tmp;
            NVRAM->buffer[0x1FF4] = val;
            NVRAM->setAlarm();
        }
        break;
    case 0x1FF5:
        /* alarm date */
        tmp = from_bcd(val & 0x3F);
        if (tmp != 0) {
            NVRAM->alarm.tm_mday = tmp;
            NVRAM->buffer[0x1FF5] = val;
            NVRAM->setAlarm();
        }
        break;
    case 0x1FF6:
        /* interrupts */
        NVRAM->buffer[0x1FF6] = val;
        break;
    case 0x1FF7:
        /* watchdog */
        NVRAM->buffer[0x1FF7] = val;
        NVRAM->setUpWatchdog(val);
        break;
    case 0x1FF8:
    case 0x07F8:
        /* control */
       NVRAM->buffer[addr] = (val & ~0xA0) | 0x90;
        break;
    case 0x1FF9:
    case 0x07F9:
        /* seconds (BCD) */
        tmp = from_bcd(val & 0x7F);
        if (tmp >= 0 && tmp <= 59) {
            NVRAM->getTime(&tm);
            tm.tm_sec = tmp;
            NVRAM->setTime(&tm);
        }
        if ((val & 0x80) ^ (NVRAM->buffer[addr] & 0x80)) {
            if (val & 0x80) {
                NVRAM->stop_time = time(NULL);
            } else {
                NVRAM->time_offset += NVRAM->stop_time - time(NULL);
                NVRAM->stop_time = 0;
            }
        }
        NVRAM->buffer[addr] = val & 0x80;
        break;
    case 0x1FFA:
    case 0x07FA:
        /* minutes (BCD) */
        tmp = from_bcd(val & 0x7F);
        if (tmp >= 0 && tmp <= 59) {
            NVRAM->getTime(&tm);
            tm.tm_min = tmp;
            NVRAM->setTime(&tm);
        }
        break;
    case 0x1FFB:
    case 0x07FB:
        /* hours (BCD) */
        tmp = from_bcd(val & 0x3F);
        if (tmp >= 0 && tmp <= 23) {
            NVRAM->getTime(&tm);
            tm.tm_hour = tmp;
            NVRAM->setTime(&tm);
        }
        break;
    case 0x1FFC:
    case 0x07FC:
        /* day of the week / century */
        tmp = from_bcd(val & 0x07);
        NVRAM->getTime(&tm);
        tm.tm_wday = tmp;
        NVRAM->setTime(&tm);
        NVRAM->buffer[addr] = val & 0x40;
        break;
    case 0x1FFD:
    case 0x07FD:
        /* date (BCD) */
        tmp = from_bcd(val & 0x3F);
        if (tmp != 0) {
            NVRAM->getTime(&tm);
            tm.tm_mday = tmp;
            NVRAM->setTime(&tm);
        }
        break;
    case 0x1FFE:
    case 0x07FE:
        /* month */
        tmp = from_bcd(val & 0x1F);
        if (tmp >= 1 && tmp <= 12) {
            NVRAM->getTime(&tm);
            tm.tm_mon = tmp - 1;
            NVRAM->setTime(&tm);
        }
        break;
    case 0x1FFF:
    case 0x07FF:
        /* year */
        tmp = from_bcd(val);
        if (tmp >= 0 && tmp <= 99) {
            NVRAM->getTime(&tm);
            tm.tm_year = from_bcd(val) + NVRAM->base_year - 1900;
            NVRAM->setTime(&tm);
        }
        break;
    default:
        /* Check lock registers state */
        if (addr >= 0x20 && addr <= 0x2F && (NVRAM->lock & 1))
            break;
        if (addr >= 0x30 && addr <= 0x3F && (NVRAM->lock & 2))
            break;
    do_write:
        if (addr < NVRAM->size) {
            NVRAM->buffer[addr] = val & 0xFF;
        }
        break;
    }
}

uint32_t m48t59_read(M48t59State *NVRAM, uint32_t addr)
{
    struct tm tm;
    uint32_t retval = 0xFF;

    /* check for NVRAM access */
    if ((NVRAM->model == 2 && addr < 0x078f) ||
        (NVRAM->model == 8 && addr < 0x1ff8) ||
        (NVRAM->model == 59 && addr < 0x1ff0)) {
        goto do_read;
    }

    /* TOD access */
    switch (addr) {
    case 0x1FF0:
        /* flags register */
        goto do_read;
    case 0x1FF1:
        /* unused */
        retval = 0;
        break;
    case 0x1FF2:
        /* alarm seconds */
        goto do_read;
    case 0x1FF3:
        /* alarm minutes */
        goto do_read;
    case 0x1FF4:
        /* alarm hours */
        goto do_read;
    case 0x1FF5:
        /* alarm date */
        goto do_read;
    case 0x1FF6:
        /* interrupts */
        goto do_read;
    case 0x1FF7:
        /* A read resets the watchdog */
        NVRAM->setUpWatchdog(NVRAM->buffer[0x1FF7]);
        goto do_read;
    case 0x1FF8:
    case 0x07F8:
        /* control */
        goto do_read;
    case 0x1FF9:
    case 0x07F9:
        /* seconds (BCD) */
        NVRAM->getTime(&tm);
        retval = (NVRAM->buffer[addr] & 0x80) | to_bcd(tm.tm_sec);
        break;
    case 0x1FFA:
    case 0x07FA:
        /* minutes (BCD) */
        NVRAM->getTime(&tm);
        retval = to_bcd(tm.tm_min);
        break;
    case 0x1FFB:
    case 0x07FB:
        /* hours (BCD) */
        NVRAM->getTime(&tm);
        retval = to_bcd(tm.tm_hour);
        break;
    case 0x1FFC:
    case 0x07FC:
        /* day of the week / century */
        NVRAM->getTime(&tm);
        retval = NVRAM->buffer[addr] | tm.tm_wday;
        break;
    case 0x1FFD:
    case 0x07FD:
        /* date */
        NVRAM->getTime(&tm);
        retval = to_bcd(tm.tm_mday);
        break;
    case 0x1FFE:
    case 0x07FE:
        /* month */
        NVRAM->getTime(&tm);
        retval = to_bcd(tm.tm_mon + 1);
        break;
    case 0x1FFF:
    case 0x07FF:
        /* year */
        NVRAM->getTime(&tm);
        retval = to_bcd((tm.tm_year + 1900 - NVRAM->base_year) % 100);
        break;
    default:
        /* Check lock registers state */
        if (addr >= 0x20 && addr <= 0x2F && (NVRAM->lock & 1))
            break;
        if (addr >= 0x30 && addr <= 0x3F && (NVRAM->lock & 2))
            break;
    do_read:
        if (addr < NVRAM->size) {
            retval = NVRAM->buffer[addr];
        }
        break;
    }
    trace_m48txx_nvram_mem_read(addr, retval);

    return retval;
}

/* IO access to NVRAM */
static void NVRAM_writeb(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    M48t59State *NVRAM = static_cast<M48t59State *>(opaque);

    trace_m48txx_nvram_io_write(addr, val);
    switch (addr) {
    case 0:
        NVRAM->addr &= ~0x00FF;
        NVRAM->addr |= val;
        break;
    case 1:
        NVRAM->addr &= ~0xFF00;
        NVRAM->addr |= val << 8;
        break;
    case 3:
        m48t59_write(NVRAM, NVRAM->addr, val);
        NVRAM->addr = 0x0000;
        break;
    default:
        break;
    }
}

static uint64_t NVRAM_readb(void *opaque, hwaddr addr, unsigned size)
{
    M48t59State *NVRAM = static_cast<M48t59State *>(opaque);
    uint32_t retval;

    switch (addr) {
    case 3:
        retval = m48t59_read(NVRAM, NVRAM->addr);
        break;
    default:
        retval = -1;
        break;
    }
    trace_m48txx_nvram_io_read(addr, retval);

    return retval;
}

static uint64_t nvram_read(void *opaque, hwaddr addr, unsigned size)
{
    M48t59State *NVRAM = static_cast<M48t59State *>(opaque);

    return m48t59_read(NVRAM, addr);
}

static void nvram_write(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size)
{
    M48t59State *NVRAM = static_cast<M48t59State *>(opaque);

    return m48t59_write(NVRAM, addr, value);
}

static const MemoryRegionOps nvram_ops = {
    .read = nvram_read,
    .write = nvram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
    .impl = { .min_access_size = 1, .max_access_size = 1, },
};

static const VMStateField vmstate_m48t59_fields[] = {
    VMSTATE_UINT8(lock, M48t59State),
    VMSTATE_UINT16(addr, M48t59State),
    VMSTATE_VBUFFER_UINT32(buffer, M48t59State, 0, NULL, size),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_m48t59 = {
    .name = "m48t59",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_m48t59_fields,
};

void m48t59_reset_common(M48t59State *NVRAM)
{
    NVRAM->addr = 0;
    NVRAM->lock = 0;
    if (NVRAM->alrm_timer != NULL)
        timer_del(NVRAM->alrm_timer);

    if (NVRAM->wd_timer != NULL)
        timer_del(NVRAM->wd_timer);
}

static void m48t59_reset_sysbus(DeviceState *d)
{
    M48txxSysBusState *sys = reinterpret_cast<M48txxSysBusState *>(d);
    sys->resetSysbus();
}

void M48txxSysBusState::resetSysbus()
{
    m48t59_reset_common(&this->state);
}

const MemoryRegionOps m48t59_io_ops = {
    .read = NVRAM_readb,
    .write = NVRAM_writeb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

void m48t59_realize_common(M48t59State *s, Error **errp)
{
    s->buffer = static_cast<uint8_t *>(g_malloc0(s->size));
    if (s->model == 59) {
        s->alrm_timer = timer_new_ns(rtc_clock, alarm_cb, s);
        s->wd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, watchdog_cb, s);
    }
    qemu_get_timedate(&s->alarm, 0);
}

void M48txxSysBusState::init()
{
    M48txxSysBusDeviceClass *u = M48TXX_SYS_BUS_GET_CLASS(this);
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(this);
    M48t59State *s = &this->state;

    s->model = u->info.model;
    s->size = u->info.size;
    sysbus_init_irq(dev, &s->IRQ);

    memory_region_init_io(&s->iomem, reinterpret_cast<Object *>(this), &nvram_ops, s, "m48t59.nvram",
                          s->size);
    memory_region_init_io(&this->io, reinterpret_cast<Object *>(this), &m48t59_io_ops, s, "m48t59", 4);
}

static void m48t59_realize(DeviceState *dev, Error **errp)
{
    M48txxSysBusState *d = reinterpret_cast<M48txxSysBusState *>(dev);
    d->realize(errp);
}

void M48txxSysBusState::realize(Error **errp)
{
    M48t59State *s = &this->state;
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_mmio(sbd, &this->io);
    m48t59_realize_common(s, errp);
}

static uint32_t m48txx_sysbus_read(Nvram *obj, uint32_t addr)
{
    M48txxSysBusState *d = reinterpret_cast<M48txxSysBusState *>(obj);
    return d->nvramRead(addr);
}

uint32_t M48txxSysBusState::nvramRead(uint32_t addr)
{
    return m48t59_read(&this->state, addr);
}

static void m48txx_sysbus_write(Nvram *obj, uint32_t addr, uint32_t val)
{
    M48txxSysBusState *d = reinterpret_cast<M48txxSysBusState *>(obj);
    d->nvramWrite(addr, val);
}

void M48txxSysBusState::nvramWrite(uint32_t addr, uint32_t val)
{
    m48t59_write(&this->state, addr, val);
}

static void m48txx_sysbus_toggle_lock(Nvram *obj, int lock)
{
    M48txxSysBusState *d = reinterpret_cast<M48txxSysBusState *>(obj);
    d->nvramToggleLock(lock);
}

void M48txxSysBusState::nvramToggleLock(int lock)
{
    m48t59_toggle_lock(&this->state, lock);
}

static const Property m48t59_sysbus_properties[] = {
    DEFINE_PROP_INT32("base-year", M48txxSysBusState, state.base_year, 0),
};

void M48txxSysBusState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    NvramClass *nc = reinterpret_cast<NvramClass *>(klass);

    dc->realize = m48t59_realize;
    device_class_set_legacy_reset(dc, m48t59_reset_sysbus);
    device_class_set_props(dc, m48t59_sysbus_properties);
    dc->vmsd = &vmstate_m48t59;
    nc->read = m48txx_sysbus_read;
    nc->write = m48txx_sysbus_write;
    nc->toggle_lock = m48txx_sysbus_toggle_lock;
}

void M48txxSysBusState::concreteClassInit(ObjectClass *klass,
                                           const void *data)
{
    M48txxSysBusDeviceClass *u = reinterpret_cast<M48txxSysBusDeviceClass *>(klass);
    const M48txxInfo *info = static_cast<const M48txxInfo *>(data);

    u->info = *info;
}

static const InterfaceInfo m48txx_sysbus_interfaces[] = {
    { TYPE_NVRAM },
    { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_INTERFACE(NvramClass, TYPE_NVRAM)

REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS_IFACES(M48txxSysBusState,
                                                      TYPE_M48TXX_SYS_BUS,
                                                      TYPE_SYS_BUS_DEVICE,
                                                      M48txxSysBusState::classInit,
                                                      m48txx_sysbus_interfaces)

static void m48t59_register_concrete_types(void)
{
    TypeInfo sysbus_type_info = {
        .parent = TYPE_M48TXX_SYS_BUS,
        .class_size = sizeof(M48txxSysBusDeviceClass),
        .class_init = M48txxSysBusState::concreteClassInit,
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(m48txx_sysbus_info); i++) {
        sysbus_type_info.name = m48txx_sysbus_info[i].bus_name;
        sysbus_type_info.class_data = &m48txx_sysbus_info[i];
        type_register_static(&sysbus_type_info);
    }
}

type_init(m48t59_register_concrete_types)
