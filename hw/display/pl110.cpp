/*
 * Arm PrimeCell PL110 Color LCD Controller
 *
 * Copyright (c) 2005-2009 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GNU LGPL
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#define PL110_CR_EN   0x001
#define PL110_CR_BGR  0x100
#define PL110_CR_BEBO 0x200
#define PL110_CR_BEPO 0x400
#define PL110_CR_PWR  0x800
#define PL110_IE_NB   0x004
#define PL110_IE_VC   0x008

enum pl110_bppmode
{
    BPP_1,
    BPP_2,
    BPP_4,
    BPP_8,
    BPP_16,
    BPP_32,
    BPP_16_565, /* PL111 only */
    BPP_12      /* PL111 only */
};


/* The Versatile/PB uses a slightly modified PL110 controller.  */
enum pl110_version
{
    VERSION_PL110,
    VERSION_PL110_VERSATILE,
    VERSION_PL111
};

#define TYPE_PL110 "pl110"
OBJECT_DECLARE_SIMPLE_TYPE(PL110State, PL110)

struct PL110State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    QEMUTimer *vblank_timer;

    int version;
    uint32_t timing[4];
    uint32_t cr;
    uint32_t upbase;
    uint32_t lpbase;
    uint32_t int_status;
    uint32_t int_mask;
    int cols;
    int rows;
    enum pl110_bppmode bpp;
    int invalidate;
    uint32_t mux_ctrl;
    uint32_t palette[256];
    uint32_t raw_palette[128];
    qemu_irq irq;
    MemoryRegion *fbmem;

    /* ----- methods ----- */
    void realize(DeviceState *dev, Error **errp);

    static void classInit(ObjectClass *klass, const void *data);

    /* static MMIO callbacks */
    static uint64_t mmioRead(void *opaque, hwaddr offset, unsigned size);
    static void mmioWrite(void *opaque, hwaddr offset, uint64_t val,
                          unsigned size);

    /* static timer callback */
    static void vblankInterrupt(void *opaque);

    /* static display callbacks */
    static void updateDisplay(void *opaque);
    static void invalidateDisplay(void *opaque);

    /* static GPIO callback */
    static void muxCtrlSet(void *opaque, int line, int level);

    /* static VMState callback */
    static int postLoad(void *opaque, int version_id);

private:
    bool isEnabled();
    void updatePalette(int n);
    void resize(int width, int height);
    void updateIrq();
};

