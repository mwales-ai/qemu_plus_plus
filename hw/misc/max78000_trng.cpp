/*
 * MAX78000 True Random Number Generator
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
#include "hw/misc/max78000_trng.h"
#include "qemu/guest-random.h"
#include "qom/cpp/object.h"

static uint64_t max78000_trng_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    uint32_t data;

    Max78000TrngState *s = static_cast<Max78000TrngState *>(opaque);
    switch (addr) {
    case CTRL:
        return s->ctrl;

    case STATUS:
        return 1;

    case DATA:
        /*
         * When interrupts are enabled, reading random data should cause a
         * new interrupt to be generated; since there's always a random number
         * available, we could qemu_set_irq(s->irq, s->ctrl & RND_IE). Because
         * of how trng_write is set up, this is always a noop, so don't
         */
        qemu_guest_getrandom_nofail(&data, sizeof(data));
        return data;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"
            HWADDR_PRIx "\n", __func__, addr);
        break;
    }
    return 0;
}

static void max78000_trng_write(void *opaque, hwaddr addr,
                    uint64_t val64, unsigned int size)
{
    Max78000TrngState *s = static_cast<Max78000TrngState *>(opaque);
    uint32_t val = val64;
    switch (addr) {
    case CTRL:
        /* TODO: implement AES keygen */
        s->ctrl = val;

        /*
         * This device models random number generation as taking 0 time.
         * A new random number is always available, so the condition for the
         * RND interrupt is always fulfilled; we can just set irq to 1.
         */
        if (val & RND_IE) {
            qemu_set_irq(s->irq, 1);
        } else{
            qemu_set_irq(s->irq, 0);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"
            HWADDR_PRIx "\n", __func__, addr);
        break;
    }
}

static void max78000_trng_reset_hold(Object *obj, ResetType type)
{
    Max78000TrngState *s = MAX78000_TRNG(obj);
    s->ctrl = 0;
    s->status = 0;
    s->data = 0;
}

static const MemoryRegionOps max78000_trng_ops = {
    .read = max78000_trng_read,
    .write = max78000_trng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const VMStateField vmstate_max78000_trng_vmstate_fields[] = {
        VMSTATE_UINT32(ctrl, Max78000TrngState),
        VMSTATE_UINT32(status, Max78000TrngState),
        VMSTATE_UINT32(data, Max78000TrngState),
        VMSTATE_END_OF_LIST()
    };

static const VMStateDescription max78000_trng_vmstate = {
    .name = TYPE_MAX78000_TRNG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_max78000_trng_vmstate_fields,
};

void Max78000TrngState::init()
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    sysbus_init_irq(sbd, &irq);
    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &max78000_trng_ops, this,
                          TYPE_MAX78000_TRNG, 0x1000);
    sysbus_init_mmio(sbd, &mmio);
}

void Max78000TrngState::classInit(DeviceClass *dc)
{
    ResettableClass *rc = reinterpret_cast<ResettableClass *>(dc);

    rc->phases.hold = max78000_trng_reset_hold;
    dc->vmsd = &max78000_trng_vmstate;
}

REGISTER_QEMU_DEVICE(Max78000TrngState, TYPE_MAX78000_TRNG,
                     TYPE_SYS_BUS_DEVICE)
