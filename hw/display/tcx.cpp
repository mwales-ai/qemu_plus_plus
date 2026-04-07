/*
 * QEMU TCX Frame buffer
 *
 * Copyright (c) 2003-2005 Fabrice Bellard
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
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TCX_ROM_FILE "QEMU,tcx.bin"
#define FCODE_MAX_ROM_SIZE 0x10000

#define MAXX 1024
#define MAXY 768
#define TCX_DAC_NREGS    16
#define TCX_THC_NREGS    0x1000
#define TCX_DHC_NREGS    0x4000
#define TCX_TEC_NREGS    0x1000
#define TCX_ALT_NREGS    0x8000
#define TCX_STIP_NREGS   0x800000
#define TCX_BLIT_NREGS   0x800000
#define TCX_RSTIP_NREGS  0x800000
#define TCX_RBLIT_NREGS  0x800000

#define TCX_THC_MISC     0x818
#define TCX_THC_CURSXY   0x8fc
#define TCX_THC_CURSMASK 0x900
#define TCX_THC_CURSBITS 0x980

#define TYPE_TCX "sun-tcx"
OBJECT_DECLARE_SIMPLE_TYPE(TCXState, TCX)

struct TCXState {
    SysBusDevice parent_obj;

    QemuConsole *con;
    qemu_irq irq;
    uint8_t *vram;
    uint32_t *vram24, *cplane;
    hwaddr prom_addr;
    MemoryRegion rom;
    MemoryRegion vram_mem;
    MemoryRegion vram_8bit;
    MemoryRegion vram_24bit;
    MemoryRegion stip;
    MemoryRegion blit;
    MemoryRegion vram_cplane;
    MemoryRegion rstip;
    MemoryRegion rblit;
    MemoryRegion tec;
    MemoryRegion dac;
    MemoryRegion thc;
    MemoryRegion dhc;
    MemoryRegion alt;
    MemoryRegion thc24;

    ram_addr_t vram24_offset, cplane_offset;
    uint32_t tmpblit;
    uint32_t vram_size;
    uint32_t palette[260];
    uint8_t r[260], g[260], b[260];
    uint16_t width, height, depth;
    uint8_t dac_index, dac_state;
    uint32_t thcmisc;
    uint32_t cursmask[32];
    uint32_t cursbits[32];
    uint16_t cursx;
    uint16_t cursy;

    /* Instance methods */
    void doReset();
    void realize(DeviceState *dev, Error **errp);
    void initfn(Object *obj);

    /* Static display callbacks */
    static void updateDisplay(void *opaque);
    static void update24Display(void *opaque);
    static void invalidateDisplay(void *opaque);
    static void invalidate24Display(void *opaque);

    /* Static MMIO callbacks */
    static uint64_t dacReadl(void *opaque, hwaddr addr, unsigned size);
    static void dacWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t stipReadl(void *opaque, hwaddr addr, unsigned size);
    static void stipWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static void rstipWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t blitReadl(void *opaque, hwaddr addr, unsigned size);
    static void blitWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static void rblitWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t thcReadl(void *opaque, hwaddr addr, unsigned size);
    static void thcWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t dummyReadl(void *opaque, hwaddr addr, unsigned size);
    static void dummyWritel(void *opaque, hwaddr addr, uint64_t val, unsigned size);

    /* Static VMState callback */
    static int postLoad(void *opaque, int version_id);

    /* Class init */
    static void classInit(ObjectClass *klass, const void *data);

private:
    void setDirty(ram_addr_t addr, int len);
    int checkDirty(DirtyBitmapSnapshot *snap, ram_addr_t addr, int len);
    void updatePaletteEntries(int start, int end);
    void drawLine32(uint8_t *d, const uint8_t *s, int width);
    void drawCursor32(uint8_t *d, int y, int width);
    void draw24Line32(uint8_t *d, const uint8_t *s, int width,
                      const uint32_t *cplane, const uint32_t *s24);
    void invalidateCursorPosition();
};

