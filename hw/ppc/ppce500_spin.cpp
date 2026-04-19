/*
 * QEMU PowerPC e500v2 ePAPR spinning code
 *
 * Copyright (C) 2011 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Author: Alexander Graf, <agraf@suse.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * This code is not really a device, but models an interface that usually
 * firmware takes care of. It's used when QEMU plays the role of firmware.
 *
 * Specification:
 *
 * https://www.power.org/resources/downloads/Power_ePAPR_APPROVED_v1.1.pdf
 *
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "system/hw_accel.h"
#include "hw/ppc/ppc.h"
#include "e500.h"
#include "qom/object.h"

#define MAX_CPUS 32

typedef struct spin_info {
    uint64_t addr;
    uint64_t r3;
    uint32_t resv;
    uint32_t pir;
    uint64_t reserved;
} QEMU_PACKED SpinInfo;

#define TYPE_E500_SPIN "e500-spin"
OBJECT_DECLARE_SIMPLE_TYPE(SpinState, E500_SPIN)

static void spin_kick(CPUState *cs, run_on_cpu_data data)
{
    CPUPPCState *env = cpu_env(cs);
    SpinInfo *curspin = static_cast<SpinInfo *>(data.host_ptr);
    hwaddr map_start, map_size = 64 * MiB;
    ppcmas_tlb_t *tlb = booke206_get_tlbm(env, 1, 0, 1);

    cpu_synchronize_state(cs);
    stl_p(&curspin->pir, env->spr[SPR_BOOKE_PIR]);
    env->nip = ldq_p(&curspin->addr) & (map_size - 1);
    env->gpr[3] = ldq_p(&curspin->r3);
    env->gpr[4] = 0;
    env->gpr[5] = 0;
    env->gpr[6] = 0;
    env->gpr[7] = map_size;
    env->gpr[8] = 0;
    env->gpr[9] = 0;

    map_start = ldq_p(&curspin->addr) & ~(map_size - 1);
    /* create initial mapping */
    booke206_set_tlb(tlb, 0, map_start, map_size);
    tlb->mas2 |= MAS2_M;
#ifdef CONFIG_KVM
    env->tlb_dirty = true;
#endif

    cs->halted = 0;
    cs->exception_index = -1;
    cpu_resume(cs);
}

struct SpinState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SpinInfo spin[MAX_CPUS];

    void reset()
    {
        int i;

        for (i = 0; i < MAX_CPUS; i++) {
            SpinInfo *info = &spin[i];

            stl_p(&info->pir, i);
            stq_p(&info->r3, i);
            stq_p(&info->addr, 1);
        }
    }

    static void write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned len)
    {
        SpinState *s = static_cast<SpinState *>(opaque);
        int env_idx = addr / sizeof(SpinInfo);
        CPUState *cpu;
        SpinInfo *curspin = &s->spin[env_idx];
        uint8_t *curspin_p = reinterpret_cast<uint8_t *>(curspin);

        cpu = qemu_get_cpu(env_idx);
        if (cpu == NULL) {
            /* Unknown CPU */
            return;
        }

        if (cpu->cpu_index == 0) {
            /* primary CPU doesn't spin */
            return;
        }

        curspin_p = &curspin_p[addr % sizeof(SpinInfo)];
        switch (len) {
        case 1:
            stb_p(curspin_p, value);
            break;
        case 2:
            stw_p(curspin_p, value);
            break;
        case 4:
            stl_p(curspin_p, value);
            break;
        }

        if (!(ldq_p(&curspin->addr) & 1)) {
            /* run CPU */
            run_on_cpu(cpu, spin_kick, RUN_ON_CPU_HOST_PTR(curspin));
        }
    }

    static uint64_t read(void *opaque, hwaddr addr, unsigned len)
    {
        SpinState *s = static_cast<SpinState *>(opaque);
        uint8_t *spin_p = &(reinterpret_cast<uint8_t *>(s->spin))[addr];

        switch (len) {
        case 1:
            return ldub_p(spin_p);
        case 2:
            return lduw_p(spin_p);
        case 4:
            return ldl_p(spin_p);
        default:
            hw_error("ppce500: unexpected %s with len = %u", __func__, len);
        }
    }

    static const MemoryRegionOps ops;

    void init()
    {
        memory_region_init_io(&iomem, OBJECT(this), &ops, this,
                              "e500 spin pv device", sizeof(SpinInfo) * MAX_CPUS);
        sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);
    }
};

const MemoryRegionOps SpinState::ops = {
    .read = SpinState::read,
    .write = SpinState::write,
    .endianness = DEVICE_BIG_ENDIAN,
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(SpinState, TYPE_E500_SPIN, TYPE_SYS_BUS_DEVICE)
