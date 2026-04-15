/*
 * Copyright (c) 2025 Bernhard Beschow <shentey@gmail.com>
 *
 * i.MX 8M Plus ANALOG IP block emulation code
 *
 * Based on hw/misc/imx7_ccm.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qemu/log.h"
#include "hw/misc/imx8mp_analog.h"
#include "migration/vmstate.h"
#include "qom/cpp/object.h"

#define ANALOG_PLL_LOCK BIT(31)

void IMX8MPAnalogState::reset()
{

    memset(analog, 0, sizeof(analog));

    analog[ANALOG_AUDIO_PLL1_GEN_CTRL] = 0x00002010;
    analog[ANALOG_AUDIO_PLL1_FDIV_CTL0] = 0x00145032;
    analog[ANALOG_AUDIO_PLL1_FDIV_CTL1] = 0x00000000;
    analog[ANALOG_AUDIO_PLL1_SSCG_CTRL] = 0x00000000;
    analog[ANALOG_AUDIO_PLL1_MNIT_CTRL] = 0x00100103;
    analog[ANALOG_AUDIO_PLL2_GEN_CTRL] = 0x00002010;
    analog[ANALOG_AUDIO_PLL2_FDIV_CTL0] = 0x00145032;
    analog[ANALOG_AUDIO_PLL2_FDIV_CTL1] = 0x00000000;
    analog[ANALOG_AUDIO_PLL2_SSCG_CTRL] = 0x00000000;
    analog[ANALOG_AUDIO_PLL2_MNIT_CTRL] = 0x00100103;
    analog[ANALOG_VIDEO_PLL1_GEN_CTRL] = 0x00002010;
    analog[ANALOG_VIDEO_PLL1_FDIV_CTL0] = 0x00145032;
    analog[ANALOG_VIDEO_PLL1_FDIV_CTL1] = 0x00000000;
    analog[ANALOG_VIDEO_PLL1_SSCG_CTRL] = 0x00000000;
    analog[ANALOG_VIDEO_PLL1_MNIT_CTRL] = 0x00100103;
    analog[ANALOG_DRAM_PLL_GEN_CTRL] = 0x00002010;
    analog[ANALOG_DRAM_PLL_FDIV_CTL0] = 0x0012c032;
    analog[ANALOG_DRAM_PLL_FDIV_CTL1] = 0x00000000;
    analog[ANALOG_DRAM_PLL_SSCG_CTRL] = 0x00000000;
    analog[ANALOG_DRAM_PLL_MNIT_CTRL] = 0x00100103;
    analog[ANALOG_GPU_PLL_GEN_CTRL] = 0x00000810;
    analog[ANALOG_GPU_PLL_FDIV_CTL0] = 0x000c8031;
    analog[ANALOG_GPU_PLL_LOCKD_CTRL] = 0x0010003f;
    analog[ANALOG_GPU_PLL_MNIT_CTRL] = 0x00280081;
    analog[ANALOG_VPU_PLL_GEN_CTRL] = 0x00000810;
    analog[ANALOG_VPU_PLL_FDIV_CTL0] = 0x0012c032;
    analog[ANALOG_VPU_PLL_LOCKD_CTRL] = 0x0010003f;
    analog[ANALOG_VPU_PLL_MNIT_CTRL] = 0x00280081;
    analog[ANALOG_ARM_PLL_GEN_CTRL] = 0x00000810;
    analog[ANALOG_ARM_PLL_FDIV_CTL0] = 0x000fa031;
    analog[ANALOG_ARM_PLL_LOCKD_CTRL] = 0x0010003f;
    analog[ANALOG_ARM_PLL_MNIT_CTRL] = 0x00280081;
    analog[ANALOG_SYS_PLL1_GEN_CTRL] = 0x0aaaa810;
    analog[ANALOG_SYS_PLL1_FDIV_CTL0] = 0x00190032;
    analog[ANALOG_SYS_PLL1_LOCKD_CTRL] = 0x0010003f;
    analog[ANALOG_SYS_PLL1_MNIT_CTRL] = 0x00280081;
    analog[ANALOG_SYS_PLL2_GEN_CTRL] = 0x0aaaa810;
    analog[ANALOG_SYS_PLL2_FDIV_CTL0] = 0x000fa031;
    analog[ANALOG_SYS_PLL2_LOCKD_CTRL] = 0x0010003f;
    analog[ANALOG_SYS_PLL2_MNIT_CTRL] = 0x00280081;
    analog[ANALOG_SYS_PLL3_GEN_CTRL] = 0x00000810;
    analog[ANALOG_SYS_PLL3_FDIV_CTL0] = 0x000fa031;
    analog[ANALOG_SYS_PLL3_LOCKD_CTRL] = 0x0010003f;
    analog[ANALOG_SYS_PLL3_MNIT_CTRL] = 0x00280081;
    analog[ANALOG_OSC_MISC_CFG] = 0x00000000;
    analog[ANALOG_ANAMIX_PLL_MNIT_CTL] = 0x00000000;
    analog[ANALOG_DIGPROG] = 0x00824010;

    /* all PLLs need to be locked */
    analog[ANALOG_AUDIO_PLL1_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_AUDIO_PLL2_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_VIDEO_PLL1_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_DRAM_PLL_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_GPU_PLL_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_VPU_PLL_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_ARM_PLL_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_SYS_PLL1_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_SYS_PLL2_GEN_CTRL] |= ANALOG_PLL_LOCK;
    analog[ANALOG_SYS_PLL3_GEN_CTRL] |= ANALOG_PLL_LOCK;
}

