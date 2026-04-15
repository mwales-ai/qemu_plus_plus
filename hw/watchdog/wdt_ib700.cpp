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
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/watchdog.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "qom/cpp/object.h"

/*#define IB700_DEBUG 1*/

#ifdef IB700_DEBUG
#define ib700_debug(fs,...)                                    \
    fprintf(stderr,"ib700: %s: "fs,__func__,##__VA_ARGS__)
#else
#define ib700_debug(fs,...)
#endif

#define TYPE_IB700 "ib700"
typedef struct IB700state IB700State;
DECLARE_INSTANCE_CHECKER(IB700State, IB700,
                         TYPE_IB700)

struct IB700state {
    ISADevice parent_obj;

    QEMUTimer *timer;

    PortioList port_list;

    void realize(Error **errp);
    void reset();
    static void classInit(DeviceClass *dc);
};

/* This is the timer.  We use a global here because the watchdog
 * code ensures there is only one watchdog (it is located at a fixed,
 * unchangeable IO port, so there could only ever be one anyway).
 */

/* A write to this register enables the timer. */
static void ib700_write_enable_reg(void *vp, uint32_t addr, uint32_t data)
{
    IB700State *s = static_cast<IB700State *>(vp);
    static int time_map[] = {
        30, 28, 26, 24, 22, 20, 18, 16,
        14, 12, 10,  8,  6,  4,  2,  0
    };
    int64_t timeout;

    ib700_debug("addr = %x, data = %x\n", addr, data);

    timeout = (int64_t) time_map[data & 0xF] * NANOSECONDS_PER_SECOND;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + timeout);
}

/* A write (of any value) to this register disables the timer. */
static void ib700_write_disable_reg(void *vp, uint32_t addr, uint32_t data)
{
    IB700State *s = static_cast<IB700State *>(vp);

    ib700_debug("addr = %x, data = %x\n", addr, data);

    timer_del(s->timer);
}

/* This is called when the watchdog expires. */
static void ib700_timer_expired(void *vp)
{
    IB700State *s = static_cast<IB700State *>(vp);

    ib700_debug("watchdog expired\n");

    watchdog_perform_action();
    timer_del(s->timer);
}

static const VMStateField vmstate_ib700_fields[] = {
    VMSTATE_TIMER_PTR(timer, IB700State),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_ib700 = {
    .name = "ib700_wdt",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = vmstate_ib700_fields,
};

static const MemoryRegionPortio wdt_portio_list[] = {
    { 0x441, 2, 1, .write = ib700_write_disable_reg, },
    { 0x443, 2, 1, .write = ib700_write_enable_reg, },
    PORTIO_END_OF_LIST(),
};

void IB700state::realize(Error **errp)
{
    ib700_debug("watchdog init\n");

    timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ib700_timer_expired, this);

    portio_list_init(&port_list, reinterpret_cast<Object *>(this),
                     wdt_portio_list, this, "ib700");
    portio_list_add(&port_list, isa_address_space_io(&parent_obj), 0);
}

void IB700state::reset()
{
    ib700_debug("watchdog reset\n");

    timer_del(timer);
}

void IB700state::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_ib700;
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
    dc->desc = "iBASE 700";
}

REGISTER_QEMU_DEVICE(IB700state, TYPE_IB700, TYPE_ISA_DEVICE)