static const VMStateField vmstate_pl110_fields[] = {
    VMSTATE_INT32(version, PL110State),
    VMSTATE_UINT32_ARRAY(timing, PL110State, 4),
    VMSTATE_UINT32(cr, PL110State),
    VMSTATE_UINT32(upbase, PL110State),
    VMSTATE_UINT32(lpbase, PL110State),
    VMSTATE_UINT32(int_status, PL110State),
    VMSTATE_UINT32(int_mask, PL110State),
    VMSTATE_INT32(cols, PL110State),
    VMSTATE_INT32(rows, PL110State),
    VMSTATE_UINT32(bpp, PL110State),
    VMSTATE_INT32(invalidate, PL110State),
    VMSTATE_UINT32_ARRAY(palette, PL110State, 256),
    VMSTATE_UINT32_ARRAY(raw_palette, PL110State, 128),
    VMSTATE_UINT32_V(mux_ctrl, PL110State, 2),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_pl110 = {
    .name = "pl110",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = PL110State::postLoad,
    .fields = vmstate_pl110_fields,
};

static const unsigned char pl110_id[] =
{ 0x10, 0x11, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1 };

static const unsigned char pl111_id[] = {
    0x11, 0x11, 0x24, 0x00, 0x0d, 0xf0, 0x05, 0xb1
};


/* Indexed by pl110_version */
static const unsigned char *idregs[] = {
    pl110_id,
    /* The ARM documentation (DDI0224C) says the CLCDC on the Versatile board
     * has a different ID (0x93, 0x10, 0x04, 0x00, ...). However the hardware
     * itself has the same ID values as a stock PL110, and guests (in
     * particular Linux) rely on this. We emulate what the hardware does,
     * rather than what the docs claim it ought to do.
     */
    pl110_id,
    pl111_id
};

#define COPY_PIXEL(to, from) do { *(uint32_t *)to = from; to += 4; } while (0)

#undef RGB
#define BORDER bgr
#define ORDER 0
#include "pl110_template.h"
#define ORDER 1
#include "pl110_template.h"
#define ORDER 2
#include "pl110_template.h"
#undef BORDER
#define RGB
#define BORDER rgb
#define ORDER 0
#include "pl110_template.h"
#define ORDER 1
#include "pl110_template.h"
#define ORDER 2
#include "pl110_template.h"
#undef BORDER

#undef COPY_PIXEL

static drawfn pl110_draw_fn_32[48] = {
    pl110_draw_line1_lblp_bgr,
    pl110_draw_line2_lblp_bgr,
    pl110_draw_line4_lblp_bgr,
    pl110_draw_line8_lblp_bgr,
    pl110_draw_line16_555_lblp_bgr,
    pl110_draw_line32_lblp_bgr,
    pl110_draw_line16_lblp_bgr,
    pl110_draw_line12_lblp_bgr,

    pl110_draw_line1_bbbp_bgr,
    pl110_draw_line2_bbbp_bgr,
    pl110_draw_line4_bbbp_bgr,
    pl110_draw_line8_bbbp_bgr,
    pl110_draw_line16_555_bbbp_bgr,
    pl110_draw_line32_bbbp_bgr,
    pl110_draw_line16_bbbp_bgr,
    pl110_draw_line12_bbbp_bgr,

    pl110_draw_line1_lbbp_bgr,
    pl110_draw_line2_lbbp_bgr,
    pl110_draw_line4_lbbp_bgr,
    pl110_draw_line8_lbbp_bgr,
    pl110_draw_line16_555_lbbp_bgr,
    pl110_draw_line32_lbbp_bgr,
    pl110_draw_line16_lbbp_bgr,
    pl110_draw_line12_lbbp_bgr,

    pl110_draw_line1_lblp_rgb,
    pl110_draw_line2_lblp_rgb,
    pl110_draw_line4_lblp_rgb,
    pl110_draw_line8_lblp_rgb,
    pl110_draw_line16_555_lblp_rgb,
    pl110_draw_line32_lblp_rgb,
    pl110_draw_line16_lblp_rgb,
    pl110_draw_line12_lblp_rgb,

    pl110_draw_line1_bbbp_rgb,
    pl110_draw_line2_bbbp_rgb,
    pl110_draw_line4_bbbp_rgb,
    pl110_draw_line8_bbbp_rgb,
    pl110_draw_line16_555_bbbp_rgb,
    pl110_draw_line32_bbbp_rgb,
    pl110_draw_line16_bbbp_rgb,
    pl110_draw_line12_bbbp_rgb,

    pl110_draw_line1_lbbp_rgb,
    pl110_draw_line2_lbbp_rgb,
    pl110_draw_line4_lbbp_rgb,
    pl110_draw_line8_lbbp_rgb,
    pl110_draw_line16_555_lbbp_rgb,
    pl110_draw_line32_lbbp_rgb,
    pl110_draw_line16_lbbp_rgb,
    pl110_draw_line12_lbbp_rgb,
};

bool PL110State::isEnabled()
{
  return (cr & PL110_CR_EN) && (cr & PL110_CR_PWR);
}

void PL110State::updateDisplay(void *opaque)
{
    PL110State *s = static_cast<PL110State *>(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    drawfn fn;
    int src_width;
    int bpp_offset;
    int first;
    int last;

    if (!s->isEnabled()) {
        return;
    }

    if (s->cr & PL110_CR_BGR)
        bpp_offset = 0;
    else
        bpp_offset = 24;

    if ((s->version != VERSION_PL111) && (s->bpp == BPP_16)) {
        /* The PL110's native 16 bit mode is 5551; however
         * most boards with a PL110 implement an external
         * mux which allows bits to be reshuffled to give
         * 565 format. The mux is typically controlled by
         * an external system register.
         * This is controlled by a GPIO input pin
         * so boards can wire it up to their register.
         *
         * The PL111 straightforwardly implements both
         * 5551 and 565 under control of the bpp field
         * in the LCDControl register.
         */
        switch (s->mux_ctrl) {
        case 3: /* 565 BGR */
            bpp_offset = (BPP_16_565 - BPP_16);
            break;
        case 1: /* 5551 */
            break;
        case 0: /* 888; also if we have loaded vmstate from an old version */
        case 2: /* 565 RGB */
        default:
            /* treat as 565 but honour BGR bit */
            bpp_offset += (BPP_16_565 - BPP_16);
            break;
        }
    }

    if (s->cr & PL110_CR_BEBO) {
        fn = pl110_draw_fn_32[s->bpp + 8 + bpp_offset];
    } else if (s->cr & PL110_CR_BEPO) {
        fn = pl110_draw_fn_32[s->bpp + 16 + bpp_offset];
    } else {
        fn = pl110_draw_fn_32[s->bpp + bpp_offset];
    }

    src_width = s->cols;
    switch (s->bpp) {
    case BPP_1:
        src_width >>= 3;
        break;
    case BPP_2:
        src_width >>= 2;
        break;
    case BPP_4:
        src_width >>= 1;
        break;
    case BPP_8:
        break;
    case BPP_16:
    case BPP_16_565:
    case BPP_12:
        src_width <<= 1;
        break;
    case BPP_32:
        src_width <<= 2;
        break;
    }
    first = 0;
    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection,
                                          s->fbmem,
                                          s->upbase,
                                          s->rows, src_width);
    }

    framebuffer_update_display(surface, &s->fbsection,
                               s->cols, s->rows,
                               src_width, s->cols * 4, 0,
                               s->invalidate,
                               fn, s->palette,
                               &first, &last);

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, s->cols, last - first + 1);
    }
    s->invalidate = 0;
}

