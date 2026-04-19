/*
 * QEMU Cirrus CLGD 54xx VGA Emulator, ISA bus support
 *
 * Copyright (c) 2004 Fabrice Bellard
 * Copyright (c) 2004 Makoto Suzuki (suzu)
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
#include "qemu/module.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/isa/isa.h"
#include "cirrus_vga_internal.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_ISA_CIRRUS_VGA "isa-cirrus-vga"
OBJECT_DECLARE_SIMPLE_TYPE(ISACirrusVGAState, ISA_CIRRUS_VGA)

struct ISACirrusVGAState {
    ISADevice parent_obj;

    CirrusVGAState cirrus_vga;

    /* methods */
    void realize(Error **errp);
    static void classInit(DeviceClass *dc);
};

void ISACirrusVGAState::realize(Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(reinterpret_cast<DeviceState *>(this));
    VGACommonState *s = &this->cirrus_vga.vga;

    /* follow real hardware, cirrus card emulated has 4 MB video memory.
       Also accept 8 MB/16 MB for backward compatibility. */
    if (s->vram_size_mb != 4 && s->vram_size_mb != 8 &&
        s->vram_size_mb != 16) {
        error_setg(errp, "Invalid cirrus_vga ram size '%u'",
                   s->vram_size_mb);
        return;
    }
    s->global_vmstate = true;
    if (!vga_common_init(s, reinterpret_cast<Object *>(this), errp)) {
        return;
    }
    cirrus_init_common(&this->cirrus_vga, reinterpret_cast<Object *>(this), CIRRUS_ID_CLGD5430, 0,
                       isa_address_space(isadev),
                       isa_address_space_io(isadev));
    s->con = graphic_console_init(reinterpret_cast<DeviceState *>(this), 0, s->hw_ops, s);
    rom_add_vga(VGABIOS_CIRRUS_FILENAME);
    /* XXX ISA-LFB support */
    /* FIXME not qdev yet */
}

static const Property isa_cirrus_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", struct ISACirrusVGAState,
                       cirrus_vga.vga.vram_size_mb, 4),
    DEFINE_PROP_BOOL("blitter", struct ISACirrusVGAState,
                     cirrus_vga.enable_blitter, true),
};

void ISACirrusVGAState::classInit(DeviceClass *dc)
{
    dc->vmsd  = &vmstate_cirrus_vga;
    device_class_set_props(dc, isa_cirrus_vga_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(ISACirrusVGAState, TYPE_ISA_CIRRUS_VGA, TYPE_ISA_DEVICE)
