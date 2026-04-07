/*
 * QEMU HP Artist Emulation
 *
 * Copyright (c) 2019-2022 Sven Schnelle <svens@stackframe.org>
 * Copyright (c) 2022 Helge Deller <deller@gmx.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/loader.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "trace.h"
#include "framebuffer.h"
#include "qom/object.h"

#define TYPE_ARTIST "artist"
OBJECT_DECLARE_SIMPLE_TYPE(ARTISTState, ARTIST)

struct vram_buffer {
    MemoryRegion mr;
    uint8_t *data;
    unsigned int size;
    unsigned int width;
    unsigned int height;
};

struct ARTISTState {
    SysBusDevice parent_obj;

    QemuConsole *con;
    MemoryRegion vram_mem;
    MemoryRegion mem_as_root;
    MemoryRegion reg;
    MemoryRegionSection fbsection;

    void *vram_int_mr;
    AddressSpace as;

    struct vram_buffer vram_buffer[16];

    bool disable;
    uint16_t width;
    uint16_t height;
    uint16_t depth;

    uint32_t fg_color;
    uint32_t bg_color;

    uint32_t vram_char_y;
    uint32_t vram_bitmask;

    uint32_t vram_start;
    uint32_t vram_pos;

    uint32_t vram_size;

    uint32_t blockmove_source;
    uint32_t blockmove_dest;
    uint32_t blockmove_size;

    uint32_t line_size;
    uint32_t line_end;
    uint32_t line_xy;
    uint32_t line_pattern_start;
    uint32_t line_pattern_skip;

    uint32_t cursor_pos;
    uint32_t cursor_cntrl;

    uint32_t cursor_height;
    uint32_t cursor_width;

    uint32_t plane_mask;

    uint32_t reg_100080;
    uint32_t horiz_backporch;
    uint32_t active_lines_low;
    uint32_t misc_video;
    uint32_t misc_ctrl;

    uint32_t dst_bm_access;
    uint32_t src_bm_access;
    uint32_t control_plane;
    uint32_t transfer_data;
    uint32_t image_bitmap_op;

    uint32_t font_write1;
    uint32_t font_write2;
    uint32_t font_write_pos_y;

    int draw_line_pattern;

    void realize(Error **errp);
    void initfn();
    static void realizeWrapper(DeviceState *dev, Error **errp);
    static void initWrapper(Object *obj);
    static void resetWrapper(DeviceState *qdev);
    static void classInit(ObjectClass *klass, const void *data);

    /* Static MMIO callbacks */
    static void vramWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);
    static uint64_t vramRead(void *opaque, hwaddr addr, unsigned size);
    static void regWrite(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size);
    static uint64_t regRead(void *opaque, hwaddr addr, unsigned size);

    /* Static display callbacks */
    static void updateDisplay(void *opaque);
    static void invalidateDisplay(void *opaque);
    static void drawLineCallback(void *opaque, uint8_t *d, const uint8_t *src,
                                 int width, int pitch);

    /* Static VMState callback */
    static int postLoad(void *opaque, int version_id);

    /* Private static utility */
    static const char *regName(uint64_t addr);
    static int16_t getX(uint32_t reg);
    static int16_t getY(uint32_t reg);
    static void invalidateLines(struct vram_buffer *buf, int starty, int height);
    static void combineWriteReg(hwaddr addr, uint64_t val, int size, void *out);
    static uint64_t combineReadReg(hwaddr addr, int size, void *in);

    /* Instance methods (private helpers) */
    int vramWriteBufidx();
    int vramReadBufidx();
    struct vram_buffer *vramReadBuffer();
    struct vram_buffer *vramWriteBuffer();
    uint8_t getColor();
    artist_rop_t getOp();
    void rop8(struct vram_buffer *buf, unsigned int offset, uint8_t val);
    void getCursorPos(int *x, int *y);
    bool cursorVisible();
    void invalidateCursor();
    void blockMove(unsigned int source_x, unsigned int source_y,
                   unsigned int dest_x, unsigned int dest_y,
                   unsigned int width, unsigned int height);
    void fillWindow(unsigned int startx, unsigned int starty,
                    unsigned int width, unsigned int height);
    void drawLine(unsigned int x1, unsigned int y1,
                  unsigned int x2, unsigned int y2,
                  bool update_start, int skip_pix, int max_pix);
    void drawLinePatternStart();
    void drawLinePatternNext();
    void drawLineSize(bool update_start);
    void drawLineXy(bool update_start);
    void drawLineEnd(bool update_start);
    void fontWrite16(uint16_t val);
    void fontWrite(uint32_t val);
    void vramWrite4(struct vram_buffer *buf, uint32_t offset, uint32_t data);
    void vramWrite32(struct vram_buffer *buf, uint32_t offset, int size,
                     uint32_t data, int fg, int bg);
    int getVramOffset(struct vram_buffer *buf, int pos, int posy);
    int vramBitWrite(uint32_t pos, int posy, uint32_t data, int size);
    void drawCursor();
    bool screenEnabled();
    void createBuffer(const char *name, hwaddr *offset, unsigned int idx,
                      int width, int height);
};

/* hardware allows up to 64x64, but we emulate 32x32 only. */
#define NGLE_MAX_SPRITE_SIZE    32

typedef enum {
    ARTIST_BUFFER_AP = 1,
    ARTIST_BUFFER_OVERLAY = 2,
    ARTIST_BUFFER_CURSOR1 = 6,
    ARTIST_BUFFER_CURSOR2 = 7,
    ARTIST_BUFFER_ATTRIBUTE = 13,
    ARTIST_BUFFER_CMAP = 15,
} artist_buffer_t;

