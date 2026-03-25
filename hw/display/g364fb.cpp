/*
 * QEMU G364 framebuffer Emulator.
 *
 * Copyright (c) 2007-2011 Herve Poussineau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/units.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "trace.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"

typedef struct G364State {
    /* hardware */
    uint32_t vram_size;
    qemu_irq irq;
    MemoryRegion mem_vram;
    MemoryRegion mem_ctrl;
    /* registers */
    uint8_t color_palette[256][3];
    uint8_t cursor_palette[3][3];
    uint16_t cursor[512];
    uint32_t cursor_position;
    uint32_t ctla;
    uint32_t top_of_screen;
    uint32_t width, height; /* in pixels */
    /* display refresh support */
    QemuConsole *con;
    int depth;
    int blanked;

    /* methods */
    void drawGraphic8();
    void drawBlank();
    static void updateDisplay(void *opaque);
    static void invalidateDisplay(void *opaque);
    void resetState();
    static uint64_t ctrlRead(void *opaque, hwaddr addr, unsigned int size);
    static void ctrlWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned int size);
    void updateDepth();
    void invalidateCursorPosition();
    static int postLoad(void *opaque, int version_id);
} G364State;

#define REG_BOOT     0x000000
#define REG_DISPLAY  0x000118
#define REG_VDISPLAY 0x000150
#define REG_CTLA     0x000300
#define REG_TOP      0x000400
#define REG_CURS_PAL 0x000508
#define REG_CURS_POS 0x000638
#define REG_CLR_PAL  0x000800
#define REG_CURS_PAT 0x001000
#define REG_RESET    0x100000

#define CTLA_FORCE_BLANK 0x00000400
#define CTLA_NO_CURSOR   0x00800000

#define G364_PAGE_SIZE 4096

static inline int check_dirty(G364State *s, DirtyBitmapSnapshot *snap, ram_addr_t page)
{
    return memory_region_snapshot_get_dirty(&s->mem_vram, snap, page, G364_PAGE_SIZE);
}

void G364State::drawGraphic8()
{
    DisplaySurface *surface = qemu_console_surface(this->con);
    DirtyBitmapSnapshot *snap;
    int i, w;
    uint8_t *vram;
    uint8_t *data_display, *dd;
    ram_addr_t page;
    int x, y;
    int xmin, xmax;
    int ymin, ymax;
    int xcursor, ycursor;
    unsigned int (*rgb_to_pixel)(unsigned int r, unsigned int g, unsigned int b);

    switch (surface_bits_per_pixel(surface)) {
        case 8:
            rgb_to_pixel = rgb_to_pixel8;
            w = 1;
            break;
        case 15:
            rgb_to_pixel = rgb_to_pixel15;
            w = 2;
            break;
        case 16:
            rgb_to_pixel = rgb_to_pixel16;
            w = 2;
            break;
        case 32:
            rgb_to_pixel = rgb_to_pixel32;
            w = 4;
            break;
        default:
            hw_error("g364: unknown host depth %d",
                     surface_bits_per_pixel(surface));
            return;
    }

    page = 0;

    x = y = 0;
    xmin = this->width;
    xmax = 0;
    ymin = this->height;
    ymax = 0;

    if (!(this->ctla & CTLA_NO_CURSOR)) {
        xcursor = this->cursor_position >> 12;
        ycursor = this->cursor_position & 0xfff;
    } else {
        xcursor = ycursor = -65;
    }

    vram = static_cast<uint8_t *>(memory_region_get_ram_ptr(&this->mem_vram)) + this->top_of_screen;
    /* XXX: out of range in vram? */
    data_display = dd = surface_data(surface);
    snap = memory_region_snapshot_and_clear_dirty(&this->mem_vram, 0, this->vram_size,
                                                  DIRTY_MEMORY_VGA);
    while (y < (int)this->height) {
        if (check_dirty(this, snap, page)) {
            if (y < ymin)
                ymin = ymax = y;
            if (x < xmin)
                xmin = x;
            for (i = 0; i < G364_PAGE_SIZE; i++) {
                uint8_t index;
                unsigned int color;
                if (unlikely((y >= ycursor && y < ycursor + 64) &&
                    (x >= xcursor && x < xcursor + 64))) {
                    /* pointer area */
                    int xdiff = x - xcursor;
                    uint16_t curs = this->cursor[(y - ycursor) * 8 + xdiff / 8];
                    int op = (curs >> ((xdiff & 7) * 2)) & 3;
                    if (likely(op == 0)) {
                        /* transparent */
                        index = *vram;
                        color = (*rgb_to_pixel)(
                            this->color_palette[index][0],
                            this->color_palette[index][1],
                            this->color_palette[index][2]);
                    } else {
                        /* get cursor color */
                        index = op - 1;
                        color = (*rgb_to_pixel)(
                            this->cursor_palette[index][0],
                            this->cursor_palette[index][1],
                            this->cursor_palette[index][2]);
                    }
                } else {
                    /* normal area */
                    index = *vram;
                    color = (*rgb_to_pixel)(
                        this->color_palette[index][0],
                        this->color_palette[index][1],
                        this->color_palette[index][2]);
                }
                memcpy(dd, &color, w);
                dd += w;
                x++;
                vram++;
                if (x == (int)this->width) {
                    xmax = this->width - 1;
                    y++;
                    if (y == (int)this->height) {
                        ymax = this->height - 1;
                        goto done;
                    }
                    data_display = dd = data_display + surface_stride(surface);
                    xmin = 0;
                    x = 0;
                }
            }
            if (x > xmax)
                xmax = x;
            if (y > ymax)
                ymax = y;
        } else {
            int dy;
            if (xmax || ymax) {
                dpy_gfx_update(this->con, xmin, ymin,
                               xmax - xmin + 1, ymax - ymin + 1);
                xmin = this->width;
                xmax = 0;
                ymin = this->height;
                ymax = 0;
            }
            x += G364_PAGE_SIZE;
            dy = x / this->width;
            x = x % this->width;
            y += dy;
            vram += G364_PAGE_SIZE;
            data_display += dy * surface_stride(surface);
            dd = data_display + x * w;
        }
        page += G364_PAGE_SIZE;
    }

done:
    if (xmax || ymax) {
        dpy_gfx_update(this->con, xmin, ymin, xmax - xmin + 1, ymax - ymin + 1);
    }
    g_free(snap);
}

