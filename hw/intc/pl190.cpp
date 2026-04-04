/*
 * Arm PrimeCell PL190 Vector Interrupt Controller
 *
 * Copyright (c) 2006 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

/* The number of virtual priority levels.  16 user vectors plus the
   unvectored IRQ.  Chained interrupts would require an additional level
   if implemented.  */

#define PL190_NUM_PRIO 17

#define TYPE_PL190 "pl190"
OBJECT_DECLARE_SIMPLE_TYPE(PL190State, PL190)

static const unsigned char pl190_id[] =
{ 0x90, 0x11, 0x04, 0x00, 0x0D, 0xf0, 0x05, 0xb1 };

struct PL190State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t level;
    uint32_t soft_level;
    uint32_t irq_enable;
    uint32_t fiq_select;
    uint8_t vect_control[16];
    uint32_t vect_addr[PL190_NUM_PRIO];
    /* Mask containing interrupts with higher priority than this one.  */
    uint32_t prio_mask[PL190_NUM_PRIO + 1];
    int protected_val;
    /* Current priority level.  */
    int priority;
    int prev_prio[PL190_NUM_PRIO];
    qemu_irq irq;
    qemu_irq fiq;

    uint32_t irqLevel()
    {
        return (level | soft_level) & irq_enable & ~fiq_select;
    }

    void update()
    {
        uint32_t lvl = irqLevel();
        int set;

        set = (lvl & prio_mask[priority]) != 0;
        qemu_set_irq(irq, set);
        set = ((level | soft_level) & fiq_select) != 0;
        qemu_set_irq(fiq, set);
    }

    void updateVectors()
    {
        uint32_t mask;
        int i;
        int n;

        mask = 0;
        for (i = 0; i < 16; i++)
          {
            prio_mask[i] = mask;
            if (vect_control[i] & 0x20)
              {
                n = vect_control[i] & 0x1f;
                mask |= 1 << n;
              }
          }
        prio_mask[16] = mask;
        update();
    }

    static void setIrq(void *opaque, int irq_num, int level_val)
    {
        PL190State *s = static_cast<PL190State *>(opaque);

        if (level_val)
            s->level |= 1u << irq_num;
        else
            s->level &= ~(1u << irq_num);
        s->update();
    }

    static uint64_t read(void *opaque, hwaddr offset,
                         unsigned size)
    {
        PL190State *s = static_cast<PL190State *>(opaque);
        int i;

        if (offset >= 0xfe0 && offset < 0x1000) {
            return pl190_id[(offset - 0xfe0) >> 2];
        }
        if (offset >= 0x100 && offset < 0x140) {
            return s->vect_addr[(offset - 0x100) >> 2];
        }
        if (offset >= 0x200 && offset < 0x240) {
            return s->vect_control[(offset - 0x200) >> 2];
        }
        switch (offset >> 2) {
        case 0: /* IRQSTATUS */
            return s->irqLevel();
        case 1: /* FIQSATUS */
            return (s->level | s->soft_level) & s->fiq_select;
        case 2: /* RAWINTR */
            return s->level | s->soft_level;
        case 3: /* INTSELECT */
            return s->fiq_select;
        case 4: /* INTENABLE */
            return s->irq_enable;
        case 6: /* SOFTINT */
            return s->soft_level;
        case 8: /* PROTECTION */
            return s->protected_val;
        case 12: /* VECTADDR */
            /* Read vector address at the start of an ISR.  Increases the
             * current priority level to that of the current interrupt.
             *
             * Since an enabled interrupt X at priority P causes prio_mask[Y]
             * to have bit X set for all Y > P, this loop will stop with
             * i == the priority of the highest priority set interrupt.
             */
            for (i = 0; i < s->priority; i++) {
                if ((s->level | s->soft_level) & s->prio_mask[i + 1]) {
                    break;
                }
            }

            /* Reading this value with no pending interrupts is undefined.
               We return the default address.  */
            if (i == PL190_NUM_PRIO)
              return s->vect_addr[16];
            if (i < s->priority)
              {
                s->prev_prio[i] = s->priority;
                s->priority = i;
                s->update();
              }
            return s->vect_addr[s->priority];
        case 13: /* DEFVECTADDR */
            return s->vect_addr[16];
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pl190_read: Bad offset %x\n", (int)offset);
            return 0;
        }
    }

    static void write(void *opaque, hwaddr offset,
                      uint64_t val, unsigned size)
    {
        PL190State *s = static_cast<PL190State *>(opaque);

        if (offset >= 0x100 && offset < 0x140) {
            s->vect_addr[(offset - 0x100) >> 2] = val;
            s->updateVectors();
            return;
        }
        if (offset >= 0x200 && offset < 0x240) {
            s->vect_control[(offset - 0x200) >> 2] = val;
            s->updateVectors();
            return;
        }
        switch (offset >> 2) {
        case 0: /* SELECT */
            /* This is a readonly register, but linux tries to write to it
               anyway.  Ignore the write.  */
            break;
        case 3: /* INTSELECT */
            s->fiq_select = val;
            break;
        case 4: /* INTENABLE */
            s->irq_enable |= val;
            break;
        case 5: /* INTENCLEAR */
            s->irq_enable &= ~val;
            break;
        case 6: /* SOFTINT */
            s->soft_level |= val;
            break;
        case 7: /* SOFTINTCLEAR */
            s->soft_level &= ~val;
            break;
        case 8: /* PROTECTION */
            /* TODO: Protection (supervisor only access) is not implemented.  */
            s->protected_val = val & 1;
            break;
        case 12: /* VECTADDR */
            /* Restore the previous priority level.  The value written is
               ignored.  */
            if (s->priority < PL190_NUM_PRIO)
                s->priority = s->prev_prio[s->priority];
            break;
        case 13: /* DEFVECTADDR */
            s->vect_addr[16] = val;
            break;
        case 0xc0: /* ITCR */
            if (val) {
                qemu_log_mask(LOG_UNIMP, "pl190: Test mode not implemented\n");
            }
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                         "pl190_write: Bad offset %x\n", (int)offset);
            return;
        }
        s->update();
    }

    static const MemoryRegionOps ops;

    void reset()
    {
        int i;

        for (i = 0; i < 16; i++) {
            vect_addr[i] = 0;
            vect_control[i] = 0;
        }
        vect_addr[16] = 0;
        prio_mask[17] = 0xffffffff;
        priority = PL190_NUM_PRIO;
        updateVectors();
    }

    static void resetWrapper(DeviceState *d)
    {
        PL190State *s = reinterpret_cast<PL190State *>(d);
        s->reset();
    }

    static void instanceInit(Object *obj)
    {
        DeviceState *dev = reinterpret_cast<DeviceState *>(obj);
        PL190State *s = reinterpret_cast<PL190State *>(obj);
        SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(obj);

        memory_region_init_io(&s->iomem, obj, &ops, s, "pl190", 0x1000);
        sysbus_init_mmio(sbd, &s->iomem);
        qdev_init_gpio_in(dev, setIrq, 32);
        sysbus_init_irq(sbd, &s->irq);
        sysbus_init_irq(sbd, &s->fiq);
    }

    static void classInit(ObjectClass *klass, const void *data);
};

