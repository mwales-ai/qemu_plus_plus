/*
 * Allwinner H3 System Control emulation
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
#include "hw/misc/allwinner-h3-sysctrl.h"
#include "qom/cpp/object.h"

/* System Control register offsets */
enum {
    REG_VER               = 0x24,  /* Version */
    REG_EMAC_PHY_CLK      = 0x30,  /* EMAC PHY Clock */
};

#define REG_INDEX(offset)   (offset / sizeof(uint32_t))

/* System Control register reset values */
enum {
    REG_VER_RST           = 0x0,
    REG_EMAC_PHY_CLK_RST  = 0x58000,
};

static uint64_t allwinner_h3_sysctrl_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    const AwH3SysCtrlState *s = AW_H3_SYSCTRL(opaque);
    const uint32_t idx = REG_INDEX(offset);

    if (idx >= AW_H3_SYSCTRL_REGS_NUM) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    }

    return s->regs[idx];
}

static void allwinner_h3_sysctrl_write(void *opaque, hwaddr offset,
                                       uint64_t val, unsigned size)
{
    AwH3SysCtrlState *s = AW_H3_SYSCTRL(opaque);
    const uint32_t idx = REG_INDEX(offset);

    if (idx >= AW_H3_SYSCTRL_REGS_NUM) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return;
    }

    switch (offset) {
    case REG_VER:       /* Version */
        break;
    default:
        s->regs[idx] = (uint32_t) val;
        break;
    }
}

static const MemoryRegionOps allwinner_h3_sysctrl_ops = {
    .read = allwinner_h3_sysctrl_read,
    .write = allwinner_h3_sysctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = { .min_access_size = 4, },
};

void AwH3SysCtrlState::reset()
{
    /* Set default values for registers */
    regs[REG_INDEX(REG_VER)] = REG_VER_RST;
    regs[REG_INDEX(REG_EMAC_PHY_CLK)] = REG_EMAC_PHY_CLK_RST;
}

void AwH3SysCtrlState::init()
{
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &allwinner_h3_sysctrl_ops, this,
                          TYPE_AW_H3_SYSCTRL, 4 * KiB);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &iomem);
}

static const VMStateDescription allwinner_h3_sysctrl_vmstate = {
    .name = "allwinner-h3-sysctrl",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AwH3SysCtrlState, AW_H3_SYSCTRL_REGS_NUM),
        VMSTATE_END_OF_LIST()
    }
};

void AwH3SysCtrlState::classInit(DeviceClass *dc)
{
    dc->vmsd = &allwinner_h3_sysctrl_vmstate;
}

REGISTER_QEMU_DEVICE(AwH3SysCtrlState, TYPE_AW_H3_SYSCTRL,
                     TYPE_SYS_BUS_DEVICE)