static uint64_t imx8mp_analog_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MPAnalogState *s = static_cast<IMX8MPAnalogState *>(opaque);

    return s->analog[offset >> 2];
}

static void imx8mp_analog_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMX8MPAnalogState *s = static_cast<IMX8MPAnalogState *>(opaque);

    if (offset >> 2 == ANALOG_DIGPROG) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Guest write to read-only ANALOG_DIGPROG register\n");
    } else {
        s->analog[offset >> 2] = value;
    }
}

static MemoryRegionOps imx8mp_analog_ops;

static void __attribute__((constructor)) init_imx8mp_analog_ops(void)
{
    memset(&imx8mp_analog_ops, 0, sizeof(imx8mp_analog_ops));
    imx8mp_analog_ops.read = imx8mp_analog_read;
    imx8mp_analog_ops.write = imx8mp_analog_write;
    imx8mp_analog_ops.endianness = DEVICE_NATIVE_ENDIAN;
    imx8mp_analog_ops.impl.min_access_size = 4;
    imx8mp_analog_ops.impl.max_access_size = 4;
    imx8mp_analog_ops.impl.unaligned = false;
}

void IMX8MPAnalogState::init()
{
    Object *obj = reinterpret_cast<Object *>(this);
    SysBusDevice *sd = reinterpret_cast<SysBusDevice *>(this);

    memory_region_init(&mmio.container, obj, TYPE_IMX8MP_ANALOG, 0x10000);

    memory_region_init_io(&mmio.analog, obj, &imx8mp_analog_ops, this,
                          TYPE_IMX8MP_ANALOG, sizeof(analog));
    memory_region_add_subregion(&mmio.container, 0, &mmio.analog);

    sysbus_init_mmio(sd, &mmio.container);
}

static const VMStateField vmstate_imx8mp_analog_fields[] = {
    VMSTATE_UINT32_ARRAY(analog, IMX8MPAnalogState, ANALOG_MAX),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription imx8mp_analog_vmstate = {
    .name = TYPE_IMX8MP_ANALOG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_imx8mp_analog_fields,
};

void IMX8MPAnalogState::classInit(DeviceClass *dc)
{
    dc->vmsd  = &imx8mp_analog_vmstate;
    dc->desc  = "i.MX 8M Plus Analog Module";
}

REGISTER_QEMU_DEVICE(IMX8MPAnalogState, TYPE_IMX8MP_ANALOG,
                     TYPE_SYS_BUS_DEVICE)