void G364State::drawBlank()
{
    DisplaySurface *surface = qemu_console_surface(this->con);
    int i, w;
    uint8_t *d;

    if (this->blanked) {
        /* Screen is already blank. No need to redraw it */
        return;
    }

    w = this->width * surface_bytes_per_pixel(surface);
    d = surface_data(surface);
    for (i = 0; i < (int)this->height; i++) {
        memset(d, 0, w);
        d += surface_stride(surface);
    }

    dpy_gfx_update_full(this->con);
    this->blanked = 1;
}

void G364State::updateDisplay(void *opaque)
{
    G364State *s = static_cast<G364State *>(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);

    qemu_flush_coalesced_mmio_buffer();

    if (s->width == 0 || s->height == 0)
        return;

    if (s->width != (uint32_t)surface_width(surface) ||
        s->height != (uint32_t)surface_height(surface)) {
        qemu_console_resize(s->con, s->width, s->height);
    }

    if (s->ctla & CTLA_FORCE_BLANK) {
        s->drawBlank();
    } else if (s->depth == 8) {
        s->drawGraphic8();
    } else {
        error_report("g364: unknown guest depth %d", s->depth);
    }

    qemu_irq_raise(s->irq);
}

void G364State::invalidateDisplay(void *opaque)
{
    G364State *s = static_cast<G364State *>(opaque);

    s->blanked = 0;
    memory_region_set_dirty(&s->mem_vram, 0, s->vram_size);
}

void G364State::resetState()
{
    uint8_t *vram = static_cast<uint8_t *>(memory_region_get_ram_ptr(&this->mem_vram));

    qemu_irq_lower(this->irq);

    memset(this->color_palette, 0, sizeof(this->color_palette));
    memset(this->cursor_palette, 0, sizeof(this->cursor_palette));
    memset(this->cursor, 0, sizeof(this->cursor));
    this->cursor_position = 0;
    this->ctla = 0;
    this->top_of_screen = 0;
    this->width = this->height = 0;
    memset(vram, 0, this->vram_size);
    G364State::invalidateDisplay(this);
}

