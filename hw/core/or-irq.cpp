/*
 * QEMU IRQ/GPIO common code.
 *
 * Copyright (c) 2016 Alistair Francis <alistair@alistair23.me>.
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
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

#include "hw/irq.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static void or_irq_handler(void *opaque, int n, int level)
{
    OrIRQState *s = OR_IRQ(opaque);
    int or_level = 0;
    int i;

    s->levels[n] = level;

    for (i = 0; i < s->num_lines; i++) {
        or_level |= s->levels[i];
    }

    qemu_set_irq(s->out_irq, or_level);
}

void OrIRQState::reset()
{
    int i;

    for (i = 0; i < MAX_OR_LINES; i++) {
        levels[i] = false;
    }
}

void OrIRQState::realize(Error **errp)
{
    assert(num_lines <= MAX_OR_LINES);

    qdev_init_gpio_in(DEVICE(this), or_irq_handler, num_lines);
}

void OrIRQState::init()
{
    qdev_init_gpio_out(DEVICE(this), &out_irq, 1);
}

/* The original version of this device had a fixed 16 entries in its
 * VMState array; devices with more inputs than this need to
 * migrate the extra lines via a subsection.
 * The subsection migrates as much of the levels[] array as is needed
 * (including repeating the first 16 elements), to avoid the awkwardness
 * of splitting it in two to meet the requirements of VMSTATE_VARRAY_UINT16.
 */
#define OLD_MAX_OR_LINES 16
#if MAX_OR_LINES < OLD_MAX_OR_LINES
#error MAX_OR_LINES must be at least 16 for migration compatibility
#endif

static bool vmstate_extras_needed(void *opaque)
{
    OrIRQState *s = OR_IRQ(opaque);

    return s->num_lines >= OLD_MAX_OR_LINES;
}

static const VMStateField vmstate_or_irq_extras_fields[] = {
    {
        .name       = "levels",
        .offset     = vmstate_offset_varray(OrIRQState, levels, bool),
        .size       = sizeof(bool),
        .num_offset = vmstate_offset_value(OrIRQState, num_lines, uint16_t),
        .info       = &vmstate_info_bool,
        .flags      = VMS_VARRAY_UINT16,
    },
    {
        .flags = VMS_END,
    },
};

static const VMStateDescription vmstate_or_irq_extras = {
    .name = "or-irq-extras",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_extras_needed,
    .fields = vmstate_or_irq_extras_fields,
};

static const VMStateField vmstate_or_irq_fields[] = {
    {
        .name       = "levels",
        .offset     = vmstate_offset_sub_array(OrIRQState, levels, bool, 0),
        .size       = sizeof(bool),
        .num        = OLD_MAX_OR_LINES,
        .info       = &vmstate_info_bool,
        .flags      = VMS_ARRAY,
    },
    {
        .flags = VMS_END,
    },
};

static const VMStateDescription * const vmstate_or_irq_subsections[] = {
    &vmstate_or_irq_extras,
    NULL
};

static const VMStateDescription vmstate_or_irq = {
    .name = TYPE_OR_IRQ,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_or_irq_fields,
    .subsections = vmstate_or_irq_subsections,
};

static const Property or_irq_properties[] = {
    DEFINE_PROP_UINT16("num-lines", OrIRQState, num_lines, 1),
};

void OrIRQState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, or_irq_properties);
    dc->vmsd = &vmstate_or_irq;
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(OrIRQState, TYPE_OR_IRQ, TYPE_DEVICE)
