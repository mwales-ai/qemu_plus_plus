/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Loongson 7A1000 msi interrupt controller.
 *
 * Copyright (C) 2021 Loongson Technology Corporation Limited
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/intc/loongarch_pch_msi.h"
#include "hw/intc/loongarch_pch_pic.h"
#include "hw/pci/msi.h"
#include "hw/misc/unimp.h"
#include "migration/vmstate.h"
#include "system/kvm.h"
#include "trace.h"

static uint64_t loongarch_msi_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void loongarch_msi_mem_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    LoongArchPCHMSI *s = (LoongArchPCHMSI *)opaque;
    int irq_num;

    if (kvm_irqchip_in_kernel()) {
        MSIMessage msg;

        msg.address = addr;
        msg.data = val;
        kvm_irqchip_send_msi(kvm_state, msg);
        return;
    }

    /*
     * vector number is irq number from upper extioi intc
     * need subtract irq base to get msi vector offset
     */
    irq_num = (val & 0xff) - s->irq_base;
    trace_loongarch_msi_set_irq(irq_num);
    assert(irq_num < s->irq_num);
    qemu_set_irq(s->pch_msi_irq[irq_num], 1);
}

static const MemoryRegionOps loongarch_pch_msi_ops = {
    .read  = loongarch_msi_mem_read,
    .write = loongarch_msi_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void LoongArchPCHMSI::realize(Error **errp)
{
    DeviceState *dev = DEVICE(this);

    if (!irq_num || irq_num  > PCH_MSI_IRQ_NUM) {
        error_setg(errp, "Invalid 'msi_irq_num'");
        return;
    }

    pch_msi_irq = g_new(qemu_irq, irq_num);
    qdev_init_gpio_out(dev, pch_msi_irq, irq_num);
}

static void loongarch_pch_msi_unrealize(DeviceState *dev)
{
    LoongArchPCHMSI *s = LOONGARCH_PCH_MSI(dev);

    g_free(s->pch_msi_irq);
}

void LoongArchPCHMSI::init()
{
    Object *obj = OBJECT(this);
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);

    memory_region_init_io(&msi_mmio, obj, &loongarch_pch_msi_ops,
                          this, TYPE_LOONGARCH_PCH_MSI, 0x8);
    sysbus_init_mmio(sbd, &msi_mmio);
    msi_nonbroken = true;
}

static const Property loongarch_msi_properties[] = {
    DEFINE_PROP_UINT32("msi_irq_base", LoongArchPCHMSI, irq_base, 0),
    DEFINE_PROP_UINT32("msi_irq_num",  LoongArchPCHMSI, irq_num, 0),
};

void LoongArchPCHMSI::classInit(DeviceClass *dc)
{
    dc->unrealize = loongarch_pch_msi_unrealize;
    device_class_set_props(dc, loongarch_msi_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(LoongArchPCHMSI, TYPE_LOONGARCH_PCH_MSI, TYPE_SYS_BUS_DEVICE)
