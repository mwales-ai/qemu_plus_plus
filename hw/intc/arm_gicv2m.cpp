/*
 *  GICv2m extension for MSI/MSI-x support with a GICv2-based system
 *
 * Copyright (C) 2015 Linaro, All rights reserved.
 *
 * Author: Christoffer Dall <christoffer.dall@linaro.org>
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

/* This file implements an emulated GICv2m widget as described in the ARM
 * Server Base System Architecture (SBSA) specification Version 2.2
 * (ARM-DEN-0029 v2.2) pages 35-39 without any optional implementation defined
 * identification registers and with a single non-secure MSI register frame.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "system/kvm.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_ARM_GICV2M "arm-gicv2m"
OBJECT_DECLARE_SIMPLE_TYPE(ARMGICv2mState, ARM_GICV2M)

#define GICV2M_NUM_SPI_MAX 128

#define V2M_MSI_TYPER           0x008
#define V2M_MSI_SETSPI_NS       0x040
#define V2M_MSI_IIDR            0xFCC
#define V2M_IIDR0               0xFD0
#define V2M_IIDR11              0xFFC

#define PRODUCT_ID_QEMU         0x51 /* ASCII code Q */

struct ARMGICv2mState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq spi[GICV2M_NUM_SPI_MAX];

    uint32_t base_spi;
    uint32_t num_spi;

    static void setIrq(void *opaque, int irq)
    {
        ARMGICv2mState *s = static_cast<ARMGICv2mState *>(opaque);

        qemu_irq_pulse(s->spi[irq]);
    }

    static uint64_t read(void *opaque, hwaddr offset,
                         unsigned size)
    {
        ARMGICv2mState *s = static_cast<ARMGICv2mState *>(opaque);
        uint32_t val;

        if (size != 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "gicv2m_read: bad size %u\n", size);
            return 0;
        }

        switch (offset) {
        case V2M_MSI_TYPER:
            val = (s->base_spi + 32) << 16;
            val |= s->num_spi;
            return val;
        case V2M_MSI_IIDR:
            /* We don't have any valid implementor so we leave that field as zero
             * and we return 0 in the arch revision as per the spec.
             */
            return (PRODUCT_ID_QEMU << 20);
        case V2M_IIDR0 ... V2M_IIDR11:
            /* We do not implement any optional identification registers and the
             * mandatory MSI_PIDR2 register reads as 0x0, so we capture all
             * implementation defined registers here.
             */
            return 0;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gicv2m_read: Bad offset %x\n", (int)offset);
            return 0;
        }
    }

    static void write(void *opaque, hwaddr offset,
                      uint64_t value, unsigned size)
    {
        ARMGICv2mState *s = static_cast<ARMGICv2mState *>(opaque);

        if (size != 2 && size != 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "gicv2m_write: bad size %u\n", size);
            return;
        }

        switch (offset) {
        case V2M_MSI_SETSPI_NS: {
            int spi;

            spi = (value & 0x3ff) - (s->base_spi + 32);
            if (spi >= 0 && static_cast<uint32_t>(spi) < s->num_spi) {
                setIrq(s, spi);
            }
            return;
        }
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gicv2m_write: Bad offset %x\n", (int)offset);
        }
    }

    static const MemoryRegionOps ops;

    void realize(Error **errp)
    {
        int i;

        if (num_spi > GICV2M_NUM_SPI_MAX) {
            error_setg(errp,
                       "requested %u SPIs exceeds GICv2m frame maximum %d",
                       num_spi, GICV2M_NUM_SPI_MAX);
            return;
        }

        if (base_spi + 32 > 1020 - num_spi) {
            error_setg(errp,
                       "requested base SPI %u+%u exceeds max. number 1020",
                       base_spi + 32, num_spi);
            return;
        }

        for (i = 0; static_cast<uint32_t>(i) < num_spi; i++) {
            sysbus_init_irq(reinterpret_cast<SysBusDevice *>(this), &spi[i]);
        }

        msi_nonbroken = true;
        kvm_gsi_direct_mapping = true;
        kvm_msi_via_irqfd_allowed = kvm_irqfds_enabled();
    }

    void init()
    {
        memory_region_init_io(&iomem, OBJECT(this), &ops, this,
                              "gicv2m", 0x1000);
        sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);
    }

    static void classInit(DeviceClass *dc);
};

const MemoryRegionOps ARMGICv2mState::ops = {
    .read = ARMGICv2mState::read,
    .write = ARMGICv2mState::write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const Property gicv2m_properties[] = {
    DEFINE_PROP_UINT32("base-spi", ARMGICv2mState, base_spi, 0),
    DEFINE_PROP_UINT32("num-spi", ARMGICv2mState, num_spi, 64),
};

void ARMGICv2mState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, gicv2m_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(ARMGICv2mState, TYPE_ARM_GICV2M, TYPE_SYS_BUS_DEVICE)
