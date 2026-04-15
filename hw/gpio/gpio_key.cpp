/*
 * GPIO key
 *
 * Copyright (c) 2016 Linaro Limited
 *
 * Author: Shannon Zhao <shannon.zhao@linaro.org>
 *
 * Emulate a (human) keypress -- when the key is triggered by
 * setting the incoming gpio line, the outbound irq line is
 * raised for 100ms before being dropped again.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qom/cpp/object.h"

#define TYPE_GPIOKEY "gpio-key"
OBJECT_DECLARE_SIMPLE_TYPE(GPIOKEYState, GPIOKEY)
#define GPIO_KEY_LATENCY 100 /* 100ms */

struct GPIOKEYState {
    SysBusDevice parent_obj;

    QEMUTimer *timer;
    qemu_irq irq;

    void reset()
    {
        timer_del(timer);
    }

    void realize(Error **errp)
    {
        SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

        sysbus_init_irq(sbd, &irq);
        qdev_init_gpio_in(reinterpret_cast<DeviceState *>(this), setIrq, 1);
        timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, timerExpired, this);
    }

    static void classInit(DeviceClass *dc);

    static void timerExpired(void *opaque)
    {
        GPIOKEYState *s = static_cast<GPIOKEYState *>(opaque);

        qemu_set_irq(s->irq, 0);
        timer_del(s->timer);
    }

    static void setIrq(void *opaque, int irq, int level)
    {
        GPIOKEYState *s = static_cast<GPIOKEYState *>(opaque);

        qemu_set_irq(s->irq, 1);
        timer_mod(s->timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + GPIO_KEY_LATENCY);
    }
};

static const VMStateField vmstate_gpio_key_fields[] = {
    VMSTATE_TIMER_PTR(timer, GPIOKEYState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_gpio_key = {
    .name = "gpio-key",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_gpio_key_fields,
};

void GPIOKEYState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_gpio_key;
}

REGISTER_QEMU_DEVICE(GPIOKEYState, TYPE_GPIOKEY, TYPE_SYS_BUS_DEVICE)
