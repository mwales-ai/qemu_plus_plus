/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Refactoring for Pi2 Copyright (c) 2015, Microsoft. Written by Andrew Baumann.
 *
 * Heavily based on milkymist-vgafb.c, copyright terms below:
 *  QEMU model of the Milkymist VGA framebuffer.
 *
 *  Copyright (c) 2010-2012 Michael Walle <michael@walle.cc>
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
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "ui/console.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define DEFAULT_VCRAM_SIZE 0x4000000
#define BCM2835_FB_OFFSET  0x00100000

/* Maximum permitted framebuffer size; experimentally determined on an rpi2 */
#define XRES_MAX 3840
#define YRES_MAX 2560
/* Framebuffer size used if guest requests zero size */
#define XRES_SMALL 592
#define YRES_SMALL 488

static void fb_invalidate_display(void *opaque)
{
    BCM2835FBState *s = BCM2835_FB(opaque);

    s->invalidate = true;
}

static void draw_line_src16(void *opaque, uint8_t *dst, const uint8_t *src,
                            int width, int deststep)
{
    BCM2835FBState *s = static_cast<BCM2835FBState *>(opaque);
    uint16_t rgb565;
    uint32_t rgb888;
    uint8_t r, g, b;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);

    while (width--) {
        switch (s->config.bpp) {
        case 8:
            /* lookup palette starting at video ram base
             * TODO: cache translation, rather than doing this each time!
             */
            rgb888 = ldl_le_phys(&s->dma_as, s->vcram_base + (*src << 2));
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            src++;
            break;
        case 16:
            rgb565 = lduw_le_p(src);
            r = ((rgb565 >> 11) & 0x1f) << 3;
            g = ((rgb565 >>  5) & 0x3f) << 2;
            b = ((rgb565 >>  0) & 0x1f) << 3;
            src += 2;
            break;
        case 24:
            rgb888 = ldl_le_p(src);
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            src += 3;
            break;
        case 32:
            rgb888 = ldl_le_p(src);
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            src += 4;
            break;
        default:
            r = 0;
            g = 0;
            b = 0;
            break;
        }

        if (s->config.pixo == 0) {
            /* swap to BGR pixel format */
            uint8_t tmp = r;
            r = b;
            b = tmp;
        }

        switch (bpp) {
        case 8:
            *dst++ = rgb_to_pixel8(r, g, b);
            break;
        case 15:
            *(uint16_t *)dst = rgb_to_pixel15(r, g, b);
            dst += 2;
            break;
        case 16:
            *(uint16_t *)dst = rgb_to_pixel16(r, g, b);
            dst += 2;
            break;
        case 24:
            rgb888 = rgb_to_pixel24(r, g, b);
            *dst++ = rgb888 & 0xff;
            *dst++ = (rgb888 >> 8) & 0xff;
            *dst++ = (rgb888 >> 16) & 0xff;
            break;
        case 32:
            *(uint32_t *)dst = rgb_to_pixel32(r, g, b);
            dst += 4;
            break;
        default:
            return;
        }
    }
}

static bool fb_use_offsets(BCM2835FBConfig *config)
{
    /*
     * Return true if we should use the viewport offsets.
     * Experimentally, the hardware seems to do this only if the
     * viewport size is larger than the physical screen. (It doesn't
     * prevent the guest setting this silly viewport setting, though...)
     */
    return config->xres_virtual > config->xres ||
        config->yres_virtual > config->yres;
}

