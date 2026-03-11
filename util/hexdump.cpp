/*
* Helper to hexdump a buffer
 *
 * Copyright (c) 2013 Red Hat, Inc.
 * Copyright (c) 2013 Gerd Hoffmann <kraxel@redhat.com>
 * Copyright (c) 2013 Peter Crosthwaite <peter.crosthwaite@xilinx.com>
 * Copyright (c) 2013 Xilinx, Inc
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/cutils.h"
#include "qemu/host-utils.h"
}

static inline char hexdump_nibble(unsigned x)
{
    return (x < 10 ? '0' : 'a' - 10) + x;
}

static size_t hexdump_line_length(size_t buf_len, size_t unit_len,
                                  size_t block_len)
{
    size_t est = buf_len * 2;
    if (unit_len) {
        est += buf_len / unit_len;
    }
    if (block_len) {
        est += buf_len / block_len;
    }
    return est;
}

extern "C"
GString *qemu_hexdump_line(GString *str, const void *vbuf, size_t len,
                           size_t unit_len, size_t block_len)
{
    const uint8_t *buf = static_cast<const uint8_t *>(vbuf);
    size_t u, b;

    if (str == NULL) {
        /* Estimate the length of the output to avoid reallocs. */
        str = g_string_sized_new(hexdump_line_length(len, unit_len, block_len)
                                 + 1);
    }

    for (u = 0, b = 0; len; u++, b++, len--, buf++) {
        uint8_t c;

        if (unit_len && u == unit_len) {
            g_string_append_c(str, ' ');
            u = 0;
        }
        if (block_len && b == block_len) {
            g_string_append_c(str, ' ');
            b = 0;
        }

        c = *buf;
        g_string_append_c(str, hexdump_nibble(c / 16));
        g_string_append_c(str, hexdump_nibble(c % 16));
    }

    return str;
}

static void asciidump_line(char *line, const void *bufptr, size_t len)
{
    const char *buf = static_cast<const char *>(bufptr);

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        if (c < ' ' || c > '~') {
            c = '.';
        }
        *line++ = c;
    }
    *line = '\0';
}

#define QEMU_HEXDUMP_LINE_BYTES 16
#define QEMU_HEXDUMP_UNIT 1
#define QEMU_HEXDUMP_BLOCK 4

extern "C"
void qemu_hexdump(FILE *fp, const char *prefix,
                  const void *bufptr, size_t size)
{
    int width = hexdump_line_length(QEMU_HEXDUMP_LINE_BYTES,
                                    QEMU_HEXDUMP_UNIT,
                                    QEMU_HEXDUMP_BLOCK);
    GString *str = g_string_sized_new(width + 1);
    char ascii[QEMU_HEXDUMP_LINE_BYTES + 1];
    const uint8_t *byteptr = static_cast<const uint8_t *>(bufptr);
    size_t b, len;

    for (b = 0; b < size; b += len) {
        len = MIN(size - b, QEMU_HEXDUMP_LINE_BYTES);

        g_string_truncate(str, 0);
        qemu_hexdump_line(str, byteptr + b, len,
                          QEMU_HEXDUMP_UNIT, QEMU_HEXDUMP_BLOCK);
        asciidump_line(ascii, byteptr + b, len);

        fprintf(fp, "%s: %04zx: %-*s %s\n", prefix, b, width, str->str, ascii);
    }

    g_string_free(str, TRUE);
}

extern "C"
void qemu_hexdump_to_buffer(char *__restrict__ buffer, size_t buffer_size,
                            const uint8_t *__restrict__ data, size_t data_size)
{
    size_t i;
    uint64_t required_buffer_size;
    bool overflow = umul64_overflow(data_size, 2, &required_buffer_size);
    overflow |= uadd64_overflow(required_buffer_size, 1, &required_buffer_size);
    assert(!overflow && buffer_size >= required_buffer_size);

    for (i = 0; i < data_size; i++) {
        uint8_t val = data[i];
        *(buffer++) = hexdump_nibble(val >> 4);
        *(buffer++) = hexdump_nibble(val & 0xf);
    }
    *buffer = '\0';
}
