/*
 * Copyright (c) 2018, Impinj, Inc.
 *
 * i.MX7 CCM, PMU and ANALOG IP blocks emulation code
 *
 * Author: Andrey Smirnov <andrew.smirnov@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"

#include "hw/misc/imx7_ccm.h"
#include "migration/vmstate.h"
#include "qom/cpp/object.h"

#include "trace.h"

#define CKIH_FREQ 24000000 /* 24MHz crystal input */

void IMX7AnalogState::reset()
{
    memset(pmu, 0, sizeof(pmu));
    memset(analog, 0, sizeof(analog));

    analog[ANALOG_PLL_ARM]         = 0x00002042;
    analog[ANALOG_PLL_DDR]         = 0x0060302c;
    analog[ANALOG_PLL_DDR_SS]      = 0x00000000;
    analog[ANALOG_PLL_DDR_NUM]     = 0x06aaac4d;
    analog[ANALOG_PLL_DDR_DENOM]   = 0x100003ec;
    analog[ANALOG_PLL_480]         = 0x00002000;
    analog[ANALOG_PLL_480A]        = 0x52605a56;
    analog[ANALOG_PLL_480B]        = 0x52525216;
    analog[ANALOG_PLL_ENET]        = 0x00001fc0;
    analog[ANALOG_PLL_AUDIO]       = 0x0001301b;
    analog[ANALOG_PLL_AUDIO_SS]    = 0x00000000;
    analog[ANALOG_PLL_AUDIO_NUM]   = 0x05f5e100;
    analog[ANALOG_PLL_AUDIO_DENOM] = 0x2964619c;
    analog[ANALOG_PLL_VIDEO]       = 0x0008201b;
    analog[ANALOG_PLL_VIDEO_SS]    = 0x00000000;
    analog[ANALOG_PLL_VIDEO_NUM]   = 0x0000f699;
    analog[ANALOG_PLL_VIDEO_DENOM] = 0x000f4240;
    analog[ANALOG_PLL_MISC0]       = 0x00000000;

    /* all PLLs need to be locked */
    analog[ANALOG_PLL_ARM]   |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_DDR]   |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_480]   |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_480A]  |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_480B]  |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_ENET]  |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_AUDIO] |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_VIDEO] |= ANALOG_PLL_LOCK;
    analog[ANALOG_PLL_MISC0] |= ANALOG_PLL_LOCK;

    /* See original source for register value rationale */
    analog[ANALOG_DIGPROG]  = 0x720000;
    analog[ANALOG_DIGPROG] |= 0x000010;
}

void IMX7CCMState::reset()
{
    memset(ccm, 0, sizeof(ccm));
}

#define CCM_INDEX(offset)   (((offset) & ~(hwaddr)0xF) / sizeof(uint32_t))
#define CCM_BITOP(offset)   ((offset) & (hwaddr)0xF)

enum {
    CCM_BITOP_NONE = 0x00,
    CCM_BITOP_SET  = 0x04,
    CCM_BITOP_CLR  = 0x08,
    CCM_BITOP_TOG  = 0x0C,
};

static uint64_t imx7_set_clr_tog_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    const uint32_t *mmio = static_cast<const uint32_t *>(opaque);

    return mmio[CCM_INDEX(offset)];
}

static void imx7_set_clr_tog_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    const uint8_t  bitop = CCM_BITOP(offset);
    const uint32_t index = CCM_INDEX(offset);
    uint32_t *mmio = static_cast<uint32_t *>(opaque);

    switch (bitop) {
    case CCM_BITOP_NONE:
        mmio[index]  = value;
        break;
    case CCM_BITOP_SET:
        mmio[index] |= value;
        break;
    case CCM_BITOP_CLR:
        mmio[index] &= ~value;
        break;
    case CCM_BITOP_TOG:
        mmio[index] ^= value;
        break;
    };
}

static const struct MemoryRegionOps imx7_set_clr_tog_ops = {
    .read = imx7_set_clr_tog_read,
    .write = imx7_set_clr_tog_write,
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

static void imx7_digprog_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Guest write to read-only ANALOG_DIGPROG register\n");
}

