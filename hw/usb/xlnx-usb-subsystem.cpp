/*
 * QEMU model of the Xilinx usb subsystem
 *
 * Copyright (c) 2020 Xilinx Inc. Sai Pavan Boddu <sai.pava.boddu@xilinx.com>
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
#include "hw/sysbus.h"
#include "hw/register.h"
#include "qemu/bitops.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/usb/xlnx-usb-subsystem.h"

void VersalUsb2::realize(Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);
    Error *err = NULL;

    sysbus_realize(SYS_BUS_DEVICE(&dwc3), &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_realize(SYS_BUS_DEVICE(&usb2Ctrl), &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_init_mmio(sbd, &dwc3_mr);
    sysbus_init_mmio(sbd, &usb2Ctrl_mr);
    qdev_pass_gpios(DEVICE(&dwc3.sysbus_xhci), DEVICE(this), SYSBUS_DEVICE_GPIO_IRQ);
}

void VersalUsb2::init()
{
    Object *obj = OBJECT(this);

    object_initialize_child(obj, "versal.dwc3", &dwc3, TYPE_USB_DWC3);
    object_initialize_child(obj, "versal.usb2-ctrl", &usb2Ctrl,
                            TYPE_XILINX_VERSAL_USB2_CTRL_REGS);
    memory_region_init_alias(&dwc3_mr, obj, "versal.dwc3_alias",
                             &dwc3.iomem, 0, DWC3_SIZE);
    memory_region_init_alias(&usb2Ctrl_mr, obj, "versal.usb2Ctrl_alias",
                             &usb2Ctrl.iomem, 0, USB2_REGS_R_MAX * 4);
    qdev_alias_all_properties(DEVICE(&dwc3), obj);
    qdev_alias_all_properties(DEVICE(&dwc3.sysbus_xhci), obj);
    object_property_add_alias(obj, "dma", OBJECT(&dwc3.sysbus_xhci), "dma");
}

void VersalUsb2::classInit(DeviceClass *dc)
{
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VersalUsb2, TYPE_XILINX_VERSAL_USB2, TYPE_SYS_BUS_DEVICE)