void TCXState::setDirty(ram_addr_t addr, int len)
{
    memory_region_set_dirty(&vram_mem, addr, len);

    if (depth == 24) {
        memory_region_set_dirty(&vram_mem, vram24_offset + addr * 4,
                                len * 4);
        memory_region_set_dirty(&vram_mem, cplane_offset + addr * 4,
                                len * 4);
    }
}

int TCXState::checkDirty(DirtyBitmapSnapshot *snap,
                         ram_addr_t addr, int len)
{
    int ret;

    ret = memory_region_snapshot_get_dirty(&vram_mem, snap, addr, len);

    if (depth == 24) {
        ret |= memory_region_snapshot_get_dirty(&vram_mem, snap,
                                       vram24_offset + addr * 4, len * 4);
        ret |= memory_region_snapshot_get_dirty(&vram_mem, snap,
                                       cplane_offset + addr * 4, len * 4);
    }

    return ret;
}

void TCXState::updatePaletteEntries(int start, int end)
{
    int i;

    for (i = start; i < end; i++) {
        palette[i] = rgb_to_pixel32(r[i], g[i], b[i]);
    }
    setDirty(0, memory_region_size(&vram_mem));
}

void TCXState::drawLine32(uint8_t *d, const uint8_t *s, int width)
{
    int x;
    uint8_t val;
    uint32_t *p = (uint32_t *)d;

    for (x = 0; x < width; x++) {
        val = *s++;
        *p++ = palette[val];
    }
}

void TCXState::drawCursor32(uint8_t *d, int y, int width)
{
    int x, len;
    uint32_t mask, bits;
    uint32_t *p = (uint32_t *)d;

    y = y - cursy;
    mask = cursmask[y];
    bits = cursbits[y];
    len = MIN(width - cursx, 32);
    p = &p[cursx];
    for (x = 0; x < len; x++) {
        if (mask & 0x80000000) {
            if (bits & 0x80000000) {
                *p = palette[259];
            } else {
                *p = palette[258];
            }
        }
        p++;
        mask <<= 1;
        bits <<= 1;
    }
}

/*
 * XXX Could be much more optimal:
 * detect if line/page/whole screen is in 24 bit mode
 */
void TCXState::draw24Line32(uint8_t *d, const uint8_t *s, int width,
                            const uint32_t *cpl, const uint32_t *s24)
{
    int x, rv, gv, bv;
    uint8_t val, *p8;
    uint32_t *p = (uint32_t *)d;
    uint32_t dval;
    for(x = 0; x < width; x++, s++, s24++) {
        if (be32_to_cpu(*cpl) & 0x03000000) {
            /* 24-bit direct, BGR order */
            p8 = (uint8_t *)s24;
            p8++;
            bv = *p8++;
            gv = *p8++;
            rv = *p8;
            dval = rgb_to_pixel32(rv, gv, bv);
        } else {
            /* 8-bit pseudocolor */
            val = *s;
            dval = palette[val];
        }
        *p++ = dval;
        cpl++;
    }
}

/* Fixed line length 1024 allows us to do nice tricks not possible on
   VGA... */

void TCXState::updateDisplay(void *opaque)
{
    TCXState *ts = static_cast<TCXState *>(opaque);
    DisplaySurface *surface = qemu_console_surface(ts->con);
    ram_addr_t page;
    DirtyBitmapSnapshot *snap = NULL;
    int y, y_start, dd, ds;
    uint8_t *d, *s;

    assert(surface_bits_per_pixel(surface) == 32);

    page = 0;
    y_start = -1;
    d = static_cast<uint8_t *>(surface_data(surface));
    s = ts->vram;
    dd = surface_stride(surface);
    ds = 1024;

    snap = memory_region_snapshot_and_clear_dirty(&ts->vram_mem, 0x0,
                                             memory_region_size(&ts->vram_mem),
                                             DIRTY_MEMORY_VGA);

    for (y = 0; y < ts->height; y++, page += ds) {
        if (ts->checkDirty(snap, page, ds)) {
            if (y_start < 0)
                y_start = y;

            ts->drawLine32(d, s, ts->width);
            if (y >= ts->cursy && y < ts->cursy + 32 && ts->cursx < ts->width) {
                ts->drawCursor32(d, y, ts->width);
            }
        } else {
            if (y_start >= 0) {
                /* flush to display */
                dpy_gfx_update(ts->con, 0, y_start,
                               ts->width, y - y_start);
                y_start = -1;
            }
        }
        s += ds;
        d += dd;
    }
    if (y_start >= 0) {
        /* flush to display */
        dpy_gfx_update(ts->con, 0, y_start,
                       ts->width, y - y_start);
    }
    g_free(snap);
}

