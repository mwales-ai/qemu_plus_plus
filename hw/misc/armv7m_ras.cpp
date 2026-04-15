/*
 * Arm M-profile RAS (Reliability, Availability and Serviceability) block
 *
 * Copyright (c) 2021 Linaro Limited
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 or
 *  (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/misc/armv7m_ras.h"
#include "qemu/log.h"
#include "qom/cpp/object.h"

static MemTxResult ras_read(void *opaque, hwaddr addr,
                            uint64_t *data, unsigned size,
                            MemTxAttrs attrs)
{
    if (attrs.user) {
        return MEMTX_ERROR;
    }

    switch (addr) {
    case 0xe10: /* ERRIIDR */
        /* architect field = Arm; product/variant/revision 0 */
        *data = 0x43b;
        break;
    case 0xfc8: /* ERRDEVID */
        /* Minimal RAS: we implement 0 error record indexes */
        *data = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "Read RAS register offset 0x%x\n",
                      (uint32_t)addr);
        *data = 0;
        break;
    }
    return MEMTX_OK;
}

static MemTxResult ras_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size,
                             MemTxAttrs attrs)
{
    if (attrs.user) {
        return MEMTX_ERROR;
    }

    switch (addr) {
    default:
        qemu_log_mask(LOG_UNIMP, "Write to RAS register offset 0x%x\n",
                      (uint32_t)addr);
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ras_ops = {
    .read_with_attrs = ras_read,
    .write_with_attrs = ras_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};


void ARMv7MRAS::init()
{
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this), &ras_ops,
                          this, "armv7m-ras", 0x1000);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

REGISTER_QEMU_DEVICE(ARMv7MRAS, TYPE_ARMV7M_RAS, TYPE_SYS_BUS_DEVICE)
