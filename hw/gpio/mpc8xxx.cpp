/*
 *  GPIO Controller for a lot of Freescale SoCs
 *
 * Copyright (C) 2014 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Author: Alexander Graf, <agraf@suse.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_MPC8XXX_GPIO "mpc8xxx_gpio"
OBJECT_DECLARE_SIMPLE_TYPE(MPC8XXXGPIOState, MPC8XXX_GPIO)

struct MPC8XXXGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq out[32];

    uint32_t dir;
    uint32_t odr;
    uint32_t dat;
    uint32_t ier;
    uint32_t imr;
    uint32_t icr;

    void update()
    {
        qemu_set_irq(irq, !!(ier & imr));
    }

    void writeData(uint32_t new_data)
    {
        uint32_t old_data = dat;
        uint32_t diff = old_data ^ new_data;
        int i;

        for (i = 0; i < 32; i++) {
            uint32_t mask = 0x80000000 >> i;
            if (!(diff & mask)) {
                continue;
            }

            if (dir & mask) {
                /* Output */
                qemu_set_irq(out[i], (new_data & mask) != 0);
            }
        }

        dat = new_data;
    }

    static uint64_t read(void *opaque, hwaddr offset,
                         unsigned size)
    {
        MPC8XXXGPIOState *s = static_cast<MPC8XXXGPIOState *>(opaque);

        if (size != 4) {
            /* All registers are 32bit */
            return 0;
        }

        switch (offset) {
        case 0x0: /* Direction */
            return s->dir;
        case 0x4: /* Open Drain */
            return s->odr;
        case 0x8: /* Data */
            return s->dat;
        case 0xC: /* Interrupt Event */
            return s->ier;
        case 0x10: /* Interrupt Mask */
            return s->imr;
        case 0x14: /* Interrupt Control */
            return s->icr;
        default:
            return 0;
        }
    }

    static void write(void *opaque, hwaddr offset,
                      uint64_t value, unsigned size)
    {
        MPC8XXXGPIOState *s = static_cast<MPC8XXXGPIOState *>(opaque);

        if (size != 4) {
            /* All registers are 32bit */
            return;
        }

        switch (offset) {
        case 0x0: /* Direction */
            s->dir = value;
            break;
        case 0x4: /* Open Drain */
            s->odr = value;
            break;
        case 0x8: /* Data */
            s->writeData(value);
            break;
        case 0xC: /* Interrupt Event */
            s->ier &= ~value;
            break;
        case 0x10: /* Interrupt Mask */
            s->imr = value;
            break;
        case 0x14: /* Interrupt Control */
            s->icr = value;
            break;
        }

        s->update();
    }

    void reset()
    {
        dir = 0;
        odr = 0;
        dat = 0;
        ier = 0;
        imr = 0;
        icr = 0;
    }

    static void resetWrapper(DeviceState *dev)
    {
        MPC8XXXGPIOState *s = MPC8XXX_GPIO(dev);
        s->reset();
    }

    static void setIrq(void *opaque, int irq_num, int level)
    {
        MPC8XXXGPIOState *s = static_cast<MPC8XXXGPIOState *>(opaque);
        uint32_t mask;

        mask = 0x80000000 >> irq_num;
        if ((s->dir & mask) == 0) {
            uint32_t old_value = s->dat & mask;

            s->dat &= ~mask;
            if (level)
                s->dat |= mask;

            if (!(s->icr & irq_num) || (old_value && !level)) {
                s->ier |= mask;
            }

            s->update();
        }
    }

    static const MemoryRegionOps ops;

    static void instanceInit(Object *obj)
    {
        DeviceState *dev = DEVICE(obj);
        MPC8XXXGPIOState *s = MPC8XXX_GPIO(obj);
        SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

        memory_region_init_io(&s->iomem, obj, &ops,
                              s, "mpc8xxx_gpio", 0x1000);
        sysbus_init_mmio(sbd, &s->iomem);
        sysbus_init_irq(sbd, &s->irq);
        qdev_init_gpio_in(dev, setIrq, 32);
        qdev_init_gpio_out(dev, s->out, 32);
    }

    static void classInit(ObjectClass *klass, const void *data);
};

static const VMStateField vmstate_mpc8xxx_gpio_fields[] = {
    VMSTATE_UINT32(dir, MPC8XXXGPIOState),
    VMSTATE_UINT32(odr, MPC8XXXGPIOState),
    VMSTATE_UINT32(dat, MPC8XXXGPIOState),
    VMSTATE_UINT32(ier, MPC8XXXGPIOState),
    VMSTATE_UINT32(imr, MPC8XXXGPIOState),
    VMSTATE_UINT32(icr, MPC8XXXGPIOState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_mpc8xxx_gpio = {
    .name = "mpc8xxx_gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_mpc8xxx_gpio_fields,
};

void MPC8XXXGPIOState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_mpc8xxx_gpio;
    device_class_set_legacy_reset(dc, resetWrapper);
}

const MemoryRegionOps MPC8XXXGPIOState::ops = {
    .read = MPC8XXXGPIOState::read,
    .write = MPC8XXXGPIOState::write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static const TypeInfo mpc8xxx_gpio_types[] = {
    {
        .name          = TYPE_MPC8XXX_GPIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MPC8XXXGPIOState),
        .instance_init = MPC8XXXGPIOState::instanceInit,
        .class_init    = MPC8XXXGPIOState::classInit,
    },
};

DEFINE_TYPES(mpc8xxx_gpio_types)