typedef enum {
    VRAM_IDX = 0x1004a0,
    VRAM_BITMASK = 0x1005a0,
    VRAM_WRITE_INCR_X = 0x100600,
    VRAM_WRITE_INCR_X2 = 0x100604,
    VRAM_WRITE_INCR_Y = 0x100620,
    VRAM_START = 0x100800,
    BLOCK_MOVE_SIZE = 0x100804,
    BLOCK_MOVE_SOURCE = 0x100808,
    TRANSFER_DATA = 0x100820,
    FONT_WRITE_INCR_Y = 0x1008a0,
    VRAM_START_TRIGGER = 0x100a00,
    VRAM_SIZE_TRIGGER = 0x100a04,
    FONT_WRITE_START = 0x100aa0,
    BLOCK_MOVE_DEST_TRIGGER = 0x100b00,
    BLOCK_MOVE_SIZE_TRIGGER = 0x100b04,
    LINE_XY = 0x100ccc,
    PATTERN_LINE_START = 0x100ecc,
    LINE_SIZE = 0x100e04,
    LINE_END = 0x100e44,
    DST_SRC_BM_ACCESS = 0x118000,
    DST_BM_ACCESS = 0x118004,
    SRC_BM_ACCESS = 0x118008,
    CONTROL_PLANE = 0x11800c,
    FG_COLOR = 0x118010,
    BG_COLOR = 0x118014,
    PLANE_MASK = 0x118018,
    IMAGE_BITMAP_OP = 0x11801c,
    CURSOR_POS = 0x300100,      /* reg17 */
    CURSOR_CTRL = 0x300104,     /* reg18 */
    MISC_VIDEO = 0x300218,      /* reg21 */
    MISC_CTRL = 0x300308,       /* reg27 */
    HORIZ_BACKPORCH = 0x300200, /* reg19 */
    ACTIVE_LINES_LOW = 0x300208,/* reg20 */
    FIFO1 = 0x300008,           /* reg34 */
    FIFO2 = 0x380008,
} artist_reg_t;

typedef enum {
    ARTIST_ROP_CLEAR = 0,
    ARTIST_ROP_COPY = 3,
    ARTIST_ROP_XOR = 6,
    ARTIST_ROP_NOT_DST = 10,
    ARTIST_ROP_SET = 15,
} artist_rop_t;

#define REG_NAME(_x) case _x: return " "#_x;
const char *ARTISTState::regName(uint64_t addr)
{
    switch ((artist_reg_t)addr) {
    REG_NAME(VRAM_IDX);
    REG_NAME(VRAM_BITMASK);
    REG_NAME(VRAM_WRITE_INCR_X);
    REG_NAME(VRAM_WRITE_INCR_X2);
    REG_NAME(VRAM_WRITE_INCR_Y);
    REG_NAME(VRAM_START);
    REG_NAME(BLOCK_MOVE_SIZE);
    REG_NAME(BLOCK_MOVE_SOURCE);
    REG_NAME(FG_COLOR);
    REG_NAME(BG_COLOR);
    REG_NAME(PLANE_MASK);
    REG_NAME(VRAM_START_TRIGGER);
    REG_NAME(VRAM_SIZE_TRIGGER);
    REG_NAME(BLOCK_MOVE_DEST_TRIGGER);
    REG_NAME(BLOCK_MOVE_SIZE_TRIGGER);
    REG_NAME(TRANSFER_DATA);
    REG_NAME(CONTROL_PLANE);
    REG_NAME(IMAGE_BITMAP_OP);
    REG_NAME(DST_SRC_BM_ACCESS);
    REG_NAME(DST_BM_ACCESS);
    REG_NAME(SRC_BM_ACCESS);
    REG_NAME(CURSOR_POS);
    REG_NAME(CURSOR_CTRL);
    REG_NAME(HORIZ_BACKPORCH);
    REG_NAME(ACTIVE_LINES_LOW);
    REG_NAME(MISC_VIDEO);
    REG_NAME(MISC_CTRL);
    REG_NAME(LINE_XY);
    REG_NAME(PATTERN_LINE_START);
    REG_NAME(LINE_SIZE);
    REG_NAME(LINE_END);
    REG_NAME(FONT_WRITE_INCR_Y);
    REG_NAME(FONT_WRITE_START);
    REG_NAME(FIFO1);
    REG_NAME(FIFO2);
    }
    return "";
}
#undef REG_NAME

/* artist has a fixed line length of 2048 bytes. */
#define ADDR_TO_Y(addr) extract32(addr, 11, 11)
#define ADDR_TO_X(addr) extract32(addr, 0, 11)

int16_t ARTISTState::getX(uint32_t reg)
{
    return reg >> 16;
}

int16_t ARTISTState::getY(uint32_t reg)
{
    return reg & 0xffff;
}

void ARTISTState::invalidateLines(struct vram_buffer *buf,
                                  int starty, int height)
{
    int start = starty * buf->width;
    int size;

    if (starty + height > buf->height) {
        height = buf->height - starty;
    }

    size = height * buf->width;

    if (start + size <= buf->size) {
        memory_region_set_dirty(&buf->mr, start, size);
    }
}

int ARTISTState::vramWriteBufidx()
{
    return (dst_bm_access >> 12) & 0x0f;
}

int ARTISTState::vramReadBufidx()
{
    return (src_bm_access >> 12) & 0x0f;
}

struct vram_buffer *ARTISTState::vramReadBuffer()
{
    return &vram_buffer[vramReadBufidx()];
}

struct vram_buffer *ARTISTState::vramWriteBuffer()
{
    return &vram_buffer[vramWriteBufidx()];
}

uint8_t ARTISTState::getColor()
{
    if (image_bitmap_op & 2) {
        return fg_color;
    } else {
        return bg_color;
    }
}

artist_rop_t ARTISTState::getOp()
{
    return static_cast<artist_rop_t>((image_bitmap_op >> 8) & 0xf);
}

