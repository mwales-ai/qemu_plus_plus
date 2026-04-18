/*
 *  Copyright (c) 2018-2021 Bastian Koppelmann Paderborn University
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/tricore/tricore_testdevice.h"

static void tricore_testdevice_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    if (value != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "Test %" PRIu64 " failed!\n", value);
    }
    exit(value);
}

static uint64_t tricore_testdevice_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    return 0xdeadbeef;
}

void TriCoreTestDeviceState::reset()
{
}

static const MemoryRegionOps tricore_testdevice_ops = {
    .read = tricore_testdevice_read,
    .write = tricore_testdevice_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void TriCoreTestDeviceState::init()
{
    memory_region_init_io(&iomem, OBJECT(this), &tricore_testdevice_ops, this,
                          "tricore_testdevice", 0x4);
}

void TriCoreTestDeviceState::classInit(DeviceClass *dc)
{
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(TriCoreTestDeviceState, TYPE_TRICORE_TESTDEVICE, TYPE_SYS_BUS_DEVICE)