void TCXState::update24Display(void *opaque)
{
    TCXState *ts = static_cast<TCXState *>(opaque);
    DisplaySurface *surface = qemu_console_surface(ts->con);
    ram_addr_t page;
    DirtyBitmapSnapshot *snap = NULL;
    int y, y_start, dd, ds;
    uint8_t *d, *s;
    uint32_t *cptr, *s24;

    assert(surface_bits_per_pixel(surface) == 32);

    page = 0;
    y_start = -1;
    d = static_cast<uint8_t *>(surface_data(surface));
    s = ts->vram;
    s24 = ts->vram24;
    cptr = ts->cplane;
    dd = surface_stride(surface);
    ds = 1024;

    snap = memory_region_snapshot_and_clear_dirty(&ts->vram_mem, 0x0,
                                             memory_region_size(&ts->vram_mem),
                                             DIRTY_MEMORY_VGA);

    for (y = 0; y < ts->height; y++, page += ds) {
        if (ts->checkDirty(snap, page, ds)) {
            if (y_start < 0)
                y_start = y;

            ts->draw24Line32(d, s, ts->width, cptr, s24);
            if (y >= ts->cursy && y < ts->cursy+32 && ts->cursx < ts->width) {
                ts->drawCursor32(d, y, ts->width);
            }
        } else {
            if (y_start >= 0) {
                /* flush to display */
                dpy_gfx_update(ts->con, 0, y_start,
                               ts->width, y - y_start);
                y_start = -1;
            }
        }
        d += dd;
        s += ds;
        cptr += ds;
        s24 += ds;
    }
    if (y_start >= 0) {
        /* flush to display */
        dpy_gfx_update(ts->con, 0, y_start,
                       ts->width, y - y_start);
    }
    g_free(snap);
}

void TCXState::invalidateDisplay(void *opaque)
{
    TCXState *s = static_cast<TCXState *>(opaque);

    s->setDirty(0, memory_region_size(&s->vram_mem));
    qemu_console_resize(s->con, s->width, s->height);
}

void TCXState::invalidate24Display(void *opaque)
{
    TCXState *s = static_cast<TCXState *>(opaque);

    s->setDirty(0, memory_region_size(&s->vram_mem));
    qemu_console_resize(s->con, s->width, s->height);
}

int TCXState::postLoad(void *opaque, int version_id)
{
    TCXState *s = static_cast<TCXState *>(opaque);

    s->updatePaletteEntries(0, 256);
    s->setDirty(0, memory_region_size(&s->vram_mem));
    return 0;
}

static const VMStateDescription vmstate_tcx = {
    .name ="tcx",
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = TCXState::postLoad,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(height, TCXState),
        VMSTATE_UINT16(width, TCXState),
        VMSTATE_UINT16(depth, TCXState),
        VMSTATE_BUFFER(r, TCXState),
        VMSTATE_BUFFER(g, TCXState),
        VMSTATE_BUFFER(b, TCXState),
        VMSTATE_UINT8(dac_index, TCXState),
        VMSTATE_UINT8(dac_state, TCXState),
        VMSTATE_END_OF_LIST()
    }
};

