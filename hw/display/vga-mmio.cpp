/*
 * QEMU MMIO VGA Emulator.
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/display/vga.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "vga_int.h"

/*
 * QEMU interface:
 *  + sysbus MMIO region 0: VGA I/O registers
 *  + sysbus MMIO region 1: VGA MMIO registers
 *  + sysbus MMIO region 2: VGA memory
 */

OBJECT_DECLARE_SIMPLE_TYPE(VGAMmioState, VGA_MMIO)

struct VGAMmioState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    VGACommonState vga;
    MemoryRegion iomem;
    MemoryRegion lowmem;

    uint8_t it_shift;

    /* methods */
    static uint64_t mmRead(void *opaque, hwaddr addr, unsigned size);
    static void mmWrite(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size);
    void realize(Error **errp);
    void reset();
    static void classInit(DeviceClass *dc);
};

uint64_t VGAMmioState::mmRead(void *opaque, hwaddr addr, unsigned size)
{
    VGAMmioState *s = static_cast<VGAMmioState *>(opaque);

    return vga_ioport_read(&s->vga, addr >> s->it_shift) &
        MAKE_64BIT_MASK(0, size * 8);
}

void VGAMmioState::mmWrite(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    VGAMmioState *s = static_cast<VGAMmioState *>(opaque);

    vga_ioport_write(&s->vga, addr >> s->it_shift,
                     value & MAKE_64BIT_MASK(0, size * 8));
}

static const MemoryRegionOps vga_mm_ctrl_ops = {
    .read = VGAMmioState::mmRead,
    .write = VGAMmioState::mmWrite,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
    .impl = { .min_access_size = 1, .max_access_size = 4, },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void VGAMmioState::reset()
{
    vga_common_reset(&this->vga);
}

void VGAMmioState::realize(Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(reinterpret_cast<DeviceState *>(this));

    memory_region_init_io(&this->iomem, reinterpret_cast<Object *>(this), &vga_mm_ctrl_ops, this,
                          "vga-mmio", 0x100000);
    memory_region_set_flush_coalesced(&this->iomem);
    sysbus_init_mmio(sbd, &this->iomem);

    /* XXX: endianness? */
    memory_region_init_io(&this->lowmem, reinterpret_cast<Object *>(this), &vga_mem_ops, &this->vga,
                          "vga-lowmem", 0x20000);
    memory_region_set_coalescing(&this->lowmem);
    sysbus_init_mmio(sbd, &this->lowmem);

    this->vga.bank_offset = 0;
    this->vga.global_vmstate = true;
    if (!vga_common_init(&this->vga, reinterpret_cast<Object *>(this), errp)) {
        return;
    }

    sysbus_init_mmio(sbd, &this->vga.vram);
    this->vga.con = graphic_console_init(reinterpret_cast<DeviceState *>(this), 0, this->vga.hw_ops, &this->vga);
}

static const Property vga_mmio_properties[] = {
    DEFINE_PROP_UINT8("it_shift", VGAMmioState, it_shift, 0),
    DEFINE_PROP_UINT32("vgamem_mb", VGAMmioState, vga.vram_size_mb, 8),
};

void VGAMmioState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_vga_common;
    device_class_set_props(dc, vga_mmio_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VGAMmioState, TYPE_VGA_MMIO, TYPE_SYS_BUS_DEVICE)
