/*
 * QEMU JAZZ LED emulator.
 *
 * Copyright (c) 2007-2012 Herve Poussineau
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
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "trace.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"

typedef enum {
    REDRAW_NONE = 0, REDRAW_SEGMENTS = 1, REDRAW_BACKGROUND = 2,
} screen_state_t;

#define TYPE_JAZZ_LED "jazz-led"
OBJECT_DECLARE_SIMPLE_TYPE(LedState, JAZZ_LED)

struct LedState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t segments;
    QemuConsole *con;
    screen_state_t state;

    /* Static MMIO callbacks */
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned int size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned int size);

    /* Static display callbacks */
    static void updateDisplay(void *opaque);
    static void invalidateDisplay(void *opaque);
    static void textUpdate(void *opaque, console_ch_t *chardata);

    /* Static VMState callback */
    static int postLoad(void *opaque, int version_id);

    /* Instance methods */
    void initfn(Object *obj);
    void realize(DeviceState *dev, Error **errp);
    void reset(DeviceState *d);

    /* Static drawing helpers */
    static void drawHorizontalLine(DisplaySurface *ds, int posy,
                                   int posx1, int posx2, uint32_t color);
    static void drawVerticalLine(DisplaySurface *ds, int posx,
                                 int posy1, int posy2, uint32_t color);

    /* Class init */
    static void classInit(ObjectClass *klass, const void *data);
};

void LedState::drawHorizontalLine(DisplaySurface *ds,
                                   int posy, int posx1, int posx2,
                                   uint32_t color)
{
    uint8_t *d;
    int x, bpp;

    bpp = (surface_bits_per_pixel(ds) + 7) >> 3;
    d = static_cast<uint8_t *>(surface_data(ds)) + surface_stride(ds) * posy + bpp * posx1;
    switch (bpp) {
    case 1:
        for (x = posx1; x <= posx2; x++) {
            *((uint8_t *)d) = color;
            d++;
        }
        break;
    case 2:
        for (x = posx1; x <= posx2; x++) {
            *((uint16_t *)d) = color;
            d += 2;
        }
        break;
    case 4:
        for (x = posx1; x <= posx2; x++) {
            *((uint32_t *)d) = color;
            d += 4;
        }
        break;
    }
}

void LedState::drawVerticalLine(DisplaySurface *ds,
                                 int posx, int posy1, int posy2,
                                 uint32_t color)
{
    uint8_t *d;
    int y, bpp;

    bpp = (surface_bits_per_pixel(ds) + 7) >> 3;
    d = static_cast<uint8_t *>(surface_data(ds)) + surface_stride(ds) * posy1 + bpp * posx;
    switch (bpp) {
    case 1:
        for (y = posy1; y <= posy2; y++) {
            *((uint8_t *)d) = color;
            d += surface_stride(ds);
        }
        break;
    case 2:
        for (y = posy1; y <= posy2; y++) {
            *((uint16_t *)d) = color;
            d += surface_stride(ds);
        }
        break;
    case 4:
        for (y = posy1; y <= posy2; y++) {
            *((uint32_t *)d) = color;
            d += surface_stride(ds);
        }
        break;
    }
}

uint64_t LedState::mmioRead(void *opaque, hwaddr addr, unsigned int size)
{
    LedState *s = static_cast<LedState *>(opaque);
    uint8_t val;

    val = s->segments;
    trace_jazz_led_read(addr, val);

    return val;
}

void LedState::mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned int size)
{
    LedState *s = static_cast<LedState *>(opaque);
    uint8_t new_val = val & 0xff;

    trace_jazz_led_write(addr, new_val);

    s->segments = new_val;
    s->state = static_cast<screen_state_t>(s->state | REDRAW_SEGMENTS);
}

static const MemoryRegionOps led_ops = {
    .read = LedState::mmioRead,
    .write = LedState::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1, },
};

