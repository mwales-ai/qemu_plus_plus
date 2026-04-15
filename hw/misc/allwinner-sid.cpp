/*
 * Allwinner Security ID emulation
 *
 * Copyright (C) 2019 Niek Linnenbank <nieklinnenbank@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/allwinner-sid.h"
#include "qom/cpp/object.h"
#include "trace.h"

/* SID register offsets */
enum {
    REG_PRCTL = 0x40,   /* Control */
    REG_RDKEY = 0x60,   /* Read Key */
};

/* SID register flags */
enum {
    REG_PRCTL_WRITE   = 0x0002, /* Unknown write flag */
    REG_PRCTL_OP_LOCK = 0xAC00, /* Lock operation */
};

static uint64_t allwinner_sid_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    const AwSidState *s = AW_SID(opaque);
    uint64_t val = 0;

    switch (offset) {
    case REG_PRCTL:    /* Control */
        val = s->control;
        break;
    case REG_RDKEY:    /* Read Key */
        val = s->rdkey;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    }

    trace_allwinner_sid_read(offset, val, size);

    return val;
}

static void allwinner_sid_write(void *opaque, hwaddr offset,
                                uint64_t val, unsigned size)
{
    AwSidState *s = AW_SID(opaque);

    trace_allwinner_sid_write(offset, val, size);

    switch (offset) {
    case REG_PRCTL:    /* Control */
        s->control = val;

        if ((s->control & REG_PRCTL_OP_LOCK) &&
            (s->control & REG_PRCTL_WRITE)) {
            uint32_t id = s->control >> 16;

            if (id <= sizeof(QemuUUID) - sizeof(s->rdkey)) {
                s->rdkey = ldl_be_p(&s->identifier.data[id]);
            }
        }
        s->control &= ~REG_PRCTL_WRITE;
        break;
    case REG_RDKEY:    /* Read Key */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        break;
    }
}

static const MemoryRegionOps allwinner_sid_ops = {
    .read = allwinner_sid_read,
    .write = allwinner_sid_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = { .min_access_size = 4, },
};

void AwSidState::reset()
{
    control = 0;
    rdkey = 0;
}

void AwSidState::init()
{
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &allwinner_sid_ops, this, TYPE_AW_SID, 1 * KiB);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

static const Property allwinner_sid_properties[] = {
    DEFINE_PROP_UUID_NODEFAULT("identifier", AwSidState, identifier),
};

static const VMStateDescription allwinner_sid_vmstate = {
    .name = "allwinner-sid",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, AwSidState),
        VMSTATE_UINT32(rdkey, AwSidState),
        VMSTATE_UINT8_ARRAY_V(identifier.data, AwSidState, sizeof(QemuUUID), 1),
        VMSTATE_END_OF_LIST()
    }
};

void AwSidState::classInit(DeviceClass *dc)
{
    dc->vmsd = &allwinner_sid_vmstate;
    device_class_set_props(dc, allwinner_sid_properties);
}

REGISTER_QEMU_DEVICE(AwSidState, TYPE_AW_SID, TYPE_SYS_BUS_DEVICE)