void ARTISTState::rop8(struct vram_buffer *buf,
                       unsigned int offset, uint8_t val)
{
    const artist_rop_t op = getOp();
    uint8_t plane_mask;
    uint8_t *dst;

    if (offset >= buf->size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rop8 offset:%u bufsize:%u\n", offset, buf->size);
        return;
    }
    dst = buf->data + offset;
    plane_mask = this->plane_mask & 0xff;

    switch (op) {
    case ARTIST_ROP_CLEAR:
        *dst &= ~plane_mask;
        break;

    case ARTIST_ROP_COPY:
        *dst = (*dst & ~plane_mask) | (val & plane_mask);
        break;

    case ARTIST_ROP_XOR:
        *dst ^= val & plane_mask;
        break;

    case ARTIST_ROP_NOT_DST:
        *dst ^= plane_mask;
        break;

    case ARTIST_ROP_SET:
        *dst |= plane_mask;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: unsupported rop %d\n", __func__, op);
        break;
    }
}

void ARTISTState::getCursorPos(int *x, int *y)
{
    /*
     * The emulated Artist graphic is like a CRX graphic, and as such
     * it's usually fixed at 1280x1024 pixels.
     * Other resolutions may work, but no guarantee.
     */

    unsigned int hbp_times_vi, horizBackPorch;
    int16_t xHi, xLo;
    const int videoInterleave = 4;
    const int pipelineDelay = 4;

    /* ignore if uninitialized */
    if (cursor_pos == 0) {
        *x = *y = 0;
        return;
    }

    /*
     * Calculate X position based on backporch and interleave values.
     * Based on code from Xorg X11R6.6
     */
    horizBackPorch = ((horiz_backporch & 0xff0000) >> 16) +
                     ((horiz_backporch & 0xff00) >> 8) + 2;
    hbp_times_vi = horizBackPorch * videoInterleave;
    xHi = cursor_pos >> 19;
    *x = ((xHi + pipelineDelay) * videoInterleave) - hbp_times_vi;

    xLo = (cursor_pos >> 16) & 0x07;
    *x += ((xLo - hbp_times_vi) & (videoInterleave - 1)) + 8 - 1;

    /* subtract cursor offset from cursor control register */
    *x -= (cursor_cntrl & 0xf0) >> 4;

    /* Calculate Y position */
    *y = height - getY(cursor_pos);
    *y -= (cursor_cntrl & 0x0f);

    if (*x > width) {
        *x = width;
    }

    if (*y > height) {
        *y = height;
    }
}

bool ARTISTState::cursorVisible()
{
    /* cursor is visible if bit 0x80 is set in cursor_cntrl */
    return cursor_cntrl & 0x80;
}

void ARTISTState::invalidateCursor()
{
    int x, y;

    if (!cursorVisible()) {
        return;
    }

    getCursorPos(&x, &y);
    invalidateLines(&vram_buffer[ARTIST_BUFFER_AP],
                    y, cursor_height);
}

void ARTISTState::blockMove(unsigned int source_x, unsigned int source_y,
                            unsigned int dest_x, unsigned int dest_y,
                            unsigned int width, unsigned int height)
{
    struct vram_buffer *buf;
    int line, endline, lineincr, startcolumn, endcolumn, columnincr, column;
    unsigned int dst, src;

    trace_artist_block_move(source_x, source_y, dest_x, dest_y, width, height);

    if (control_plane != 0) {
        /* We don't support CONTROL_PLANE accesses */
        qemu_log_mask(LOG_UNIMP, "%s: CONTROL_PLANE: %08x\n", __func__,
                      control_plane);
        return;
    }

    buf = &vram_buffer[ARTIST_BUFFER_AP];
    if (height > buf->height) {
        height = buf->height;
    }
    if (width > buf->width) {
        width = buf->width;
    }

    if (dest_y > source_y) {
        /* move down */
        line = height - 1;
        endline = -1;
        lineincr = -1;
    } else {
        /* move up */
        line = 0;
        endline = height;
        lineincr = 1;
    }

    if (dest_x > source_x) {
        /* move right */
        startcolumn = width - 1;
        endcolumn = -1;
        columnincr = -1;
    } else {
        /* move left */
        startcolumn = 0;
        endcolumn = width;
        columnincr = 1;
    }

    for ( ; line != endline; line += lineincr) {
        src = source_x + ((line + source_y) * buf->width) + startcolumn;
        dst = dest_x + ((line + dest_y) * buf->width) + startcolumn;

        for (column = startcolumn; column != endcolumn; column += columnincr) {
            if (dst >= buf->size || src >= buf->size) {
                continue;
            }
            rop8(buf, dst, buf->data[src]);
            src += columnincr;
            dst += columnincr;
        }
    }

    invalidateLines(buf, dest_y, height);
}

void ARTISTState::fillWindow(unsigned int startx, unsigned int starty,
                             unsigned int width, unsigned int height)
{
    unsigned int offset;
    uint8_t color = getColor();
    struct vram_buffer *buf;
    int x, y;

    trace_artist_fill_window(startx, starty, width, height,
                             image_bitmap_op, control_plane);

    if (control_plane != 0) {
        /* We don't support CONTROL_PLANE accesses */
        qemu_log_mask(LOG_UNIMP, "%s: CONTROL_PLANE: %08x\n", __func__,
                      control_plane);
        return;
    }

    if (reg_100080 == 0x7d) {
        /*
         * Not sure what this register really does, but
         * 0x7d seems to enable autoincremt of the Y axis
         * by the current block move height.
         */
        height = getY(blockmove_size);
        vram_start += height;
    }

    buf = &vram_buffer[ARTIST_BUFFER_AP];

    for (y = starty; y < starty + (int)height; y++) {
        offset = y * this->width;

        for (x = startx; x < startx + (int)width; x++) {
            rop8(buf, offset + x, color);
        }
    }
    invalidateLines(buf, starty, height);
}