void PL110State::invalidateDisplay(void *opaque)
{
    PL110State *s = static_cast<PL110State *>(opaque);
    s->invalidate = 1;
    if (s->isEnabled()) {
        qemu_console_resize(s->con, s->cols, s->rows);
    }
}

void PL110State::updatePalette(int n)
{
    DisplaySurface *surface = qemu_console_surface(con);
    int i;
    uint32_t raw;
    unsigned int r, g, b;

    raw = raw_palette[n];
    n <<= 1;
    for (i = 0; i < 2; i++) {
        r = (raw & 0x1f) << 3;
        raw >>= 5;
        g = (raw & 0x1f) << 3;
        raw >>= 5;
        b = (raw & 0x1f) << 3;
        /* The I bit is ignored.  */
        raw >>= 6;
        switch (surface_bits_per_pixel(surface)) {
        case 8:
            palette[n] = rgb_to_pixel8(r, g, b);
            break;
        case 15:
            palette[n] = rgb_to_pixel15(r, g, b);
            break;
        case 16:
            palette[n] = rgb_to_pixel16(r, g, b);
            break;
        case 24:
        case 32:
            palette[n] = rgb_to_pixel32(r, g, b);
            break;
        }
        n++;
    }
}

void PL110State::resize(int width, int height)
{
    if (width != cols || height != rows) {
        if (isEnabled()) {
            qemu_console_resize(con, width, height);
        }
    }
    cols = width;
    rows = height;
}

/* Update interrupts.  */
void PL110State::updateIrq()
{
    /* Raise IRQ if enabled and any status bit is 1 */
    if (int_status & int_mask) {
        qemu_irq_raise(irq);
    } else {
        qemu_irq_lower(irq);
    }
}

void PL110State::vblankInterrupt(void *opaque)
{
    PL110State *s = static_cast<PL110State *>(opaque);

    /* Fire the vertical compare and next base IRQs and re-arm */
    s->int_status |= (PL110_IE_NB | PL110_IE_VC);
    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                NANOSECONDS_PER_SECOND / 60);
    s->updateIrq();
}

uint64_t PL110State::mmioRead(void *opaque, hwaddr offset, unsigned size)
{
    PL110State *s = static_cast<PL110State *>(opaque);

    if (offset >= 0xfe0 && offset < 0x1000) {
        return idregs[s->version][(offset - 0xfe0) >> 2];
    }
    if (offset >= 0x200 && offset < 0x400) {
        return s->raw_palette[(offset - 0x200) >> 2];
    }
    switch (offset >> 2) {
    case 0: /* LCDTiming0 */
        return s->timing[0];
    case 1: /* LCDTiming1 */
        return s->timing[1];
    case 2: /* LCDTiming2 */
        return s->timing[2];
    case 3: /* LCDTiming3 */
        return s->timing[3];
    case 4: /* LCDUPBASE */
        return s->upbase;
    case 5: /* LCDLPBASE */
        return s->lpbase;
    case 6: /* LCDIMSC */
        if (s->version != VERSION_PL110) {
            return s->cr;
        }
        return s->int_mask;
    case 7: /* LCDControl */
        if (s->version != VERSION_PL110) {
            return s->int_mask;
        }
        return s->cr;
    case 8: /* LCDRIS */
        return s->int_status;
    case 9: /* LCDMIS */
        return s->int_status & s->int_mask;
    case 11: /* LCDUPCURR */
        /* TODO: Implement vertical refresh.  */
        return s->upbase;
    case 12: /* LCDLPCURR */
        return s->lpbase;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl110_read: Bad offset %x\n", (int)offset);
        return 0;
    }
}

