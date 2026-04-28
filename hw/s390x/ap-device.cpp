/*
 * Adjunct Processor (AP) matrix device
 *
 * Copyright 2018 IBM Corp.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/s390x/ap-device.h"
#include "qom/cpp/object.h"

void APDevice::classInit(DeviceClass *dc)
{
    dc->desc = "AP device class";
    dc->hotpluggable = false;
}

REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS(APDevice, TYPE_AP_DEVICE, TYPE_DEVICE)
