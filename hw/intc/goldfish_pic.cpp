/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Goldfish PIC
 *
 * (c) 2020 Laurent Vivier <laurent@vivier.eu>
 *
 */

#include "qemu/osdep.h"

extern "C" {
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "hw/intc/intc.h"
#include "hw/intc/goldfish_pic.h"
}

#include "trace.h"

/* registers */

enum {
    REG_STATUS          = 0x00,
    REG_IRQ_PENDING     = 0x04,
    REG_IRQ_DISABLE_ALL = 0x08,
    REG_DISABLE         = 0x0c,
    REG_ENABLE          = 0x10,
};

static bool goldfish_pic_get_statistics(InterruptStatsProvider *obj,
                                        uint64_t **irq_counts,
                                        unsigned int *nb_irqs)
{
    GoldfishPICState *s = GOLDFISH_PIC(obj);

    *irq_counts = s->stats_irq_count;
    *nb_irqs = ARRAY_SIZE(s->stats_irq_count);
    return true;
}

static void goldfish_pic_print_info(InterruptStatsProvider *obj, GString *buf)
{
    GoldfishPICState *s = GOLDFISH_PIC(obj);
    g_string_append_printf(buf,
                           "goldfish-pic.%d: pending=0x%08x enabled=0x%08x\n",
                           s->idx, s->pending, s->enabled);
}

static void goldfish_pic_update(GoldfishPICState *s)
{
    if (s->pending & s->enabled) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static void goldfish_irq_request(void *opaque, int irq, int level)
{
    GoldfishPICState *s = static_cast<GoldfishPICState *>(opaque);

    trace_goldfish_irq_request(s, s->idx, irq, level);

    if (level) {
        s->pending |= 1 << irq;
        s->stats_irq_count[irq]++;
    } else {
        s->pending &= ~(1 << irq);
    }
    goldfish_pic_update(s);
}

static uint64_t goldfish_pic_read(void *opaque, hwaddr addr,
                                  unsigned size)
{
    GoldfishPICState *s = static_cast<GoldfishPICState *>(opaque);
    uint64_t value = 0;

    switch (addr) {
    case REG_STATUS:
        /* The number of pending interrupts (0 to 32) */
        value = ctpop32(s->pending & s->enabled);
        break;
    case REG_IRQ_PENDING:
        /* The pending interrupt mask */
        value = s->pending & s->enabled;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented register read 0x%02" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    trace_goldfish_pic_read(s, s->idx, addr, size, value);

    return value;
}

static void goldfish_pic_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    GoldfishPICState *s = static_cast<GoldfishPICState *>(opaque);

    trace_goldfish_pic_write(s, s->idx, addr, size, value);

    switch (addr) {
    case REG_IRQ_DISABLE_ALL:
        s->enabled = 0;
        s->pending = 0;
        break;
    case REG_DISABLE:
        s->enabled &= ~value;
        break;
    case REG_ENABLE:
        s->enabled |= value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented register write 0x%02" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
    goldfish_pic_update(s);
}

static MemoryRegionOps goldfish_pic_ops;

static void __attribute__((constructor)) init_goldfish_pic_ops(void)
{
    memset(&goldfish_pic_ops, 0, sizeof(goldfish_pic_ops));
    goldfish_pic_ops.read = goldfish_pic_read;
    goldfish_pic_ops.write = goldfish_pic_write;
    goldfish_pic_ops.endianness = DEVICE_NATIVE_ENDIAN;
    goldfish_pic_ops.valid.max_access_size = 4;
    goldfish_pic_ops.impl.min_access_size = 4;
    goldfish_pic_ops.impl.max_access_size = 4;
}

static void goldfish_pic_reset(DeviceState *dev)
{
    GoldfishPICState *s = GOLDFISH_PIC(dev);
    int i;

    trace_goldfish_pic_reset(s, s->idx);
    s->pending = 0;
    s->enabled = 0;

    for (i = 0; i < ARRAY_SIZE(s->stats_irq_count); i++) {
        s->stats_irq_count[i] = 0;
    }
}

static void goldfish_pic_realize(DeviceState *dev, Error **errp)
{
    GoldfishPICState *s = GOLDFISH_PIC(dev);

    trace_goldfish_pic_realize(s, s->idx);

    memory_region_init_io(&s->iomem, OBJECT(s), &goldfish_pic_ops, s,
                          "goldfish_pic", 0x24);
}

static const VMStateField vmstate_goldfish_pic_fields[] = {
    VMSTATE_UINT32(pending, GoldfishPICState),
    VMSTATE_UINT32(enabled, GoldfishPICState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_goldfish_pic = {
    .name = "goldfish_pic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_goldfish_pic_fields,
};

void GoldfishPICState::init()
{
    SysBusDevice *dev = SYS_BUS_DEVICE(this);

    trace_goldfish_pic_instance_init(this);

    sysbus_init_mmio(dev, &iomem);
    sysbus_init_irq(dev, &irq);

    qdev_init_gpio_in(DEVICE(this), goldfish_irq_request, GOLDFISH_PIC_IRQ_NB);
}

static const Property goldfish_pic_properties[] = {
    DEFINE_PROP_UINT8("index", GoldfishPICState, idx, 0),
};

void GoldfishPICState::classInit(DeviceClass *dc)
{
    ObjectClass *oc = reinterpret_cast<ObjectClass *>(dc);
    InterruptStatsProviderClass *ic = INTERRUPT_STATS_PROVIDER_CLASS(oc);

    device_class_set_legacy_reset(dc, goldfish_pic_reset);
    dc->realize = goldfish_pic_realize;
    dc->vmsd = &vmstate_goldfish_pic;
    ic->get_statistics = goldfish_pic_get_statistics;
    ic->print_info = goldfish_pic_print_info;
    device_class_set_props(dc, goldfish_pic_properties);
}

static const InterfaceInfo goldfish_pic_interfaces[] = {
     { TYPE_INTERRUPT_STATS_PROVIDER },
     { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(GoldfishPICState, TYPE_GOLDFISH_PIC,
                             TYPE_SYS_BUS_DEVICE, goldfish_pic_interfaces)