static void fb_update_display(void *opaque)
{
    BCM2835FBState *s = static_cast<BCM2835FBState *>(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0;
    int last = 0;
    int src_width = 0;
    int dest_width = 0;
    uint32_t xoff = 0, yoff = 0;

    if (s->lock || !s->config.xres) {
        return;
    }

    src_width = bcm2835_fb_get_pitch(&s->config);
    if (fb_use_offsets(&s->config)) {
        xoff = s->config.xoffset;
        yoff = s->config.yoffset;
    }

    dest_width = s->config.xres;

    switch (surface_bits_per_pixel(surface)) {
    case 0:
        return;
    case 8:
        break;
    case 15:
        dest_width *= 2;
        break;
    case 16:
        dest_width *= 2;
        break;
    case 24:
        dest_width *= 3;
        break;
    case 32:
        dest_width *= 4;
        break;
    default:
        hw_error("bcm2835_fb: bad color depth\n");
        break;
    }

    if (s->invalidate) {
        hwaddr base = s->config.base + xoff + (hwaddr)yoff * src_width;
        framebuffer_update_memory_section(&s->fbsection, s->dma_mr,
                                          base,
                                          s->config.yres, src_width);
    }

    framebuffer_update_display(surface, &s->fbsection,
                               s->config.xres, s->config.yres,
                               src_width, dest_width, 0, s->invalidate,
                               draw_line_src16, s, &first, &last);

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, s->config.xres,
                       last - first + 1);
    }

    s->invalidate = false;
}

void bcm2835_fb_validate_config(BCM2835FBConfig *config)
{
    /*
     * Validate the config, and clip any bogus values into range,
     * as the hardware does. Note that fb_update_display() relies on
     * this happening to prevent it from performing out-of-range
     * accesses on redraw.
     */
    config->xres = MIN(config->xres, XRES_MAX);
    config->xres_virtual = MIN(config->xres_virtual, XRES_MAX);
    config->yres = MIN(config->yres, YRES_MAX);
    config->yres_virtual = MIN(config->yres_virtual, YRES_MAX);

    /*
     * These are not minima: a 40x40 framebuffer will be accepted.
     * They're only used as defaults if the guest asks for zero size.
     */
    if (config->xres == 0) {
        config->xres = XRES_SMALL;
    }
    if (config->yres == 0) {
        config->yres = YRES_SMALL;
    }
    if (config->xres_virtual == 0) {
        config->xres_virtual = config->xres;
    }
    if (config->yres_virtual == 0) {
        config->yres_virtual = config->yres;
    }

    if (fb_use_offsets(config)) {
        /* Clip the offsets so the viewport is within the physical screen */
        config->xoffset = MIN(config->xoffset,
                              config->xres_virtual - config->xres);
        config->yoffset = MIN(config->yoffset,
                              config->yres_virtual - config->yres);
    }
}

void bcm2835_fb_reconfigure(BCM2835FBState *s, BCM2835FBConfig *newconfig)
{
    s->lock = true;

    s->config = *newconfig;

    s->invalidate = true;
    qemu_console_resize(s->con, s->config.xres, s->config.yres);
    s->lock = false;
}

static void bcm2835_fb_mbox_push(BCM2835FBState *s, uint32_t value)
{
    uint32_t pitch;
    uint32_t size;
    BCM2835FBConfig newconf;

    value &= ~0xf;

    newconf.xres = ldl_le_phys(&s->dma_as, value);
    newconf.yres = ldl_le_phys(&s->dma_as, value + 4);
    newconf.xres_virtual = ldl_le_phys(&s->dma_as, value + 8);
    newconf.yres_virtual = ldl_le_phys(&s->dma_as, value + 12);
    newconf.bpp = ldl_le_phys(&s->dma_as, value + 20);
    newconf.xoffset = ldl_le_phys(&s->dma_as, value + 24);
    newconf.yoffset = ldl_le_phys(&s->dma_as, value + 28);

    newconf.base = s->vcram_base + BCM2835_FB_OFFSET;

    /* Copy fields which we don't want to change from the existing config */
    newconf.pixo = s->config.pixo;
    newconf.alpha = s->config.alpha;

    bcm2835_fb_validate_config(&newconf);

    pitch = bcm2835_fb_get_pitch(&newconf);
    size = bcm2835_fb_get_size(&newconf);

    stl_le_phys(&s->dma_as, value + 16, pitch);
    stl_le_phys(&s->dma_as, value + 32, newconf.base);
    stl_le_phys(&s->dma_as, value + 36, size);

    bcm2835_fb_reconfigure(s, &newconf);
}

static uint64_t bcm2835_fb_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835FBState *s = static_cast<BCM2835FBState *>(opaque);
    uint32_t res = 0;

    switch (offset) {
    case MBOX_AS_DATA:
        res = MBOX_CHAN_FB;
        s->pending = false;
        qemu_set_irq(s->mbox_irq, 0);
        break;

    case MBOX_AS_PENDING:
        res = s->pending;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }

    return res;
}