void ARTISTState::drawLine(unsigned int x1, unsigned int y1,
                           unsigned int x2, unsigned int y2,
                           bool update_start, int skip_pix, int max_pix)
{
    struct vram_buffer *buf = &vram_buffer[ARTIST_BUFFER_AP];
    uint8_t color;
    int dx, dy, t, e, x, y, incy, diago, horiz;
    bool c1;

    trace_artist_draw_line(x1, y1, x2, y2);

    if ((x1 >= buf->width && x2 >= buf->width) ||
        (y1 >= buf->height && y2 >= buf->height)) {
        return;
    }

    if (update_start) {
        vram_start = (x2 << 16) | y2;
    }

    if (x2 > x1) {
        dx = x2 - x1;
    } else {
        dx = x1 - x2;
    }
    if (y2 > y1) {
        dy = y2 - y1;
    } else {
        dy = y1 - y2;
    }

    c1 = false;
    if (dy > dx) {
        t = y2;
        y2 = x2;
        x2 = t;

        t = y1;
        y1 = x1;
        x1 = t;

        t = dx;
        dx = dy;
        dy = t;

        c1 = true;
    }

    if (x1 > x2) {
        t = y2;
        y2 = y1;
        y1 = t;

        t = x1;
        x1 = x2;
        x2 = t;
    }

    horiz = dy << 1;
    diago = (dy - dx) << 1;
    e = (dy << 1) - dx;

    if (y1 <= y2) {
        incy = 1;
    } else {
        incy = -1;
    }
    x = x1;
    y = y1;
    color = getColor();

    do {
        unsigned int ofs;

        if (c1) {
            ofs = x * width + y;
        } else {
            ofs = y * width + x;
        }

        if (skip_pix > 0) {
            skip_pix--;
        } else {
            rop8(buf, ofs, color);
        }

        if (e > 0) {
            y  += incy;
            e  += diago;
        } else {
            e += horiz;
        }
        x++;
    } while (x <= (int)x2 && (max_pix == -1 || --max_pix > 0));

    if (c1) {
        invalidateLines(buf, x1, x2 - x1);
    } else {
        invalidateLines(buf, y1 > y2 ? y2 : y1, x2 - x1);
    }
}

void ARTISTState::drawLinePatternStart()
{
    int startx = getX(vram_start);
    int starty = getY(vram_start);
    int endx = getX(blockmove_size);
    int endy = getY(blockmove_size);
    int pstart = line_pattern_start >> 16;

    drawLine(startx, starty, endx, endy, false, -1, pstart);
    line_pattern_skip = pstart;
}

void ARTISTState::drawLinePatternNext()
{
    int startx = getX(vram_start);
    int starty = getY(vram_start);
    int endx = getX(blockmove_size);
    int endy = getY(blockmove_size);
    int lxy = line_xy >> 16;

    drawLine(startx, starty, endx, endy, false, line_pattern_skip,
             line_pattern_skip + lxy);
    line_pattern_skip += lxy;
    image_bitmap_op ^= 2;
}

void ARTISTState::drawLineSize(bool update_start)
{
    int startx = getX(vram_start);
    int starty = getY(vram_start);
    int endx = getX(line_size);
    int endy = getY(line_size);

    drawLine(startx, starty, endx, endy, update_start, -1, -1);
}

void ARTISTState::drawLineXy(bool update_start)
{
    int startx = getX(vram_start);
    int starty = getY(vram_start);
    int sizex = getX(blockmove_size);
    int sizey = getY(blockmove_size);
    int linexy = line_xy >> 16;
    int endx, endy;

    endx = startx;
    endy = starty;

    if (sizex > 0) {
        endx = startx + linexy;
    }

    if (sizex < 0) {
        endx = startx;
        startx -= linexy;
    }

    if (sizey > 0) {
        endy = starty + linexy;
    }

    if (sizey < 0) {
        endy = starty;
        starty -= linexy;
    }

    if (startx < 0) {
        startx = 0;
    }

    if (endx < 0) {
        endx = 0;
    }

    if (starty < 0) {
        starty = 0;
    }

    if (endy < 0) {
        endy = 0;
    }

    drawLine(startx, starty, endx, endy, false, -1, -1);
}

void ARTISTState::drawLineEnd(bool update_start)
{
    int startx = getX(vram_start);
    int starty = getY(vram_start);
    int endx = getX(line_end);
    int endy = getY(line_end);

    drawLine(startx, starty, endx, endy, update_start, -1, -1);
}

void ARTISTState::fontWrite16(uint16_t val)
{
    struct vram_buffer *buf;
    uint32_t color = (image_bitmap_op & 2) ? fg_color : bg_color;
    uint16_t mask;
    int i;

    unsigned int startx = getX(vram_start);
    unsigned int starty = getY(vram_start) + font_write_pos_y;
    unsigned int offset = starty * width + startx;

    buf = &vram_buffer[ARTIST_BUFFER_AP];

    if (startx >= buf->width || starty >= buf->height ||
        offset + 16 >= buf->size) {
        return;
    }

    for (i = 0; i < 16; i++) {
        mask = 1 << (15 - i);
        if (val & mask) {
            rop8(buf, offset + i, color);
        } else {
            if (!(image_bitmap_op & 0x20000000)) {
                rop8(buf, offset + i, bg_color);
            }
        }
    }
    invalidateLines(buf, starty, 1);
}

void ARTISTState::fontWrite(uint32_t val)
{
    fontWrite16(val >> 16);
    if (++font_write_pos_y == getY(blockmove_size)) {
        vram_start += (blockmove_size & 0xffff0000);
        return;
    }

    fontWrite16(val & 0xffff);
    if (++font_write_pos_y == getY(blockmove_size)) {
        vram_start += (blockmove_size & 0xffff0000);
        return;
    }
}

void ARTISTState::combineWriteReg(hwaddr addr, uint64_t val, int size, void *out)
{
    /*
     * FIXME: is there a qemu helper for this?
     */

#if !HOST_BIG_ENDIAN
    addr ^= 3;
#endif

    switch (size) {
    case 1:
        *(uint8_t *)(out + (addr & 3)) = val;
        break;

    case 2:
        *(uint16_t *)(out + (addr & 2)) = val;
        break;

    case 4:
        *(uint32_t *)out = val;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "unsupported write size: %d\n", size);
    }
}

