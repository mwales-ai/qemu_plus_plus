/*
 * Cortex-A9MPCore Snoop Control Unit (SCU) emulation.
 *
 * Copyright (c) 2009 CodeSourcery.
 * Copyright (c) 2011 Linaro Limited.
 * Written by Paul Brook, Peter Maydell.
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "hw/misc/a9scu.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/cpp/object.h"

#define A9_SCU_CPU_MAX  4

static uint64_t a9_scu_read(void *opaque, hwaddr offset,
                            unsigned size)
{
    A9SCUState *s = (A9SCUState *)opaque;
    switch (offset) {
    case 0x00: /* Control */
        return s->control;
    case 0x04: /* Configuration */
        return (((1 << s->num_cpu) - 1) << 4) | (s->num_cpu - 1);
    case 0x08: /* CPU Power Status */
        return s->status;
    case 0x0c: /* Invalidate All Registers In Secure State */
        return 0;
    case 0x40: /* Filtering Start Address Register */
    case 0x44: /* Filtering End Address Register */
        /* RAZ/WI, like an implementation with only one AXI master */
        return 0;
    case 0x50: /* SCU Access Control Register */
    case 0x54: /* SCU Non-secure Access Control Register */
        /* unimplemented, fall through */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Unsupported offset 0x%" HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }
}

static void a9_scu_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    A9SCUState *s = (A9SCUState *)opaque;

    switch (offset) {
    case 0x00: /* Control */
        s->control = value & 1;
        break;
    case 0x4: /* Configuration: RO */
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: /* Power Control */
        s->status = value;
        break;
    case 0x0c: /* Invalidate All Registers In Secure State */
        /* no-op as we do not implement caches */
        break;
    case 0x40: /* Filtering Start Address Register */
    case 0x44: /* Filtering End Address Register */
        /* RAZ/WI, like an implementation with only one AXI master */
        break;
    case 0x50: /* SCU Access Control Register */
    case 0x54: /* SCU Non-secure Access Control Register */
        /* unimplemented, fall through */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Unsupported offset 0x%" HWADDR_PRIx
                                 " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps a9_scu_ops = {
    .read = a9_scu_read,
    .write = a9_scu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void A9SCUState::reset()
{
    control = 0;
}

void A9SCUState::realize(Error **errp)
{
    if (!num_cpu || num_cpu > A9_SCU_CPU_MAX) {
        error_setg(errp, "Illegal CPU count: %u", num_cpu);
        return;
    }

    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &a9_scu_ops, this, "a9-scu", 0x100);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

static const VMStateField vmstate_a9_scu_fields[] = {
    VMSTATE_UINT32(control, A9SCUState),
    VMSTATE_UINT32(status, A9SCUState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_a9_scu = {
    .name = "a9-scu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_a9_scu_fields,
};

static const Property a9_scu_properties[] = {
    DEFINE_PROP_UINT32("num-cpu", A9SCUState, num_cpu, 1),
};

void A9SCUState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, a9_scu_properties);
    dc->vmsd = &vmstate_a9_scu;
}

REGISTER_QEMU_DEVICE(A9SCUState, TYPE_A9_SCU, TYPE_SYS_BUS_DEVICE)
