/*
 * IRQ splitter device.
 *
 * Copyright (c) 2018 Linaro Limited.
 * Written by Peter Maydell
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

#include "hw/core/split-irq.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"

static void split_irq_handler(void *opaque, int n, int level)
{
    SplitIRQ *s = SPLIT_IRQ(opaque);
    int i;

    for (i = 0; i < s->num_lines; i++) {
        qemu_set_irq(s->out_irq[i], level);
    }
}

void SplitIRQ::init()
{
    qdev_init_gpio_in(DEVICE(this), split_irq_handler, 1);
}

void SplitIRQ::realize(Error **errp)
{
    if (num_lines < 1 || num_lines >= MAX_SPLIT_LINES) {
        error_setg(errp,
                   "IRQ splitter number of lines %d is not between 1 and %d",
                   num_lines, MAX_SPLIT_LINES);
        return;
    }

    qdev_init_gpio_out(DEVICE(this), out_irq, num_lines);
}

static const Property split_irq_properties[] = {
    DEFINE_PROP_UINT16("num-lines", SplitIRQ, num_lines, 1),
};

void SplitIRQ::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, split_irq_properties);
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(SplitIRQ, TYPE_SPLIT_IRQ, TYPE_DEVICE)