void ARTISTState::vramWrite4(struct vram_buffer *buf,
                             uint32_t offset, uint32_t data)
{
    int i;
    int mask = vram_bitmask >> 28;

    for (i = 0; i < 4; i++) {
        if (!(image_bitmap_op & 0x20000000) || (mask & 8)) {
            rop8(buf, offset + i, data >> 24);
            data <<= 8;
            mask <<= 1;
        }
    }
    memory_region_set_dirty(&buf->mr, offset, 3);
}

void ARTISTState::vramWrite32(struct vram_buffer *buf,
                              uint32_t offset, int size, uint32_t data,
                              int fg, int bg)
{
    uint32_t mask, vbitmask = vram_bitmask >> ((4 - size) * 8);
    int i, pix_count = size * 8;

    for (i = 0; i < pix_count && offset + i < buf->size; i++) {
        mask = 1 << (pix_count - 1 - i);

        if (!(image_bitmap_op & 0x20000000) || (vbitmask & mask)) {
            if (data & mask) {
                rop8(buf, offset + i, fg);
            } else {
                if (!(image_bitmap_op & 0x10000002)) {
                    rop8(buf, offset + i, bg);
                }
            }
        }
    }
    memory_region_set_dirty(&buf->mr, offset, pix_count);
}

int ARTISTState::getVramOffset(struct vram_buffer *buf, int pos, int posy)
{
    unsigned int posx, w;

    w = buf->width;
    posx = ADDR_TO_X(pos);
    posy += ADDR_TO_Y(pos);
    return posy * w + posx;
}

int ARTISTState::vramBitWrite(uint32_t pos, int posy,
                              uint32_t data, int size)
{
    struct vram_buffer *buf = vramWriteBuffer();

    switch (dst_bm_access >> 16) {
    case 0x3ba0:
    case 0xbbe0:
        vramWrite4(buf, pos, bswap32(data));
        pos += 4;
        break;

    case 0x1360: /* linux */
        vramWrite4(buf, getVramOffset(buf, pos, posy), data);
        pos += 4;
        break;

    case 0x13a0:
        vramWrite4(buf, getVramOffset(buf, pos >> 2, posy), data);
        pos += 16;
        break;

    case 0x2ea0:
        vramWrite32(buf, getVramOffset(buf, pos >> 2, posy),
                    size, data, fg_color, bg_color);
        pos += 4;
        break;

    case 0x28a0:
        vramWrite32(buf, getVramOffset(buf, pos >> 2, posy),
                    size, data, 1, 0);
        pos += 4;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown dst bm access %08x\n",
                      __func__, dst_bm_access);
        break;
    }

    if (vramWriteBufidx() == ARTIST_BUFFER_CURSOR1 ||
        vramWriteBufidx() == ARTIST_BUFFER_CURSOR2) {
        invalidateCursor();
    }
    return pos;
}

void ARTISTState::vramWrite(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    ARTISTState *s = static_cast<ARTISTState *>(opaque);

    s->vram_char_y = 0;
    trace_artist_vram_write(size, addr, val);
    s->vramBitWrite(addr, 0, val, size);
}

uint64_t ARTISTState::vramRead(void *opaque, hwaddr addr, unsigned size)
{
    ARTISTState *s = static_cast<ARTISTState *>(opaque);
    struct vram_buffer *buf;
    unsigned int offset;
    uint64_t val;

    buf = s->vramReadBuffer();
    if (!buf->size) {
        return 0;
    }

    offset = s->getVramOffset(buf, addr >> 2, 0);

    if (offset > buf->size) {
        return 0;
    }

    switch (s->src_bm_access >> 16) {
    case 0x3ba0:
        val = *(uint32_t *)(buf->data + offset);
        break;

    case 0x13a0:
    case 0x2ea0:
        val = bswap32(*(uint32_t *)(buf->data + offset));
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown src bm access %08x\n",
                      __func__, s->dst_bm_access);
        val = -1ULL;
        break;
    }
    trace_artist_vram_read(size, addr, val);
    return val;
}

