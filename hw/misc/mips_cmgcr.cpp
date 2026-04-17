/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2012  MIPS Technologies, Inc.  All rights reserved.
 * Authors: Sanjay Lal <sanjayl@kymasys.com>
 *
 * Copyright (C) 2015 Imagination Technologies
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "hw/misc/mips_cmgcr.h"
#include "hw/misc/mips_cpc.h"
#include "hw/qdev-properties.h"
#include "hw/intc/mips_gic.h"
}
#include "qom/cpp/object.h"

static inline bool is_cpc_connected(MIPSGCRState *s)
{
    return s->cpc_mr != NULL;
}

static inline bool is_gic_connected(MIPSGCRState *s)
{
    return s->gic_mr != NULL;
}

static inline void update_gcr_base(MIPSGCRState *gcr, uint64_t val)
{
    CPUState *cpu;
    MIPSCPU *mips_cpu;

    gcr->gcr_base = val & GCR_BASE_GCRBASE_MSK;
    memory_region_set_address(&gcr->iomem, gcr->gcr_base);

    CPU_FOREACH(cpu) {
        mips_cpu = MIPS_CPU(cpu);
        mips_cpu->env.CP0_CMGCRBase = gcr->gcr_base >> 4;
    }
}

static inline void update_cpc_base(MIPSGCRState *gcr, uint64_t val)
{
    if (is_cpc_connected(gcr)) {
        gcr->cpc_base = val & GCR_CPC_BASE_MSK;
        memory_region_transaction_begin();
        memory_region_set_address(gcr->cpc_mr,
                                  gcr->cpc_base & GCR_CPC_BASE_CPCBASE_MSK);
        memory_region_set_enabled(gcr->cpc_mr,
                                  gcr->cpc_base & GCR_CPC_BASE_CPCEN_MSK);
        memory_region_transaction_commit();
    }
}

static inline void update_gic_base(MIPSGCRState *gcr, uint64_t val)
{
    if (is_gic_connected(gcr)) {
        gcr->gic_base = val & GCR_GIC_BASE_MSK;
        memory_region_transaction_begin();
        memory_region_set_address(gcr->gic_mr,
                                  gcr->gic_base & GCR_GIC_BASE_GICBASE_MSK);
        memory_region_set_enabled(gcr->gic_mr,
                                  gcr->gic_base & GCR_GIC_BASE_GICEN_MSK);
        memory_region_transaction_commit();
    }
}

/* Read GCR registers */
static uint64_t gcr_read(void *opaque, hwaddr addr, unsigned size)
{
    MIPSGCRState *gcr = static_cast<MIPSGCRState *>(opaque);
    MIPSGCRVPState *current_vps = &gcr->vps[current_cpu->cpu_index];
    MIPSGCRVPState *other_vps = &gcr->vps[current_vps->other];

    switch (addr) {
    /* Global Control Block Register */
    case GCR_CONFIG_OFS:
        /* Set PCORES to 0 */
        return 0;
    case GCR_BASE_OFS:
        return gcr->gcr_base;
    case GCR_REV_OFS:
        return gcr->gcr_rev;
    case GCR_GIC_BASE_OFS:
        return gcr->gic_base;
    case GCR_CPC_BASE_OFS:
        return gcr->cpc_base;
    case GCR_GIC_STATUS_OFS:
        return is_gic_connected(gcr);
    case GCR_CPC_STATUS_OFS:
        return is_cpc_connected(gcr);
    case GCR_L2_CONFIG_OFS:
        /* L2 BYPASS */
        return GCR_L2_CONFIG_BYPASS_MSK;
        /* Core-Local and Core-Other Control Blocks */
    case MIPS_CLCB_OFS + GCR_CL_CONFIG_OFS:
    case MIPS_COCB_OFS + GCR_CL_CONFIG_OFS:
        /* Set PVP to # of VPs - 1 */
        return gcr->num_vps - 1;
    case MIPS_CLCB_OFS + GCR_CL_RESETBASE_OFS:
        return current_vps->reset_base;
    case MIPS_COCB_OFS + GCR_CL_RESETBASE_OFS:
        return other_vps->reset_base;
    case MIPS_CLCB_OFS + GCR_CL_OTHER_OFS:
        return current_vps->other;
    case MIPS_COCB_OFS + GCR_CL_OTHER_OFS:
        return other_vps->other;
    default:
        qemu_log_mask(LOG_UNIMP, "Read %d bytes at GCR offset 0x%" HWADDR_PRIx
                      "\n", size, addr);
        return 0;
    }
    return 0;
}