void TCXState::doReset()
{
    /* Initialize palette */
    memset(r, 0, 260);
    memset(g, 0, 260);
    memset(b, 0, 260);
    r[255] = g[255] = b[255] = 255;
    r[256] = g[256] = b[256] = 255;
    r[258] = g[258] = b[258] = 255;
    updatePaletteEntries(0, 260);
    memset(vram, 0, MAXX*MAXY);
    memory_region_reset_dirty(&vram_mem, 0, MAXX * MAXY * (1 + 4 + 4),
                              DIRTY_MEMORY_VGA);
    dac_index = 0;
    dac_state = 0;
    cursx = 0xf000; /* Put cursor off screen */
    cursy = 0xf000;
}

static void tcx_reset(DeviceState *d)
{
    TCXState *s = reinterpret_cast<TCXState *>(d);
    s->doReset();
}

uint64_t TCXState::dacReadl(void *opaque, hwaddr addr,
                            unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    uint32_t val = 0;

    switch (s->dac_state) {
    case 0:
        val = s->r[s->dac_index] << 24;
        s->dac_state++;
        break;
    case 1:
        val = s->g[s->dac_index] << 24;
        s->dac_state++;
        break;
    case 2:
        val = s->b[s->dac_index] << 24;
        s->dac_index = (s->dac_index + 1) & 0xff; /* Index autoincrement */
        /* fall through */
    default:
        s->dac_state = 0;
        break;
    }

    return val;
}

void TCXState::dacWritel(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    unsigned index;

    switch (addr) {
    case 0: /* Address */
        s->dac_index = val >> 24;
        s->dac_state = 0;
        break;
    case 4:  /* Pixel colours */
    case 12: /* Overlay (cursor) colours */
        if (addr & 8) {
            index = (s->dac_index & 3) + 256;
        } else {
            index = s->dac_index;
        }
        switch (s->dac_state) {
        case 0:
            s->r[index] = val >> 24;
            s->updatePaletteEntries(index, index + 1);
            s->dac_state++;
            break;
        case 1:
            s->g[index] = val >> 24;
            s->updatePaletteEntries(index, index + 1);
            s->dac_state++;
            break;
        case 2:
            s->b[index] = val >> 24;
            s->updatePaletteEntries(index, index + 1);
            s->dac_index = (s->dac_index + 1) & 0xff; /* Index autoincrement */
            /* fall through */
        default:
            s->dac_state = 0;
            break;
        }
        break;
    default: /* Control registers */
        break;
    }
}

