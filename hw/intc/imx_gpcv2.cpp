/*
 * Copyright (c) 2018, Impinj, Inc.
 *
 * i.MX7 GPCv2 block emulation code
 *
 * Author: Andrey Smirnov <andrew.smirnov@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/intc/imx_gpcv2.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define GPC_PU_PGC_SW_PUP_REQ       0x0f8
#define GPC_PU_PGC_SW_PDN_REQ       0x104

#define USB_HSIC_PHY_SW_Pxx_REQ     BIT(4)
#define USB_OTG2_PHY_SW_Pxx_REQ     BIT(3)
#define USB_OTG1_PHY_SW_Pxx_REQ     BIT(2)
#define PCIE_PHY_SW_Pxx_REQ         BIT(1)
#define MIPI_PHY_SW_Pxx_REQ         BIT(0)


void IMXGPCv2State::reset()
{
    memset(regs, 0, sizeof(regs));
}

static uint64_t imx_gpcv2_read(void *opaque, hwaddr offset,
                               unsigned size)
{
    IMXGPCv2State *s = static_cast<IMXGPCv2State *>(opaque);

    return s->regs[offset / sizeof(uint32_t)];
}

static void imx_gpcv2_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    IMXGPCv2State *s = static_cast<IMXGPCv2State *>(opaque);
    const size_t idx = offset / sizeof(uint32_t);

    s->regs[idx] = value;

    /*
     * Real HW will clear those bits once as a way to indicate that
     * power up request is complete
     */
    if (offset == GPC_PU_PGC_SW_PUP_REQ ||
        offset == GPC_PU_PGC_SW_PDN_REQ) {
        s->regs[idx] &= ~(USB_HSIC_PHY_SW_Pxx_REQ |
                          USB_OTG2_PHY_SW_Pxx_REQ |
                          USB_OTG1_PHY_SW_Pxx_REQ |
                          PCIE_PHY_SW_Pxx_REQ     |
                          MIPI_PHY_SW_Pxx_REQ);
    }
}

static const struct MemoryRegionOps imx_gpcv2_ops = {
    .read = imx_gpcv2_read,
    .write = imx_gpcv2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        /*
         * Our device would not work correctly if the guest was doing
         * unaligned access. This might not be a limitation on the real
         * device but in practice there is no reason for a guest to access
         * this device unaligned.
         */
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void IMXGPCv2State::init()
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);

    memory_region_init_io(&iomem,
                          OBJECT(this),
                          &imx_gpcv2_ops,
                          this,
                          TYPE_IMX_GPCV2 ".iomem",
                          sizeof(regs));
    sysbus_init_mmio(sbd, &iomem);
}

static const VMStateDescription vmstate_imx_gpcv2 = {
    .name = TYPE_IMX_GPCV2,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXGPCv2State, GPC_NUM),
        VMSTATE_END_OF_LIST()
    },
};

void IMXGPCv2State::classInit(DeviceClass *dc)
{
    dc->vmsd  = &vmstate_imx_gpcv2;
    dc->desc  = "i.MX GPCv2 Module";
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(IMXGPCv2State, TYPE_IMX_GPCV2, TYPE_SYS_BUS_DEVICE)