static const struct MemoryRegionOps imx7_digprog_ops = {
    .read = imx7_set_clr_tog_read,
    .write = imx7_digprog_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void IMX7CCMState::init()
{
    Object *obj = reinterpret_cast<Object *>(this);
    memory_region_init_io(&iomem, obj, &imx7_set_clr_tog_ops, ccm,
                          TYPE_IMX7_CCM ".ccm", sizeof(ccm));
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

void IMX7AnalogState::init()
{
    Object *obj = reinterpret_cast<Object *>(this);
    SysBusDevice *sd = reinterpret_cast<SysBusDevice *>(this);

    memory_region_init(&mmio.container, obj, TYPE_IMX7_ANALOG, 0x10000);

    memory_region_init_io(&mmio.analog, obj, &imx7_set_clr_tog_ops,
                          analog, TYPE_IMX7_ANALOG, sizeof(analog));
    memory_region_add_subregion(&mmio.container, 0x60, &mmio.analog);

    memory_region_init_io(&mmio.pmu, obj, &imx7_set_clr_tog_ops, pmu,
                          TYPE_IMX7_ANALOG ".pmu", sizeof(pmu));
    memory_region_add_subregion(&mmio.container, 0x200, &mmio.pmu);

    memory_region_init_io(&mmio.digprog, obj, &imx7_digprog_ops,
                          &analog[ANALOG_DIGPROG],
                          TYPE_IMX7_ANALOG ".digprog", sizeof(uint32_t));
    memory_region_add_subregion_overlap(&mmio.container, 0x800,
                                        &mmio.digprog, 10);

    sysbus_init_mmio(sd, &mmio.container);
}

static const VMStateDescription vmstate_imx7_ccm = {
    .name = TYPE_IMX7_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ccm, IMX7CCMState, CCM_MAX),
        VMSTATE_END_OF_LIST()
    },
};

static uint32_t imx7_ccm_get_clock_frequency(IMXCCMState *dev, IMXClk clock)
{
    /*
     * This function is "consumed" by GPT emulation code. Some clocks
     * have fixed frequencies and we can provide requested frequency
     * easily. However for CCM provided clocks (like IPG) each GPT
     * timer can have its own clock root.
     * This means we need additional information when calling this
     * function to know the requester's identity.
     */
    uint32_t freq = 0;

    switch (clock) {
    case CLK_NONE:
        break;
    case CLK_32k:
        freq = CKIL_FREQ;
        break;
    case CLK_HIGH:
        freq = CKIH_FREQ;
        break;
    case CLK_IPG:
    case CLK_IPG_HIGH:
        /*
         * For now we don't have a way to figure out the device this
         * function is called for. Until then the IPG derived clocks
         * are left unimplemented.
         */
        qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Clock %d Not implemented\n",
                      TYPE_IMX7_CCM, __func__, clock);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: unsupported clock %d\n",
                      TYPE_IMX7_CCM, __func__, clock);
        break;
    }

    trace_ccm_clock_freq(clock, freq);

    return freq;
}

void IMX7CCMState::classInit(DeviceClass *dc)
{
    IMXCCMClass *ccm_class = reinterpret_cast<IMXCCMClass *>(dc);

    dc->vmsd  = &vmstate_imx7_ccm;
    dc->desc  = "i.MX7 Clock Control Module";

    ccm_class->get_clock_frequency = imx7_ccm_get_clock_frequency;
}

REGISTER_QEMU_DEVICE(IMX7CCMState, TYPE_IMX7_CCM, TYPE_IMX_CCM)

static const VMStateDescription vmstate_imx7_analog = {
    .name = TYPE_IMX7_ANALOG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(analog, IMX7AnalogState, ANALOG_MAX),
        VMSTATE_UINT32_ARRAY(pmu,    IMX7AnalogState, PMU_MAX),
        VMSTATE_END_OF_LIST()
    },
};

void IMX7AnalogState::classInit(DeviceClass *dc)
{
    dc->vmsd  = &vmstate_imx7_analog;
    dc->desc  = "i.MX7 Analog Module";
}

REGISTER_QEMU_DEVICE(IMX7AnalogState, TYPE_IMX7_ANALOG, TYPE_SYS_BUS_DEVICE)