/* called for accesses to io ports */
uint64_t G364State::ctrlRead(void *opaque,
                              hwaddr addr,
                              unsigned int size)
{
    G364State *s = static_cast<G364State *>(opaque);
    uint32_t val;

    if (addr >= REG_CURS_PAT && addr < REG_CURS_PAT + 0x1000) {
        /* cursor pattern */
        int idx = (addr - REG_CURS_PAT) >> 3;
        val = s->cursor[idx];
    } else if (addr >= REG_CURS_PAL && addr < REG_CURS_PAL + 0x18) {
        /* cursor palette */
        int idx = (addr - REG_CURS_PAL) >> 3;
        val = ((uint32_t)s->cursor_palette[idx][0] << 16);
        val |= ((uint32_t)s->cursor_palette[idx][1] << 8);
        val |= ((uint32_t)s->cursor_palette[idx][2] << 0);
    } else {
        switch (addr) {
            case REG_DISPLAY:
                val = s->width / 4;
                break;
            case REG_VDISPLAY:
                val = s->height * 2;
                break;
            case REG_CTLA:
                val = s->ctla;
                break;
            default:
            {
                error_report("g364: invalid read at [" HWADDR_FMT_plx "]",
                             addr);
                val = 0;
                break;
            }
        }
    }

    trace_g364fb_read(addr, val);

    return val;
}

void G364State::updateDepth()
{
    static const int depths[8] = { 1, 2, 4, 8, 15, 16, 0 };
    this->depth = depths[(this->ctla & 0x00700000) >> 20];
}

void G364State::invalidateCursorPosition()
{
    DisplaySurface *surface = qemu_console_surface(this->con);
    int ymin, ymax, start, end;

    /* invalidate only near the cursor */
    ymin = this->cursor_position & 0xfff;
    ymax = MIN(this->height, (uint32_t)(ymin + 64));
    start = ymin * surface_stride(surface);
    end = (ymax + 1) * surface_stride(surface);

    memory_region_set_dirty(&this->mem_vram, start, end - start);
}

void G364State::ctrlWrite(void *opaque,
                           hwaddr addr,
                           uint64_t val,
                           unsigned int size)
{
    G364State *s = static_cast<G364State *>(opaque);

    trace_g364fb_write(addr, val);

    if (addr >= REG_CLR_PAL && addr < REG_CLR_PAL + 0x800) {
        /* color palette */
        int idx = (addr - REG_CLR_PAL) >> 3;
        s->color_palette[idx][0] = (val >> 16) & 0xff;
        s->color_palette[idx][1] = (val >> 8) & 0xff;
        s->color_palette[idx][2] = val & 0xff;
        G364State::invalidateDisplay(s);
    } else if (addr >= REG_CURS_PAT && addr < REG_CURS_PAT + 0x1000) {
        /* cursor pattern */
        int idx = (addr - REG_CURS_PAT) >> 3;
        s->cursor[idx] = val;
        G364State::invalidateDisplay(s);
    } else if (addr >= REG_CURS_PAL && addr < REG_CURS_PAL + 0x18) {
        /* cursor palette */
        int idx = (addr - REG_CURS_PAL) >> 3;
        s->cursor_palette[idx][0] = (val >> 16) & 0xff;
        s->cursor_palette[idx][1] = (val >> 8) & 0xff;
        s->cursor_palette[idx][2] = val & 0xff;
        G364State::invalidateDisplay(s);
    } else {
        switch (addr) {
        case REG_BOOT: /* Boot timing */
        case 0x00108: /* Line timing: half sync */
        case 0x00110: /* Line timing: back porch */
        case 0x00120: /* Line timing: short display */
        case 0x00128: /* Frame timing: broad pulse */
        case 0x00130: /* Frame timing: v sync */
        case 0x00138: /* Frame timing: v preequalise */
        case 0x00140: /* Frame timing: v postequalise */
        case 0x00148: /* Frame timing: v blank */
        case 0x00158: /* Line timing: line time */
        case 0x00160: /* Frame store: line start */
        case 0x00168: /* vram cycle: mem init */
        case 0x00170: /* vram cycle: transfer delay */
        case 0x00200: /* vram cycle: mask register */
            /* ignore */
            break;
        case REG_TOP:
            s->top_of_screen = val;
            G364State::invalidateDisplay(s);
            break;
        case REG_DISPLAY:
            s->width = val * 4;
            break;
        case REG_VDISPLAY:
            s->height = val / 2;
            break;
        case REG_CTLA:
            s->ctla = val;
            s->updateDepth();
            G364State::invalidateDisplay(s);
            break;
        case REG_CURS_POS:
            s->invalidateCursorPosition();
            s->cursor_position = val;
            s->invalidateCursorPosition();
            break;
        case REG_RESET:
            s->resetState();
            break;
        default:
            error_report("g364: invalid write of 0x%" PRIx64
                         " at [" HWADDR_FMT_plx "]", val, addr);
            break;
        }
    }
    qemu_irq_lower(s->irq);
}

