/*
 * QEMU ISA VGA Emulator.
 *
 * see docs/specs/standard-vga.rst for virtual hardware specs.
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

#include "hw/isa/isa.h"
#include "vga_int.h"
#include "ui/pixel_ops.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_ISA_VGA "isa-vga"
OBJECT_DECLARE_SIMPLE_TYPE(ISAVGAState, ISA_VGA)

struct ISAVGAState {
    ISADevice parent_obj;

    struct VGACommonState state;
    PortioList portio_vga;
    PortioList portio_vbe;

    /* Instance methods */
    void reset();
    void realize(Error **errp);

    /* Class methods */
    static void classInit(DeviceClass *dc);
};

void ISAVGAState::reset()
{
    vga_common_reset(&state);
}

void ISAVGAState::realize(Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(reinterpret_cast<DeviceState *>(this));
    VGACommonState *s = &state;
    MemoryRegion *vga_io_memory;
    const MemoryRegionPortio *vga_ports, *vbe_ports;

    s->global_vmstate = true;
    if (!vga_common_init(s, reinterpret_cast<Object *>(this), errp)) {
        return;
    }

    s->legacy_address_space = isa_address_space(isadev);
    vga_io_memory = vga_init_io(s, reinterpret_cast<Object *>(this), &vga_ports, &vbe_ports);
    isa_register_portio_list(isadev, &portio_vga,
                             0x3b0, vga_ports, s, "vga");
    if (vbe_ports) {
        isa_register_portio_list(isadev, &portio_vbe,
                                 0x1ce, vbe_ports, s, "vbe");
    }
    memory_region_add_subregion_overlap(isa_address_space(isadev),
                                        0x000a0000,
                                        vga_io_memory, 1);
    memory_region_set_coalescing(vga_io_memory);
    s->con = graphic_console_init(reinterpret_cast<DeviceState *>(this), 0, s->hw_ops, s);

    memory_region_add_subregion(isa_address_space(isadev),
                                VBE_DISPI_LFB_PHYSICAL_ADDRESS,
                                &s->vram);
    /* ROM BIOS */
    rom_add_vga(VGABIOS_FILENAME);
}

static const Property vga_isa_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", ISAVGAState, state.vram_size_mb, 8),
};

void ISAVGAState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_vga_common;
    device_class_set_props(dc, vga_isa_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(ISAVGAState, TYPE_ISA_VGA, TYPE_ISA_DEVICE)