void ARTISTState::regWrite(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    ARTISTState *s = static_cast<ARTISTState *>(opaque);
    int w, h;
    uint64_t oldval;

    trace_artist_reg_write(size, addr, regName(addr & ~3ULL), val);

    switch (addr & ~3ULL) {
    case 0x100080:
        combineWriteReg(addr, val, size, &s->reg_100080);
        break;

    case FG_COLOR:
        combineWriteReg(addr, val, size, &s->fg_color);
        break;

    case BG_COLOR:
        combineWriteReg(addr, val, size, &s->bg_color);
        break;

    case VRAM_BITMASK:
        combineWriteReg(addr, val, size, &s->vram_bitmask);
        break;

    case VRAM_WRITE_INCR_Y:
        s->vramBitWrite(s->vram_pos, s->vram_char_y++, val, size);
        break;

    case VRAM_WRITE_INCR_X:
    case VRAM_WRITE_INCR_X2:
        s->vram_pos = s->vramBitWrite(s->vram_pos, s->vram_char_y, val, size);
        break;

    case VRAM_IDX:
        combineWriteReg(addr, val, size, &s->vram_pos);
        s->vram_char_y = 0;
        s->draw_line_pattern = 0;
        break;

    case VRAM_START:
        combineWriteReg(addr, val, size, &s->vram_start);
        s->draw_line_pattern = 0;
        break;

    case VRAM_START_TRIGGER:
        combineWriteReg(addr, val, size, &s->vram_start);
        s->fillWindow(getX(s->vram_start),
                      getY(s->vram_start),
                      getX(s->blockmove_size),
                      getY(s->blockmove_size));
        break;

    case VRAM_SIZE_TRIGGER:
        combineWriteReg(addr, val, size, &s->vram_size);

        if (size == 2 && !(addr & 2)) {
            h = getY(s->blockmove_size);
        } else {
            h = getY(s->vram_size);
        }

        if (size == 2 && (addr & 2)) {
            w = getX(s->blockmove_size);
        } else {
            w = getX(s->vram_size);
        }

        s->fillWindow(getX(s->vram_start),
                       getY(s->vram_start),
                       w, h);
        break;

    case LINE_XY:
        combineWriteReg(addr, val, size, &s->line_xy);
        if (s->draw_line_pattern) {
            s->drawLinePatternNext();
        } else {
            s->drawLineXy(true);
        }
        break;

    case PATTERN_LINE_START:
        combineWriteReg(addr, val, size, &s->line_pattern_start);
        s->draw_line_pattern = 1;
        s->drawLinePatternStart();
        break;

    case LINE_SIZE:
        combineWriteReg(addr, val, size, &s->line_size);
        s->drawLineSize(true);
        break;

    case LINE_END:
        combine_write_reg(addr, val, size, &s->line_end);
        draw_line_end(s, true);
        break;

    case BLOCK_MOVE_SIZE:
        combine_write_reg(addr, val, size, &s->blockmove_size);
        break;

    case BLOCK_MOVE_SOURCE:
        combine_write_reg(addr, val, size, &s->blockmove_source);
        break;

    case BLOCK_MOVE_DEST_TRIGGER:
        combine_write_reg(addr, val, size, &s->blockmove_dest);

        block_move(s, artist_get_x(s->blockmove_source),
                   artist_get_y(s->blockmove_source),
                   artist_get_x(s->blockmove_dest),
                   artist_get_y(s->blockmove_dest),
                   artist_get_x(s->blockmove_size),
                   artist_get_y(s->blockmove_size));
        break;

    case BLOCK_MOVE_SIZE_TRIGGER:
        combine_write_reg(addr, val, size, &s->blockmove_size);

        block_move(s,
                   artist_get_x(s->blockmove_source),
                   artist_get_y(s->blockmove_source),
                   artist_get_x(s->vram_start),
                   artist_get_y(s->vram_start),
                   artist_get_x(s->blockmove_size),
                   artist_get_y(s->blockmove_size));
        break;

    case PLANE_MASK:
        combine_write_reg(addr, val, size, &s->plane_mask);
        break;

    case DST_SRC_BM_ACCESS:
        combine_write_reg(addr, val, size, &s->dst_bm_access);
        combine_write_reg(addr, val, size, &s->src_bm_access);
        break;

    case DST_BM_ACCESS:
        combine_write_reg(addr, val, size, &s->dst_bm_access);
        break;

    case SRC_BM_ACCESS:
        combine_write_reg(addr, val, size, &s->src_bm_access);
        break;

    case CONTROL_PLANE:
        combine_write_reg(addr, val, size, &s->control_plane);
        break;

    case TRANSFER_DATA:
        combine_write_reg(addr, val, size, &s->transfer_data);
        break;

    case HORIZ_BACKPORCH:
        /* overwrite HP-UX settings to fix X cursor position. */
        val = (NGLE_MAX_SPRITE_SIZE << 16) + (NGLE_MAX_SPRITE_SIZE << 8);
        combine_write_reg(addr, val, size, &s->horiz_backporch);
        break;

    case ACTIVE_LINES_LOW:
        combine_write_reg(addr, val, size, &s->active_lines_low);
        break;

    case MISC_VIDEO:
        oldval = s->misc_video;
        combine_write_reg(addr, val, size, &s->misc_video);
        /* Invalidate and hide screen if graphics signal is turned off. */
        if (((oldval & 0x0A000000) == 0x0A000000) &&
            ((val & 0x0A000000) != 0x0A000000)) {
            artist_invalidate(s);
        }
        /* Invalidate and redraw screen if graphics signal is turned back on. */
        if (((oldval & 0x0A000000) != 0x0A000000) &&
            ((val & 0x0A000000) == 0x0A000000)) {
            artist_invalidate(s);
        }
        break;

    case MISC_CTRL:
        combine_write_reg(addr, val, size, &s->misc_ctrl);
        break;

    case CURSOR_POS:
        artist_invalidate_cursor(s);
        combine_write_reg(addr, val, size, &s->cursor_pos);
        artist_invalidate_cursor(s);
        break;

    case CURSOR_CTRL:
        combine_write_reg(addr, val, size, &s->cursor_cntrl);
        break;

    case IMAGE_BITMAP_OP:
        combine_write_reg(addr, val, size, &s->image_bitmap_op);
        break;

    case FONT_WRITE_INCR_Y:
        combine_write_reg(addr, val, size, &s->font_write1);
        font_write(s, s->font_write1);
        break;

    case FONT_WRITE_START:
        combine_write_reg(addr, val, size, &s->font_write2);
        s->font_write_pos_y = 0;
        font_write(s, s->font_write2);
        break;

    case 300104:
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register: reg=%08" HWADDR_PRIx
                      " val=%08" PRIx64 " size=%d\n",
                      __func__, addr, val, size);
        break;
    }
}

static uint64_t combine_read_reg(hwaddr addr, int size, void *in)
{
    /*
     * FIXME: is there a qemu helper for this?
     */

#if !HOST_BIG_ENDIAN
    addr ^= 3;
#endif

    switch (size) {
    case 1:
        return *(uint8_t *)(in + (addr & 3));

    case 2:
        return *(uint16_t *)(in + (addr & 2));

    case 4:
        return *(uint32_t *)in;

    default:
        qemu_log_mask(LOG_UNIMP, "unsupported read size: %d\n", size);
        return 0;
    }
}