static const MemoryRegionOps g364fb_ctrl_ops = {
    .read = G364State::ctrlRead,
    .write = G364State::ctrlWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
};

int G364State::postLoad(void *opaque, int version_id)
{
    G364State *s = static_cast<G364State *>(opaque);

    /* force refresh */
    s->updateDepth();
    G364State::invalidateDisplay(s);

    return 0;
}

static const VMStateDescription vmstate_g364fb = {
    .name = "g364fb",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = G364State::postLoad,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(color_palette, G364State, 0, 256 * 3),
        VMSTATE_BUFFER_UNSAFE(cursor_palette, G364State, 0, 9),
        VMSTATE_UINT16_ARRAY(cursor, G364State, 512),
        VMSTATE_UINT32(cursor_position, G364State),
        VMSTATE_UINT32(ctla, G364State),
        VMSTATE_UINT32(top_of_screen, G364State),
        VMSTATE_UINT32(width, G364State),
        VMSTATE_UINT32(height, G364State),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps g364fb_ops = {
    .invalidate  = G364State::invalidateDisplay,
    .gfx_update  = G364State::updateDisplay,
};

static void g364fb_init(DeviceState *dev, G364State *s)
{
    s->con = graphic_console_init(dev, 0, &g364fb_ops, s);

    memory_region_init_io(&s->mem_ctrl, OBJECT(dev), &g364fb_ctrl_ops, s,
                          "ctrl", 0x180000);
    memory_region_init_ram(&s->mem_vram, NULL, "g364fb.vram", s->vram_size,
                           &error_fatal);
    memory_region_set_log(&s->mem_vram, true, DIRTY_MEMORY_VGA);
}

#define TYPE_G364 "sysbus-g364"
OBJECT_DECLARE_SIMPLE_TYPE(G364SysBusState, G364)

struct G364SysBusState {
    SysBusDevice parent_obj;

    G364State g364;

    /* methods */
    void realize(Error **errp);
    void reset();
    static void classInit(ObjectClass *klass, const void *data);
};

static void g364fb_sysbus_realize(DeviceState *dev, Error **errp)
{
    G364SysBusState *sbs = G364(dev);
    sbs->realize(errp);
}

void G364SysBusState::realize(Error **errp)
{
    G364State *s = &this->g364;
    SysBusDevice *sbd = SYS_BUS_DEVICE(DEVICE(this));

    g364fb_init(DEVICE(this), s);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_mmio(sbd, &s->mem_ctrl);
    sysbus_init_mmio(sbd, &s->mem_vram);
}

static void g364fb_sysbus_reset(DeviceState *d)
{
    G364SysBusState *s = G364(d);
    s->reset();
}

void G364SysBusState::reset()
{
    this->g364.resetState();
}

static const Property g364fb_sysbus_properties[] = {
    DEFINE_PROP_UINT32("vram_size", G364SysBusState, g364.vram_size, 8 * MiB),
};

static const VMStateDescription vmstate_g364fb_sysbus = {
    .name = "g364fb-sysbus",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(g364, G364SysBusState, 2, vmstate_g364fb, G364State),
        VMSTATE_END_OF_LIST()
    }
};

void G364SysBusState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g364fb_sysbus_realize;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "G364 framebuffer";
    device_class_set_legacy_reset(dc, g364fb_sysbus_reset);
    dc->vmsd = &vmstate_g364fb_sysbus;
    device_class_set_props(dc, g364fb_sysbus_properties);
}

static const TypeInfo g364fb_sysbus_info = {
    .name          = TYPE_G364,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G364SysBusState),
    .class_init    = G364SysBusState::classInit,
};

static void g364fb_register_types(void)
{
    type_register_static(&g364fb_sysbus_info);
}

type_init(g364fb_register_types)
