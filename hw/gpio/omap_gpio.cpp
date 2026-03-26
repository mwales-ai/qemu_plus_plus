/*
 * TI OMAP processors GPIO emulation.
 *
 * Copyright (C) 2006-2008 Andrzej Zaborowski  <balrog@zabor.org>
 * Copyright (C) 2007-2009 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
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

extern "C" {
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
}

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/arm/omap.h"
#include "hw/sysbus.h"

struct omap_gpio_s {
    qemu_irq irq;
    qemu_irq handler[16];

    uint16_t inputs;
    uint16_t outputs;
    uint16_t dir;
    uint16_t edge;
    uint16_t mask;
    uint16_t ints;
    uint16_t pins;

    static void gpioReset(struct omap_gpio_s *s);
};

struct Omap1GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    int mpu_model;
    void *clk;
    struct omap_gpio_s omap1;

    static void gpioSet(void *opaque, int line, int level);
    static uint64_t gpioRead(void *opaque, hwaddr addr, unsigned size);
    static void gpioWrite(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size);

    void reset();
    static void resetWrapper(DeviceState *dev);

    static void instanceInit(Object *obj);
    void realize(Error **errp);
    static void realizeWrapper(DeviceState *dev, Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
};

/* General-Purpose I/O of OMAP1 */
void Omap1GpioState::gpioSet(void *opaque, int line, int level)
{
    Omap1GpioState *p = static_cast<Omap1GpioState *>(opaque);
    struct omap_gpio_s *s = &p->omap1;
    uint16_t prev = s->inputs;

    if (level)
        s->inputs |= 1 << line;
    else
        s->inputs &= ~(1 << line);

    if (((s->edge & s->inputs & ~prev) | (~s->edge & ~s->inputs & prev)) &
                    (1 << line) & s->dir & ~s->mask) {
        s->ints |= 1 << line;
        qemu_irq_raise(s->irq);
    }
}

uint64_t Omap1GpioState::gpioRead(void *opaque, hwaddr addr, unsigned size)
{
    struct omap_gpio_s *s = static_cast<struct omap_gpio_s *>(opaque);
    int offset = addr & OMAP_MPUI_REG_MASK;

    if (size != 2) {
        return omap_badwidth_read16(opaque, addr);
    }

    switch (offset) {
    case 0x00:  /* DATA_INPUT */
        return s->inputs & s->pins;

    case 0x04:  /* DATA_OUTPUT */
        return s->outputs;

    case 0x08:  /* DIRECTION_CONTROL */
        return s->dir;

    case 0x0c:  /* INTERRUPT_CONTROL */
        return s->edge;

    case 0x10:  /* INTERRUPT_MASK */
        return s->mask;

    case 0x14:  /* INTERRUPT_STATUS */
        return s->ints;

    case 0x18:  /* PIN_CONTROL (not in OMAP310) */
        OMAP_BAD_REG(addr);
        return s->pins;
    }

    OMAP_BAD_REG(addr);
    return 0;
}

void Omap1GpioState::gpioWrite(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    struct omap_gpio_s *s = static_cast<struct omap_gpio_s *>(opaque);
    int offset = addr & OMAP_MPUI_REG_MASK;
    uint16_t diff;
    int ln;

    if (size != 2) {
        omap_badwidth_write16(opaque, addr, value);
        return;
    }

    switch (offset) {
    case 0x00:  /* DATA_INPUT */
        OMAP_RO_REG(addr);
        return;

    case 0x04:  /* DATA_OUTPUT */
        diff = (s->outputs ^ value) & ~s->dir;
        s->outputs = value;
        while ((ln = ctz32(diff)) != 32) {
            if (s->handler[ln])
                qemu_set_irq(s->handler[ln], (value >> ln) & 1);
            diff &= ~(1 << ln);
        }
        break;

    case 0x08:  /* DIRECTION_CONTROL */
        diff = s->outputs & (s->dir ^ value);
        s->dir = value;

        value = s->outputs & ~s->dir;
        while ((ln = ctz32(diff)) != 32) {
            if (s->handler[ln])
                qemu_set_irq(s->handler[ln], (value >> ln) & 1);
            diff &= ~(1 << ln);
        }
        break;

    case 0x0c:  /* INTERRUPT_CONTROL */
        s->edge = value;
        break;

    case 0x10:  /* INTERRUPT_MASK */
        s->mask = value;
        break;

    case 0x14:  /* INTERRUPT_STATUS */
        s->ints &= ~value;
        if (!s->ints)
            qemu_irq_lower(s->irq);
        break;

    case 0x18:  /* PIN_CONTROL (not in OMAP310 TRM) */
        OMAP_BAD_REG(addr);
        s->pins = value;
        break;

    default:
        OMAP_BAD_REG(addr);
        return;
    }
}

/* *Some* sources say the memory region is 32-bit.  */
static const MemoryRegionOps omap_gpio_ops = {
    .read = Omap1GpioState::gpioRead,
    .write = Omap1GpioState::gpioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void omap_gpio_s::gpioReset(struct omap_gpio_s *s)
{
    s->inputs = 0;
    s->outputs = ~0;
    s->dir = ~0;
    s->edge = ~0;
    s->mask = ~0;
    s->ints = 0;
    s->pins = ~0;
}

void Omap1GpioState::reset()
{
    omap_gpio_s::gpioReset(&omap1);
}

void Omap1GpioState::resetWrapper(DeviceState *dev)
{
    Omap1GpioState *s = OMAP1_GPIO(dev);
    s->reset();
}

void Omap1GpioState::instanceInit(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    Omap1GpioState *s = OMAP1_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qdev_init_gpio_in(dev, gpioSet, 16);
    qdev_init_gpio_out(dev, s->omap1.handler, 16);
    sysbus_init_irq(sbd, &s->omap1.irq);
    memory_region_init_io(&s->iomem, obj, &omap_gpio_ops, &s->omap1,
                          "omap.gpio", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

void Omap1GpioState::realize(Error **errp)
{
    if (!clk) {
        error_setg(errp, "omap-gpio: clk not connected");
    }
}

void Omap1GpioState::realizeWrapper(DeviceState *dev, Error **errp)
{
    Omap1GpioState *s = OMAP1_GPIO(dev);
    s->realize(errp);
}

extern "C"
void omap_gpio_set_clk(Omap1GpioState *gpio, omap_clk clk)
{
    gpio->clk = clk;
}

static const Property omap_gpio_properties[] = {
    DEFINE_PROP_INT32("mpu_model", Omap1GpioState, mpu_model, 0),
};

void Omap1GpioState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = realizeWrapper;
    device_class_set_legacy_reset(dc, resetWrapper);
    device_class_set_props(dc, omap_gpio_properties);
    /* Reason: pointer property "clk" */
    dc->user_creatable = false;
}

static const TypeInfo omap_gpio_info = {
    .name          = TYPE_OMAP1_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap1GpioState),
    .instance_init = Omap1GpioState::instanceInit,
    .class_init    = Omap1GpioState::classInit,
};

static void omap_gpio_register_types(void)
{
    type_register_static(&omap_gpio_info);
}

type_init(omap_gpio_register_types)
