/*
 * MAX78000 Instruction Cache
 *
 * Copyright (c) 2025 Jackson Donaldson <jcksn@duck.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "trace.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "hw/misc/max78000_icc.h"
#include "qom/cpp/object.h"


static uint64_t max78000_icc_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    Max78000IccState *s = static_cast<Max78000IccState *>(opaque);
    switch (addr) {
    case ICC_INFO:
        return s->info;

    case ICC_SZ:
        return s->sz;

    case ICC_CTRL:
        return s->ctrl;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;

    }
}

static void max78000_icc_write(void *opaque, hwaddr addr,
                    uint64_t val64, unsigned int size)
{
    Max78000IccState *s = static_cast<Max78000IccState *>(opaque);

    switch (addr) {
    case ICC_CTRL:
        s->ctrl = 0x10000 | (val64 & 1);
        break;

    case ICC_INVALIDATE:
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps max78000_icc_ops = {
    .read = max78000_icc_read,
    .write = max78000_icc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const VMStateField vmstate_max78000_icc_vmstate_fields[] = {
        VMSTATE_UINT32(info, Max78000IccState),
        VMSTATE_UINT32(sz, Max78000IccState),
        VMSTATE_UINT32(ctrl, Max78000IccState),
        VMSTATE_END_OF_LIST()
    };

static const VMStateDescription max78000_icc_vmstate = {
    .name = TYPE_MAX78000_ICC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_max78000_icc_vmstate_fields,
};

static void max78000_icc_reset_hold(Object *obj, ResetType type)
{
    Max78000IccState *s = MAX78000_ICC(obj);
    s->info = 0;
    s->sz = 0x10000010;
    s->ctrl = 0x10000;
}

void Max78000IccState::init()
{
    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &max78000_icc_ops, this,
                          TYPE_MAX78000_ICC, 0x800);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &mmio);
}

void Max78000IccState::classInit(DeviceClass *dc)
{
    ResettableClass *rc = reinterpret_cast<ResettableClass *>(dc);

    rc->phases.hold = max78000_icc_reset_hold;
    dc->vmsd = &max78000_icc_vmstate;
}

REGISTER_QEMU_DEVICE(Max78000IccState, TYPE_MAX78000_ICC, TYPE_SYS_BUS_DEVICE)