const MemoryRegionOps PL190State::ops = {
    .read = PL190State::read,
    .write = PL190State::write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateField vmstate_pl190_fields[] = {
    VMSTATE_UINT32(level, PL190State),
    VMSTATE_UINT32(soft_level, PL190State),
    VMSTATE_UINT32(irq_enable, PL190State),
    VMSTATE_UINT32(fiq_select, PL190State),
    VMSTATE_UINT8_ARRAY(vect_control, PL190State, 16),
    VMSTATE_UINT32_ARRAY(vect_addr, PL190State, PL190_NUM_PRIO),
    VMSTATE_UINT32_ARRAY(prio_mask, PL190State, PL190_NUM_PRIO+1),
    VMSTATE_INT32(protected_val, PL190State),
    VMSTATE_INT32(priority, PL190State),
    VMSTATE_INT32_ARRAY(prev_prio, PL190State, PL190_NUM_PRIO),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_pl190 = {
    .name = "pl190",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_pl190_fields,
};

void PL190State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    device_class_set_legacy_reset(dc, resetWrapper);
    dc->vmsd = &vmstate_pl190;
}

static const TypeInfo pl190_info = {
    .name          = TYPE_PL190,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL190State),
    .instance_init = PL190State::instanceInit,
    .class_init    = PL190State::classInit,
};

static void pl190_register_types(void)
{
    type_register_static(&pl190_info);
}

type_init(pl190_register_types)
