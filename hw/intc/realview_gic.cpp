/*
 * ARM RealView Emulation Baseboard Interrupt Controller
 *
 * Copyright (c) 2006-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/intc/realview_gic.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"

static void realview_gic_set_irq(void *opaque, int irq, int level)
{
    RealViewGICState *s = static_cast<RealViewGICState *>(opaque);

    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->gic), irq), level);
}

void RealViewGICState::realize(Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);
    SysBusDevice *busdev;
    int numirq = 96;

    qdev_prop_set_uint32(DEVICE(&gic), "num-irq", numirq);
    if (!sysbus_realize(SYS_BUS_DEVICE(&gic), errp)) {
        return;
    }
    busdev = SYS_BUS_DEVICE(&gic);

    /* Pass through outbound IRQ lines from the GIC */
    sysbus_pass_irq(sbd, busdev);

    /* Pass through inbound GPIO lines to the GIC */
    qdev_init_gpio_in(DEVICE(this), realview_gic_set_irq, numirq - 32);

    memory_region_add_subregion(&container, 0,
                                sysbus_mmio_get_region(busdev, 1));
    memory_region_add_subregion(&container, 0x1000,
                                sysbus_mmio_get_region(busdev, 0));
}

void RealViewGICState::init()
{
    Object *obj = OBJECT(this);

    memory_region_init(&container, obj,
                       "realview-gic-container", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &container);

    object_initialize_child(obj, "gic", &gic, TYPE_ARM_GIC);
    qdev_prop_set_uint32(DEVICE(&gic), "num-cpu", 1);
}

void RealViewGICState::classInit(DeviceClass *dc)
{
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(RealViewGICState, TYPE_REALVIEW_GIC, TYPE_SYS_BUS_DEVICE)