static inline target_ulong get_exception_base(MIPSGCRVPState *vps)
{
    /* TODO: BEV_BASE and SELECT_BEV */
    return (int32_t)(vps->reset_base & GCR_CL_RESET_BASE_RESETBASE_MSK);
}

/* Write GCR registers */
static void gcr_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    MIPSGCRState *gcr = static_cast<MIPSGCRState *>(opaque);
    MIPSGCRVPState *current_vps = &gcr->vps[current_cpu->cpu_index];
    MIPSGCRVPState *other_vps = &gcr->vps[current_vps->other];

    switch (addr) {
    case GCR_BASE_OFS:
        update_gcr_base(gcr, data);
        break;
    case GCR_GIC_BASE_OFS:
        update_gic_base(gcr, data);
        break;
    case GCR_CPC_BASE_OFS:
        update_cpc_base(gcr, data);
        break;
    case MIPS_CLCB_OFS + GCR_CL_RESETBASE_OFS:
        current_vps->reset_base = data & GCR_CL_RESET_BASE_MSK;
        cpu_set_exception_base(current_cpu->cpu_index,
                               get_exception_base(current_vps));
        break;
    case MIPS_COCB_OFS + GCR_CL_RESETBASE_OFS:
        other_vps->reset_base = data & GCR_CL_RESET_BASE_MSK;
        cpu_set_exception_base(current_vps->other,
                               get_exception_base(other_vps));
        break;
    case MIPS_CLCB_OFS + GCR_CL_OTHER_OFS:
        if ((data & GCR_CL_OTHER_MSK) < gcr->num_vps) {
            current_vps->other = data & GCR_CL_OTHER_MSK;
        }
        break;
    case MIPS_COCB_OFS + GCR_CL_OTHER_OFS:
        if ((data & GCR_CL_OTHER_MSK) < gcr->num_vps) {
            other_vps->other = data & GCR_CL_OTHER_MSK;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "Write %d bytes at GCR offset 0x%" HWADDR_PRIx
                      " 0x%" PRIx64 "\n", size, addr, data);
        break;
    }
}

static MemoryRegionOps gcr_ops;

void MIPSGCRState::init()
{
    memory_region_init_io(&iomem, OBJECT(this), &gcr_ops, this,
                          "mips-gcr", GCR_ADDRSPACE_SZ);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);
}

void MIPSGCRState::reset()
{
    update_gic_base(this, 0);
    update_cpc_base(this, 0);

    for (uint32_t i = 0; i < num_vps; i++) {
        vps[i].other = 0;
        vps[i].reset_base = 0xBFC00000 & GCR_CL_RESET_BASE_MSK;
        cpu_set_exception_base(i, get_exception_base(&vps[i]));
    }
}

static const VMStateField vmstate_mips_gcr_fields[] = {
    VMSTATE_UINT64(cpc_base, MIPSGCRState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_mips_gcr = {
    .name = "mips-gcr",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = vmstate_mips_gcr_fields,
};

static const Property mips_gcr_properties[] = {
    DEFINE_PROP_UINT32("num-vp", MIPSGCRState, num_vps, 1),
    DEFINE_PROP_INT32("gcr-rev", MIPSGCRState, gcr_rev, 0x800),
    DEFINE_PROP_UINT64("gcr-base", MIPSGCRState, gcr_base, GCR_BASE_ADDR),
    DEFINE_PROP_LINK("gic", MIPSGCRState, gic_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("cpc", MIPSGCRState, cpc_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

void MIPSGCRState::realize(Error **errp)
{
    /* Create local set of registers for each VP */
    vps = g_new(MIPSGCRVPState, num_vps);
}

void MIPSGCRState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, mips_gcr_properties);
    dc->vmsd = &vmstate_mips_gcr;
}

REGISTER_QEMU_DEVICE(MIPSGCRState, TYPE_MIPS_GCR, TYPE_SYS_BUS_DEVICE)

static void __attribute__((constructor)) init_gcr_ops(void)
{
    memset(&gcr_ops, 0, sizeof(gcr_ops));
    gcr_ops.read = gcr_read;
    gcr_ops.write = gcr_write;
    gcr_ops.endianness = DEVICE_NATIVE_ENDIAN;
    gcr_ops.impl.max_access_size = 8;
}