static void bcm2835_fb_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    BCM2835FBState *s = static_cast<BCM2835FBState *>(opaque);

    switch (offset) {
    case MBOX_AS_DATA:
        /* bcm2835_mbox should check our pending status before pushing */
        assert(!s->pending);
        s->pending = true;
        bcm2835_fb_mbox_push(s, value);
        qemu_set_irq(s->mbox_irq, 1);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps bcm2835_fb_ops = {
    .read = bcm2835_fb_read,
    .write = bcm2835_fb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const VMStateField vmstate_bcm2835_fb_fields[] = {
    VMSTATE_BOOL(lock, BCM2835FBState),
    VMSTATE_BOOL(invalidate, BCM2835FBState),
    VMSTATE_BOOL(pending, BCM2835FBState),
    VMSTATE_UINT32(config.xres, BCM2835FBState),
    VMSTATE_UINT32(config.yres, BCM2835FBState),
    VMSTATE_UINT32(config.xres_virtual, BCM2835FBState),
    VMSTATE_UINT32(config.yres_virtual, BCM2835FBState),
    VMSTATE_UINT32(config.xoffset, BCM2835FBState),
    VMSTATE_UINT32(config.yoffset, BCM2835FBState),
    VMSTATE_UINT32(config.bpp, BCM2835FBState),
    VMSTATE_UINT32(config.base, BCM2835FBState),
    VMSTATE_UNUSED(8), /* Was pitch and size */
    VMSTATE_UINT32(config.pixo, BCM2835FBState),
    VMSTATE_UINT32(config.alpha, BCM2835FBState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_bcm2835_fb = {
    .name = TYPE_BCM2835_FB,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_bcm2835_fb_fields,
};

static const GraphicHwOps vgafb_ops = {
    .invalidate  = fb_invalidate_display,
    .gfx_update  = fb_update_display,
};

void BCM2835FBState::init()
{
    memory_region_init_io(&iomem, OBJECT(this), &bcm2835_fb_ops, this,
                          TYPE_BCM2835_FB, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(this), &mbox_irq);
}

void BCM2835FBState::reset()
{
    pending = false;
    config = initial_config;
    invalidate = true;
    lock = false;
}

void BCM2835FBState::realize(Error **errp)
{
    if (vcram_base == 0) {
        error_setg(errp, "%s: required vcram-base property not set", __func__);
        return;
    }

    Object *obj = object_property_get_link(OBJECT(this), "dma-mr", &error_abort);

    initial_config.xres_virtual = initial_config.xres;
    initial_config.yres_virtual = initial_config.yres;
    initial_config.xoffset = 0;
    initial_config.yoffset = 0;
    initial_config.base = vcram_base + BCM2835_FB_OFFSET;

    dma_mr = MEMORY_REGION(obj);
    address_space_init(&dma_as, dma_mr, TYPE_BCM2835_FB "-memory");

    reset();

    con = graphic_console_init(DEVICE(this), 0, &vgafb_ops, this);
    qemu_console_resize(con, config.xres, config.yres);
}

static const Property bcm2835_fb_props[] = {
    DEFINE_PROP_UINT32("vcram-base", BCM2835FBState, vcram_base, 0),
    DEFINE_PROP_UINT32("vcram-size", BCM2835FBState, vcram_size,
                       DEFAULT_VCRAM_SIZE),
    DEFINE_PROP_UINT32("xres", BCM2835FBState, initial_config.xres, 640),
    DEFINE_PROP_UINT32("yres", BCM2835FBState, initial_config.yres, 480),
    DEFINE_PROP_UINT32("bpp", BCM2835FBState, initial_config.bpp, 16),
    DEFINE_PROP_UINT32("pixo", BCM2835FBState,
                       initial_config.pixo, 1),
    DEFINE_PROP_UINT32("alpha", BCM2835FBState,
                       initial_config.alpha, 2),
};

void BCM2835FBState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, bcm2835_fb_props);
    dc->vmsd = &vmstate_bcm2835_fb;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(BCM2835FBState, TYPE_BCM2835_FB, TYPE_SYS_BUS_DEVICE)