static uint64_t artist_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    ARTISTState *s = static_cast<ARTISTState *>(opaque);
    uint32_t val = 0;

    switch (addr & ~3ULL) {
        /* Unknown status registers */
    case 0:
        break;

    case 0x211110:
        val = (s->width << 16) | s->height;
        if (s->depth == 1) {
            val |= 1 << 31;
        }
        break;

    case 0x100000:
    case 0x300000:
    case 0x300004:
    case 0x380000:
        break;

    case FIFO1:
    case FIFO2:
        /*
         * FIFO ready flag. we're not emulating the FIFOs
         * so we're always ready
         */
        val = 0x10;
        break;

    case HORIZ_BACKPORCH:
        val = s->horiz_backporch;
        break;

    case ACTIVE_LINES_LOW:
        val = s->active_lines_low;
        /* activeLinesLo for cursor is in reg20.b.b0 */
        val &= ~(0xff << 24);
        val |= (s->height & 0xff) << 24;
        break;

    case MISC_VIDEO:
        /* emulate V-blank */
        s->misc_video ^= 0x00040000;
        /* activeLinesHi for cursor is in reg21.b.b2 */
        val = s->misc_video;
        val &= ~0xff00UL;
        val |= (s->height & 0xff00);
        break;

    case MISC_CTRL:
        val = s->misc_ctrl;
        break;

    case 0x30023c:
        val = 0xac4ffdac;
        break;

    case 0x380004:
        /* magic number detected by SeaBIOS-hppa */
        val = s->disable ? 0 : 0x6dc20006;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register: %08" HWADDR_PRIx
                      " size %d\n", __func__, addr, size);
        break;
    }
    val = combine_read_reg(addr, size, &val);
    trace_artist_reg_read(size, addr, artist_reg_name(addr & ~3ULL), val);
    return val;
}

static const MemoryRegionOps artist_reg_ops = {
    .read = artist_reg_read,
    .write = artist_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4, },
};

static const MemoryRegionOps artist_vram_ops = {
    .read = artist_vram_read,
    .write = artist_vram_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4, },
};

static void artist_draw_cursor(ARTISTState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *data = (uint32_t *)surface_data(surface);
    struct vram_buffer *cursor0, *cursor1 , *buf;
    int cx, cy, cursor_pos_x, cursor_pos_y;

    if (!cursor_visible(s)) {
        return;
    }

    cursor0 = &s->vram_buffer[ARTIST_BUFFER_CURSOR1];
    cursor1 = &s->vram_buffer[ARTIST_BUFFER_CURSOR2];
    buf = &s->vram_buffer[ARTIST_BUFFER_AP];

    artist_get_cursor_pos(s, &cursor_pos_x, &cursor_pos_y);

    for (cy = 0; cy < s->cursor_height; cy++) {

        for (cx = 0; cx < s->cursor_width; cx++) {

            if (cursor_pos_y + cy < 0 ||
                cursor_pos_x + cx < 0 ||
                cursor_pos_y + cy > buf->height - 1 ||
                cursor_pos_x + cx > buf->width) {
                continue;
            }

            int dstoffset = (cursor_pos_y + cy) * s->width +
                (cursor_pos_x + cx);

            if (cursor0->data[cy * cursor0->width + cx]) {
                data[dstoffset] = 0;
            } else {
                if (cursor1->data[cy * cursor1->width + cx]) {
                    data[dstoffset] = 0xffffff;
                }
            }
        }
    }
}

static bool artist_screen_enabled(ARTISTState *s)
{
    /*  We could check for (s->misc_ctrl & 0x00800000) too... */
    return ((s->misc_video & 0x0A000000) == 0x0A000000);
}

static void artist_draw_line(void *opaque, uint8_t *d, const uint8_t *src,
                             int width, int pitch)
{
    ARTISTState *s = reinterpret_cast<ARTISTState *>(opaque);
    uint32_t *cmap, *data = (uint32_t *)d;
    int x;

    if (!artist_screen_enabled(s)) {
        /* clear screen */
        memset(data, 0, s->width * sizeof(uint32_t));
        return;
    }

    cmap = (uint32_t *)(s->vram_buffer[ARTIST_BUFFER_CMAP].data + 0x400);

    for (x = 0; x < s->width; x++) {
        *data++ = cmap[*src++];
    }
}

static void artist_update_display(void *opaque)
{
    ARTISTState *s = static_cast<ARTISTState *>(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last;

    framebuffer_update_display(surface, &s->fbsection, s->width, s->height,
                               s->width, s->width * 4, 0, 0, artist_draw_line,
                               s, &first, &last);

    artist_draw_cursor(s);

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, s->width, last - first + 1);
    }
}

static void artist_invalidate(void *opaque)
{
    ARTISTState *s = reinterpret_cast<ARTISTState *>(opaque);
    struct vram_buffer *buf = &s->vram_buffer[ARTIST_BUFFER_AP];

    memory_region_set_dirty(&buf->mr, 0, buf->size);
}

static const GraphicHwOps artist_ops = {
    .invalidate  = artist_invalidate,
    .gfx_update = artist_update_display,
};

void ARTISTState::initfn()
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    memory_region_init_io(&reg, OBJECT(this), &artist_reg_ops, this,
                          "artist.reg", 4 * MiB);
    memory_region_init_io(&vram_mem, OBJECT(this), &artist_vram_ops, this,
                          "artist.vram", 8 * MiB);
    sysbus_init_mmio(sbd, &reg);
    sysbus_init_mmio(sbd, &vram_mem);
}

void ARTISTState::initWrapper(Object *obj)
{
    ARTISTState *s = reinterpret_cast<ARTISTState *>(obj);
    s->initfn();
}

static void artist_create_buffer(ARTISTState *s, const char *name,
                                 hwaddr *offset, unsigned int idx,
                                 int width, int height)
{
    struct vram_buffer *buf = s->vram_buffer + idx;

    memory_region_init_ram(&buf->mr, OBJECT(s), name, width * height,
                           &error_fatal);
    memory_region_add_subregion_overlap(&s->mem_as_root, *offset, &buf->mr, 0);

    buf->data = memory_region_get_ram_ptr(&buf->mr);
    buf->size = height * width;
    buf->width = width;
    buf->height = height;

    *offset += buf->size;
}

