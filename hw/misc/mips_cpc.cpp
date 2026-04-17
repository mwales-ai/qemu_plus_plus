/*
 * Cluster Power Controller emulation
 *
 * Copyright (c) 2016 Imagination Technologies
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
 */

#include "qemu/osdep.h"

extern "C" {
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "hw/misc/mips_cpc.h"
#include "hw/qdev-properties.h"
}

#include "cpu.h"
#include "qom/cpp/object.h"

static inline uint64_t cpc_vp_run_mask(MIPSCPCState *cpc)
{
    return (1ULL << cpc->num_vp) - 1;
}

static void mips_cpu_reset_async_work(CPUState *cs, run_on_cpu_data data)
{
    MIPSCPCState *cpc = static_cast<MIPSCPCState *>(data.host_ptr);

    cpu_reset(cs);
    cs->halted = 0;
    cpc->vp_running |= 1ULL << cs->cpu_index;
}

static void cpc_run_vp(MIPSCPCState *cpc, uint64_t vp_run)
{
    CPUState *cs = first_cpu;

    CPU_FOREACH(cs) {
        uint64_t i = 1ULL << cs->cpu_index;
        if (i & vp_run & ~cpc->vp_running) {
            /*
             * To avoid racing with a CPU we are just kicking off.
             * We do the final bit of preparation for the work in
             * the target CPUs context.
             */
            async_safe_run_on_cpu(cs, mips_cpu_reset_async_work,
                                  RUN_ON_CPU_HOST_PTR(cpc));
        }
    }
}

static void cpc_stop_vp(MIPSCPCState *cpc, uint64_t vp_stop)
{
    CPUState *cs = first_cpu;

    CPU_FOREACH(cs) {
        uint64_t i = 1ULL << cs->cpu_index;
        if (i & vp_stop & cpc->vp_running) {
            cpu_interrupt(cs, CPU_INTERRUPT_HALT);
            cpc->vp_running &= ~i;
        }
    }
}

static void cpc_write(void *opaque, hwaddr offset, uint64_t data,
                      unsigned size)
{
    MIPSCPCState *s = static_cast<MIPSCPCState *>(opaque);

    switch (offset) {
    case CPC_CL_BASE_OFS + CPC_VP_RUN_OFS:
    case CPC_CO_BASE_OFS + CPC_VP_RUN_OFS:
        cpc_run_vp(s, data & cpc_vp_run_mask(s));
        break;
    case CPC_CL_BASE_OFS + CPC_VP_STOP_OFS:
    case CPC_CO_BASE_OFS + CPC_VP_STOP_OFS:
        cpc_stop_vp(s, data & cpc_vp_run_mask(s));
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: Bad offset 0x%x\n",  __func__, (int)offset);
        break;
    }
}

static uint64_t cpc_read(void *opaque, hwaddr offset, unsigned size)
{
    MIPSCPCState *s = static_cast<MIPSCPCState *>(opaque);

    switch (offset) {
    case CPC_CL_BASE_OFS + CPC_VP_RUNNING_OFS:
    case CPC_CO_BASE_OFS + CPC_VP_RUNNING_OFS:
        return s->vp_running;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: Bad offset 0x%x\n",  __func__, (int)offset);
        return 0;
    }
}

static MemoryRegionOps cpc_ops;

static void __attribute__((constructor)) init_cpc_ops(void)
{
    memset(&cpc_ops, 0, sizeof(cpc_ops));
    cpc_ops.read = cpc_read;
    cpc_ops.write = cpc_write;
    cpc_ops.endianness = DEVICE_NATIVE_ENDIAN;
    cpc_ops.impl.max_access_size = 8;
}

void MIPSCPCState::init()
{
    memory_region_init_io(&mr, OBJECT(this), &cpc_ops, this, "mips-cpc",
                          CPC_ADDRSPACE_SZ);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &mr);
}

void MIPSCPCState::realize(Error **errp)
{
    if (vp_start_running > cpc_vp_run_mask(this)) {
        error_setg(errp,
                   "incorrect vp_start_running 0x%" PRIx64 " for num_vp = %d",
                   vp_running, num_vp);
        return;
    }
}

void MIPSCPCState::reset()
{
    vp_running = 0;
    cpc_run_vp(this, vp_start_running);
}

static const VMStateField vmstate_mips_cpc_fields[] = {
    VMSTATE_UINT64(vp_running, MIPSCPCState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_mips_cpc = {
    .name = "mips-cpc",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = vmstate_mips_cpc_fields,
};

static const Property mips_cpc_properties[] = {
    DEFINE_PROP_UINT32("num-vp", MIPSCPCState, num_vp, 0x1),
    DEFINE_PROP_UINT64("vp-start-running", MIPSCPCState, vp_start_running, 0x1),
};

void MIPSCPCState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_mips_cpc;
    device_class_set_props(dc, mips_cpc_properties);
}

REGISTER_QEMU_DEVICE(MIPSCPCState, TYPE_MIPS_CPC, TYPE_SYS_BUS_DEVICE)