static const MemoryRegionOps tcx_dac_ops = {
    .read = TCXState::dacReadl,
    .write = TCXState::dacWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

uint64_t TCXState::stipReadl(void *opaque, hwaddr addr,
                             unsigned size)
{
    return 0;
}

void TCXState::stipWritel(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    int i;
    uint32_t col;

    if (!(addr & 4)) {
        s->tmpblit = val;
    } else {
        addr = (addr >> 3) & 0xfffff;
        col = cpu_to_be32(s->tmpblit);
        if (s->depth == 24) {
            for (i = 0; i < 32; i++)  {
                if (val & 0x80000000) {
                    s->vram[addr + i] = s->tmpblit;
                    s->vram24[addr + i] = col;
                }
                val <<= 1;
            }
        } else {
            for (i = 0; i < 32; i++)  {
                if (val & 0x80000000) {
                    s->vram[addr + i] = s->tmpblit;
                }
                val <<= 1;
            }
        }
        s->setDirty( addr, 32);
    }
}

void TCXState::rstipWritel(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    int i;
    uint32_t col;

    if (!(addr & 4)) {
        s->tmpblit = val;
    } else {
        addr = (addr >> 3) & 0xfffff;
        col = cpu_to_be32(s->tmpblit);
        if (s->depth == 24) {
            for (i = 0; i < 32; i++) {
                if (val & 0x80000000) {
                    s->vram[addr + i] = s->tmpblit;
                    s->vram24[addr + i] = col;
                    s->cplane[addr + i] = col;
                }
                val <<= 1;
            }
        } else {
            for (i = 0; i < 32; i++)  {
                if (val & 0x80000000) {
                    s->vram[addr + i] = s->tmpblit;
                }
                val <<= 1;
            }
        }
        s->setDirty( addr, 32);
    }
}

static const MemoryRegionOps tcx_stip_ops = {
    .read = TCXState::stipReadl,
    .write = TCXState::stipWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps tcx_rstip_ops = {
    .read = TCXState::stipReadl,
    .write = TCXState::rstipWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

uint64_t TCXState::blitReadl(void *opaque, hwaddr addr,
                             unsigned size)
{
    return 0;
}

void TCXState::blitWritel(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    uint32_t adsr, len;
    int i;

    if (!(addr & 4)) {
        s->tmpblit = val;
    } else {
        addr = (addr >> 3) & 0xfffff;
        adsr = val & 0xffffff;
        len = ((val >> 24) & 0x1f) + 1;
        if (adsr == 0xffffff) {
            memset(&s->vram[addr], s->tmpblit, len);
            if (s->depth == 24) {
                val = s->tmpblit & 0xffffff;
                val = cpu_to_be32(val);
                for (i = 0; i < len; i++) {
                    s->vram24[addr + i] = val;
                }
            }
        } else {
            memcpy(&s->vram[addr], &s->vram[adsr], len);
            if (s->depth == 24) {
                memcpy(&s->vram24[addr], &s->vram24[adsr], len * 4);
            }
        }
        s->setDirty( addr, len);
    }
}

void TCXState::rblitWritel(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    uint32_t adsr, len;
    int i;

    if (!(addr & 4)) {
        s->tmpblit = val;
    } else {
        addr = (addr >> 3) & 0xfffff;
        adsr = val & 0xffffff;
        len = ((val >> 24) & 0x1f) + 1;
        if (adsr == 0xffffff) {
            memset(&s->vram[addr], s->tmpblit, len);
            if (s->depth == 24) {
                val = s->tmpblit & 0xffffff;
                val = cpu_to_be32(val);
                for (i = 0; i < len; i++) {
                    s->vram24[addr + i] = val;
                    s->cplane[addr + i] = val;
                }
            }
        } else {
            memcpy(&s->vram[addr], &s->vram[adsr], len);
            if (s->depth == 24) {
                memcpy(&s->vram24[addr], &s->vram24[adsr], len * 4);
                memcpy(&s->cplane[addr], &s->cplane[adsr], len * 4);
            }
        }
        s->setDirty( addr, len);
    }
}

static const MemoryRegionOps tcx_blit_ops = {
    .read = TCXState::blitReadl,
    .write = TCXState::blitWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps tcx_rblit_ops = {
    .read = TCXState::blitReadl,
    .write = TCXState::rblitWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void TCXState::invalidateCursorPosition()
{
    int ymin, ymax, start, end;

    /* invalidate only near the cursor */
    ymin = cursy;
    if (ymin >= height) {
        return;
    }
    ymax = MIN(height, ymin + 32);
    start = ymin * 1024;
    end   = ymax * 1024;

    setDirty(start, end - start);
}

uint64_t TCXState::thcReadl(void *opaque, hwaddr addr,
                            unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);
    uint64_t val;

    if (addr == TCX_THC_MISC) {
        val = s->thcmisc | 0x02000000;
    } else {
        val = 0;
    }
    return val;
}

void TCXState::thcWritel(void *opaque, hwaddr addr,
                         uint64_t val, unsigned size)
{
    TCXState *s = static_cast<TCXState *>(opaque);

    if (addr == TCX_THC_CURSXY) {
        s->invalidateCursorPosition();
        s->cursx = val >> 16;
        s->cursy = val;
        s->invalidateCursorPosition();
    } else if (addr >= TCX_THC_CURSMASK && addr < TCX_THC_CURSMASK + 128) {
        s->cursmask[(addr - TCX_THC_CURSMASK) >> 2] = val;
        s->invalidateCursorPosition();
    } else if (addr >= TCX_THC_CURSBITS && addr < TCX_THC_CURSBITS + 128) {
        s->cursbits[(addr - TCX_THC_CURSBITS) >> 2] = val;
        s->invalidateCursorPosition();
    } else if (addr == TCX_THC_MISC) {
        s->thcmisc = val;
    }

}

static const MemoryRegionOps tcx_thc_ops = {
    .read = TCXState::thcReadl,
    .write = TCXState::thcWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

uint64_t TCXState::dummyReadl(void *opaque, hwaddr addr,
                              unsigned size)
{
    return 0;
}

void TCXState::dummyWritel(void *opaque, hwaddr addr,
                            uint64_t val, unsigned size)
{
}

static const MemoryRegionOps tcx_dummy_ops = {
    .read = TCXState::dummyReadl,
    .write = TCXState::dummyWritel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const GraphicHwOps tcx_ops = {
    .invalidate = TCXState::invalidateDisplay,
    .gfx_update = TCXState::updateDisplay,
};

static const GraphicHwOps tcx24_ops = {
    .invalidate = TCXState::invalidate24Display,
    .gfx_update = TCXState::update24Display,
};

void TCXState::initfn(Object *obj)
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(obj);

    memory_region_init_rom_nomigrate(&rom, obj, "tcx.prom",
                                     FCODE_MAX_ROM_SIZE, &error_fatal);
    sysbus_init_mmio(sbd, &rom);

    /* 2/STIP : Stippler */
    memory_region_init_io(&stip, obj, &tcx_stip_ops, this, "tcx.stip",
                          TCX_STIP_NREGS);
    sysbus_init_mmio(sbd, &stip);

    /* 3/BLIT : Blitter */
    memory_region_init_io(&blit, obj, &tcx_blit_ops, this, "tcx.blit",
                          TCX_BLIT_NREGS);
    sysbus_init_mmio(sbd, &blit);

    /* 5/RSTIP : Raw Stippler */
    memory_region_init_io(&rstip, obj, &tcx_rstip_ops, this, "tcx.rstip",
                          TCX_RSTIP_NREGS);
    sysbus_init_mmio(sbd, &rstip);

    /* 6/RBLIT : Raw Blitter */
    memory_region_init_io(&rblit, obj, &tcx_rblit_ops, this, "tcx.rblit",
                          TCX_RBLIT_NREGS);
    sysbus_init_mmio(sbd, &rblit);

    /* 7/TEC : ??? */
    memory_region_init_io(&tec, obj, &tcx_dummy_ops, this, "tcx.tec",
                          TCX_TEC_NREGS);
    sysbus_init_mmio(sbd, &tec);

    /* 8/CMAP : DAC */
    memory_region_init_io(&dac, obj, &tcx_dac_ops, this, "tcx.dac",
                          TCX_DAC_NREGS);
    sysbus_init_mmio(sbd, &dac);

    /* 9/THC : Cursor */
    memory_region_init_io(&thc, obj, &tcx_thc_ops, this, "tcx.thc",
                          TCX_THC_NREGS);
    sysbus_init_mmio(sbd, &thc);

    /* 11/DHC : ??? */
    memory_region_init_io(&dhc, obj, &tcx_dummy_ops, this, "tcx.dhc",
                          TCX_DHC_NREGS);
    sysbus_init_mmio(sbd, &dhc);

    /* 12/ALT : ??? */
    memory_region_init_io(&alt, obj, &tcx_dummy_ops, this, "tcx.alt",
                          TCX_ALT_NREGS);
    sysbus_init_mmio(sbd, &alt);
}

static void tcx_initfn(Object *obj)
{
    TCXState *s = reinterpret_cast<TCXState *>(obj);
    s->initfn(obj);
}

void TCXState::realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(dev);
    TCXState *s = reinterpret_cast<TCXState *>(dev);
    ram_addr_t vram_offset = 0;
    int size, ret;
    uint8_t *vram_base;
    char *fcode_filename;

    memory_region_init_ram_nomigrate(&s->vram_mem, OBJECT(s), "tcx.vram",
                           s->vram_size * (1 + 4 + 4), &error_fatal);
    vmstate_register_ram_global(&s->vram_mem);
    memory_region_set_log(&s->vram_mem, true, DIRTY_MEMORY_VGA);
    vram_base = static_cast<uint8_t *>(memory_region_get_ram_ptr(&s->vram_mem));

    /* 10/ROM : FCode ROM */
    vmstate_register_ram_global(&s->rom);
    fcode_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, TCX_ROM_FILE);
    if (fcode_filename) {
        ret = load_image_mr(fcode_filename, &s->rom);
        g_free(fcode_filename);
        if (ret < 0 || ret > FCODE_MAX_ROM_SIZE) {
            warn_report("tcx: could not load prom '%s'", TCX_ROM_FILE);
        }
    }

    /* 0/DFB8 : 8-bit plane */
    s->vram = vram_base;
    size = s->vram_size;
    memory_region_init_alias(&s->vram_8bit, OBJECT(s), "tcx.vram.8bit",
                             &s->vram_mem, vram_offset, size);
    sysbus_init_mmio(sbd, &s->vram_8bit);
    vram_offset += size;
    vram_base += size;

    /* 1/DFB24 : 24bit plane */
    size = s->vram_size * 4;
    s->vram24 = (uint32_t *)vram_base;
    s->vram24_offset = vram_offset;
    memory_region_init_alias(&s->vram_24bit, OBJECT(s), "tcx.vram.24bit",
                             &s->vram_mem, vram_offset, size);
    sysbus_init_mmio(sbd, &s->vram_24bit);
    vram_offset += size;
    vram_base += size;

    /* 4/RDFB32 : Raw Framebuffer */
    size = s->vram_size * 4;
    s->cplane = (uint32_t *)vram_base;
    s->cplane_offset = vram_offset;
    memory_region_init_alias(&s->vram_cplane, OBJECT(s), "tcx.vram.cplane",
                             &s->vram_mem, vram_offset, size);
    sysbus_init_mmio(sbd, &s->vram_cplane);

    /* 9/THC24bits : NetBSD writes here even with 8-bit display: dummy */
    if (s->depth == 8) {
        memory_region_init_io(&s->thc24, OBJECT(s), &tcx_dummy_ops, s,
                              "tcx.thc24", TCX_THC_NREGS);
        sysbus_init_mmio(sbd, &s->thc24);
    }

    sysbus_init_irq(sbd, &s->irq);

    if (s->depth == 8) {
        s->con = graphic_console_init(dev, 0, &tcx_ops, s);
    } else {
        s->con = graphic_console_init(dev, 0, &tcx24_ops, s);
    }
    s->thcmisc = 0;

    qemu_console_resize(s->con, s->width, s->height);
}

static const Property tcx_properties[] = {
    DEFINE_PROP_UINT32("vram_size", TCXState, vram_size, -1),
    DEFINE_PROP_UINT16("width",    TCXState, width,     -1),
    DEFINE_PROP_UINT16("height",   TCXState, height,    -1),
    DEFINE_PROP_UINT16("depth",    TCXState, depth,     -1),
};

static void tcx_realizefn(DeviceState *dev, Error **errp)
{
    TCXState *s = reinterpret_cast<TCXState *>(dev);
    s->realize(dev, errp);
}

void TCXState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tcx_realizefn;
    device_class_set_legacy_reset(dc, tcx_reset);
    dc->vmsd = &vmstate_tcx;
    device_class_set_props(dc, tcx_properties);
}

static const TypeInfo tcx_info = {
    .name          = TYPE_TCX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TCXState),
    .instance_init = tcx_initfn,
    .class_init    = TCXState::classInit,
};

static void tcx_register_types(void)
{
    type_register_static(&tcx_info);
}

type_init(tcx_register_types)