void ARTISTState::realize(Error **errp)
{
    struct vram_buffer *buf;
    hwaddr offset = 0;

    if (width > 2048 || height > 2048) {
        error_report("artist: screen size can not exceed 2048 x 2048 pixel.");
        width = MIN(width, 2048);
        height = MIN(height, 2048);
    }

    if (width < 640 || height < 480) {
        error_report("artist: minimum screen size is 640 x 480 pixel.");
        width = MAX(width, 640);
        height = MAX(height, 480);
    }

    memory_region_init(&mem_as_root, OBJECT(this), "artist", ~0ull);
    address_space_init(&as, &mem_as_root, "artist");

    artist_create_buffer(this, "cmap", &offset, ARTIST_BUFFER_CMAP, 2048, 4);
    artist_create_buffer(this, "ap", &offset, ARTIST_BUFFER_AP,
                         width, height);
    artist_create_buffer(this, "cursor1", &offset, ARTIST_BUFFER_CURSOR1, 64, 64);
    artist_create_buffer(this, "cursor2", &offset, ARTIST_BUFFER_CURSOR2, 64, 64);
    artist_create_buffer(this, "attribute", &offset, ARTIST_BUFFER_ATTRIBUTE,
                         64, 64);

    buf = &vram_buffer[ARTIST_BUFFER_AP];
    framebuffer_update_memory_section(&fbsection, &buf->mr, 0,
                                      buf->width, buf->height);
    /*
     * Artist cursor max size
     */
    cursor_height = NGLE_MAX_SPRITE_SIZE;
    cursor_width = NGLE_MAX_SPRITE_SIZE;

    /*
     * These two registers are not initialized by seabios's STI implementation.
     * Initialize them here to sane values so artist also works with older
     * (not-fixed) seabios versions.
     */
    image_bitmap_op = 0x23000300;
    plane_mask = 0xff;

    /* enable screen */
    misc_video |= 0x0A000000;
    misc_ctrl  |= 0x00800000;

    con = graphic_console_init(reinterpret_cast<DeviceState *>(this), 0, &artist_ops, this);
    qemu_console_resize(con, width, height);
}

void ARTISTState::realizeWrapper(DeviceState *dev, Error **errp)
{
    ARTISTState *s = reinterpret_cast<ARTISTState *>(dev);
    s->realize(errp);
}

static int vmstate_artist_post_load(void *opaque, int version_id)
{
    artist_invalidate(opaque);
    return 0;
}

static const VMStateDescription vmstate_artist = {
    .name = "artist",
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = vmstate_artist_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(height, ARTISTState),
        VMSTATE_UINT16(width, ARTISTState),
        VMSTATE_UINT16(depth, ARTISTState),
        VMSTATE_UINT32(fg_color, ARTISTState),
        VMSTATE_UINT32(bg_color, ARTISTState),
        VMSTATE_UINT32(vram_char_y, ARTISTState),
        VMSTATE_UINT32(vram_bitmask, ARTISTState),
        VMSTATE_UINT32(vram_start, ARTISTState),
        VMSTATE_UINT32(vram_pos, ARTISTState),
        VMSTATE_UINT32(vram_size, ARTISTState),
        VMSTATE_UINT32(blockmove_source, ARTISTState),
        VMSTATE_UINT32(blockmove_dest, ARTISTState),
        VMSTATE_UINT32(blockmove_size, ARTISTState),
        VMSTATE_UINT32(line_size, ARTISTState),
        VMSTATE_UINT32(line_end, ARTISTState),
        VMSTATE_UINT32(line_xy, ARTISTState),
        VMSTATE_UINT32(cursor_pos, ARTISTState),
        VMSTATE_UINT32(cursor_cntrl, ARTISTState),
        VMSTATE_UINT32(cursor_height, ARTISTState),
        VMSTATE_UINT32(cursor_width, ARTISTState),
        VMSTATE_UINT32(plane_mask, ARTISTState),
        VMSTATE_UINT32(reg_100080, ARTISTState),
        VMSTATE_UINT32(horiz_backporch, ARTISTState),
        VMSTATE_UINT32(active_lines_low, ARTISTState),
        VMSTATE_UINT32(misc_video, ARTISTState),
        VMSTATE_UINT32(misc_ctrl, ARTISTState),
        VMSTATE_UINT32(dst_bm_access, ARTISTState),
        VMSTATE_UINT32(src_bm_access, ARTISTState),
        VMSTATE_UINT32(control_plane, ARTISTState),
        VMSTATE_UINT32(transfer_data, ARTISTState),
        VMSTATE_UINT32(image_bitmap_op, ARTISTState),
        VMSTATE_UINT32(font_write1, ARTISTState),
        VMSTATE_UINT32(font_write2, ARTISTState),
        VMSTATE_UINT32(font_write_pos_y, ARTISTState),
        VMSTATE_BOOL(disable, ARTISTState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property artist_properties[] = {
    DEFINE_PROP_UINT16("width",        ARTISTState, width, 1280),
    DEFINE_PROP_UINT16("height",       ARTISTState, height, 1024),
    DEFINE_PROP_UINT16("depth",        ARTISTState, depth, 8),
    DEFINE_PROP_BOOL("disable",        ARTISTState, disable, false),
};

void ARTISTState::resetWrapper(DeviceState *qdev)
{
}

void ARTISTState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ARTISTState::realizeWrapper;
    dc->vmsd = &vmstate_artist;
    device_class_set_legacy_reset(dc, ARTISTState::resetWrapper);
    device_class_set_props(dc, artist_properties);
}

static const TypeInfo artist_info = {
    .name          = TYPE_ARTIST,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ARTISTState),
    .instance_init = ARTISTState::initWrapper,
    .class_init    = ARTISTState::classInit,
};

static void artist_register_types(void)
{
    type_register_static(&artist_info);
}

type_init(artist_register_types)