void LedState::updateDisplay(void *opaque)
{
    LedState *s = static_cast<LedState *>(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint8_t *d1;
    uint32_t color_segment, color_led;
    int y, bpp;

    if (s->state & REDRAW_BACKGROUND) {
        /* clear screen */
        bpp = (surface_bits_per_pixel(surface) + 7) >> 3;
        d1 = static_cast<uint8_t *>(surface_data(surface));
        for (y = 0; y < surface_height(surface); y++) {
            memset(d1, 0x00, surface_width(surface) * bpp);
            d1 += surface_stride(surface);
        }
    }

    if (s->state & REDRAW_SEGMENTS) {
        /* set colors according to bpp */
        switch (surface_bits_per_pixel(surface)) {
        case 8:
            color_segment = rgb_to_pixel8(0xaa, 0xaa, 0xaa);
            color_led = rgb_to_pixel8(0x00, 0xff, 0x00);
            break;
        case 15:
            color_segment = rgb_to_pixel15(0xaa, 0xaa, 0xaa);
            color_led = rgb_to_pixel15(0x00, 0xff, 0x00);
            break;
        case 16:
            color_segment = rgb_to_pixel16(0xaa, 0xaa, 0xaa);
            color_led = rgb_to_pixel16(0x00, 0xff, 0x00);
            break;
        case 24:
            color_segment = rgb_to_pixel24(0xaa, 0xaa, 0xaa);
            color_led = rgb_to_pixel24(0x00, 0xff, 0x00);
            break;
        case 32:
            color_segment = rgb_to_pixel32(0xaa, 0xaa, 0xaa);
            color_led = rgb_to_pixel32(0x00, 0xff, 0x00);
            break;
        default:
            return;
        }

        /* display segments */
        drawHorizontalLine(surface, 40, 10, 40,
                             (s->segments & 0x02) ? color_segment : 0);
        drawVerticalLine(surface, 10, 10, 40,
                           (s->segments & 0x04) ? color_segment : 0);
        drawVerticalLine(surface, 10, 40, 70,
                           (s->segments & 0x08) ? color_segment : 0);
        drawHorizontalLine(surface, 70, 10, 40,
                             (s->segments & 0x10) ? color_segment : 0);
        drawVerticalLine(surface, 40, 40, 70,
                           (s->segments & 0x20) ? color_segment : 0);
        drawVerticalLine(surface, 40, 10, 40,
                           (s->segments & 0x40) ? color_segment : 0);
        drawHorizontalLine(surface, 10, 10, 40,
                             (s->segments & 0x80) ? color_segment : 0);

        /* display led */
        if (!(s->segments & 0x01)) {
            color_led = 0; /* black */
        }
        drawHorizontalLine(surface, 68, 50, 50, color_led);
        drawHorizontalLine(surface, 69, 49, 51, color_led);
        drawHorizontalLine(surface, 70, 48, 52, color_led);
        drawHorizontalLine(surface, 71, 49, 51, color_led);
        drawHorizontalLine(surface, 72, 50, 50, color_led);
    }

    s->state = REDRAW_NONE;
    dpy_gfx_update_full(s->con);
}

void LedState::invalidateDisplay(void *opaque)
{
    LedState *s = static_cast<LedState *>(opaque);
    s->state = static_cast<screen_state_t>(s->state | REDRAW_SEGMENTS | REDRAW_BACKGROUND);
}

void LedState::textUpdate(void *opaque, console_ch_t *chardata)
{
    LedState *s = static_cast<LedState *>(opaque);
    char buf[3];

    dpy_text_cursor(s->con, -1, -1);
    qemu_console_resize(s->con, 2, 1);

    /* TODO: draw the segments */
    snprintf(buf, 3, "%02hhx", s->segments);
    console_write_ch(chardata++, ATTR2CHTYPE(buf[0], QEMU_COLOR_BLUE,
                                             QEMU_COLOR_BLACK, 1));
    console_write_ch(chardata++, ATTR2CHTYPE(buf[1], QEMU_COLOR_BLUE,
                                             QEMU_COLOR_BLACK, 1));

    dpy_text_update(s->con, 0, 0, 2, 1);
}

int LedState::postLoad(void *opaque, int version_id)
{
    /* force refresh */
    LedState::invalidateDisplay(opaque);

    return 0;
}

static const VMStateDescription vmstate_jazz_led = {
    .name = "jazz-led",
    .version_id = 0,
    .minimum_version_id = 0,
    .post_load = LedState::postLoad,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(segments, LedState),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps jazz_led_ops = {
    .invalidate  = LedState::invalidateDisplay,
    .gfx_update  = LedState::updateDisplay,
    .text_update = LedState::textUpdate,
};

void LedState::initfn(Object *obj)
{
    LedState *s = reinterpret_cast<LedState *>(obj);
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(obj);

    memory_region_init_io(&s->iomem, obj, &led_ops, s, "led", 1);
    sysbus_init_mmio(dev, &s->iomem);
}

static void jazz_led_init(Object *obj)
{
    LedState *s = reinterpret_cast<LedState *>(obj);
    s->initfn(obj);
}

void LedState::realize(DeviceState *dev, Error **errp)
{
    LedState *s = reinterpret_cast<LedState *>(dev);

    s->con = graphic_console_init(dev, 0, &jazz_led_ops, s);
}

static void jazz_led_realize(DeviceState *dev, Error **errp)
{
    LedState *s = reinterpret_cast<LedState *>(dev);
    s->realize(dev, errp);
}

void LedState::reset(DeviceState *d)
{
    LedState *s = reinterpret_cast<LedState *>(d);

    s->segments = 0;
    s->state = static_cast<screen_state_t>(REDRAW_SEGMENTS | REDRAW_BACKGROUND);
    qemu_console_resize(s->con, 60, 80);
}

static void jazz_led_reset(DeviceState *d)
{
    LedState *s = reinterpret_cast<LedState *>(d);
    s->reset(d);
}

void LedState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Jazz LED display",
    dc->vmsd = &vmstate_jazz_led;
    device_class_set_legacy_reset(dc, jazz_led_reset);
    dc->realize = jazz_led_realize;
}

static const TypeInfo jazz_led_info = {
    .name          = TYPE_JAZZ_LED,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LedState),
    .instance_init = jazz_led_init,
    .class_init    = LedState::classInit,
};

static void jazz_led_register(void)
{
    type_register_static(&jazz_led_info);
}

type_init(jazz_led_register);