void PL110State::mmioWrite(void *opaque, hwaddr offset,
                            uint64_t val, unsigned size)
{
    PL110State *s = static_cast<PL110State *>(opaque);
    int n;

    /* For simplicity invalidate the display whenever a control register
       is written to.  */
    s->invalidate = 1;
    if (offset >= 0x200 && offset < 0x400) {
        /* Palette.  */
        n = (offset - 0x200) >> 2;
        s->raw_palette[(offset - 0x200) >> 2] = val;
        s->updatePalette(n);
        return;
    }
    switch (offset >> 2) {
    case 0: /* LCDTiming0 */
        s->timing[0] = val;
        n = ((val & 0xfc) + 4) * 4;
        s->resize(n, s->rows);
        break;
    case 1: /* LCDTiming1 */
        s->timing[1] = val;
        n = (val & 0x3ff) + 1;
        s->resize(s->cols, n);
        break;
    case 2: /* LCDTiming2 */
        s->timing[2] = val;
        break;
    case 3: /* LCDTiming3 */
        s->timing[3] = val;
        break;
    case 4: /* LCDUPBASE */
        s->upbase = val;
        break;
    case 5: /* LCDLPBASE */
        s->lpbase = val;
        break;
    case 6: /* LCDIMSC */
        if (s->version != VERSION_PL110) {
            goto control;
        }
    imsc:
        s->int_mask = val;
        s->updateIrq();
        break;
    case 7: /* LCDControl */
        if (s->version != VERSION_PL110) {
            goto imsc;
        }
    control:
        s->cr = val;
        s->bpp = static_cast<pl110_bppmode>((val >> 1) & 7);
        if (s->isEnabled()) {
            qemu_console_resize(s->con, s->cols, s->rows);
            timer_mod(s->vblank_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                        NANOSECONDS_PER_SECOND / 60);
        } else {
            timer_del(s->vblank_timer);
        }
        break;
    case 10: /* LCDICR */
        s->int_status &= ~val;
        s->updateIrq();
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl110_write: Bad offset %x\n", (int)offset);
    }
}

static const MemoryRegionOps pl110_ops = {
    .read = PL110State::mmioRead,
    .write = PL110State::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void PL110State::muxCtrlSet(void *opaque, int line, int level)
{
    PL110State *s = static_cast<PL110State *>(opaque);
    s->mux_ctrl = level;
}

int PL110State::postLoad(void *opaque, int version_id)
{
    PL110State *s = static_cast<PL110State *>(opaque);
    /* Make sure we redraw, and at the right size */
    PL110State::invalidateDisplay(s);
    return 0;
}

static const GraphicHwOps pl110_gfx_ops = {
    .invalidate  = PL110State::invalidateDisplay,
    .gfx_update  = PL110State::updateDisplay,
};

static const Property pl110_properties[] = {
    DEFINE_PROP_LINK("framebuffer-memory", PL110State, fbmem,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void pl110_realize_wrapper(DeviceState *dev, Error **errp)
{
    PL110State *s = PL110(dev);
    s->realize(dev, errp);
}

void PL110State::realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!fbmem) {
        error_setg(errp, "'framebuffer-memory' property was not set");
        return;
    }

    memory_region_init_io(&iomem, OBJECT(this), &pl110_ops, this,
                          "pl110", 0x1000);
    sysbus_init_mmio(sbd, &iomem);
    sysbus_init_irq(sbd, &irq);
    vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                PL110State::vblankInterrupt, this);
    qdev_init_gpio_in(dev, PL110State::muxCtrlSet, 1);
    con = graphic_console_init(dev, 0, &pl110_gfx_ops, this);
}

static void pl110_init(Object *obj)
{
    PL110State *s = PL110(obj);

    s->version = VERSION_PL110;
}

static void pl110_versatile_init(Object *obj)
{
    PL110State *s = PL110(obj);

    s->version = VERSION_PL110_VERSATILE;
}

static void pl111_init(Object *obj)
{
    PL110State *s = PL110(obj);

    s->version = VERSION_PL111;
}

void PL110State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->vmsd = &vmstate_pl110;
    dc->realize = pl110_realize_wrapper;
    device_class_set_props(dc, pl110_properties);
}

static const TypeInfo pl110_info = {
    .name          = TYPE_PL110,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL110State),
    .instance_init = pl110_init,
    .class_init    = PL110State::classInit,
};

static const TypeInfo pl110_versatile_info = {
    .name          = "pl110_versatile",
    .parent        = TYPE_PL110,
    .instance_init = pl110_versatile_init,
};

static const TypeInfo pl111_info = {
    .name          = "pl111",
    .parent        = TYPE_PL110,
    .instance_init = pl111_init,
};

static void pl110_register_types(void)
{
    type_register_static(&pl110_info);
    type_register_static(&pl110_versatile_info);
    type_register_static(&pl111_info);
}

type_init(pl110_register_types)
