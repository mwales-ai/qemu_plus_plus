/*
 * Coherent Processing System emulation.
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
#include "qemu/module.h"
#include "hw/mips/cps.h"
#include "hw/mips/mips.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "system/tcg.h"
#include "system/reset.h"
}

extern "C" qemu_irq get_cps_irq(MIPSCPSState *s, int pin_number)
{
    assert(pin_number < s->num_irq);
    return s->gic.irq_state[pin_number].irq;
}

void MIPSCPSState::init()
{
    clock = qdev_init_clock_in(DEVICE(this), "clk-in", NULL, NULL, 0);
    memory_region_init(&container, OBJECT(this), "mips-cps-container", UINT64_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &container);
}

static void main_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = static_cast<MIPSCPU *>(opaque);
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
}

static bool cpu_mips_itu_supported(CPUMIPSState *env)
{
    bool is_mt = (env->CP0_Config5 & (1 << CP0C5_VP)) || ase_mt_available(env);

    return is_mt && tcg_enabled();
}

void MIPSCPSState::realize(Error **errp)
{
    Object *obj = OBJECT(this);
    target_ulong gcr_base;
    bool itu_present = false;

    if (!clock_get(clock)) {
        error_setg(errp, "CPS input clock is not connected to an output clock");
        return;
    }

    for (int i = 0; i < num_vp; i++) {
        MIPSCPU *cpu = MIPS_CPU(object_new(cpu_type));
        CPUMIPSState *env = &cpu->env;

        object_property_set_bool(OBJECT(cpu), "big-endian", cpu_is_bigendian,
                                 &error_abort);

        object_property_set_bool(OBJECT(cpu), "start-powered-off", true,
                                 &error_abort);

        qdev_connect_clock_in(DEVICE(cpu), "clk-in", clock);

        if (!qdev_realize_and_unref(DEVICE(cpu), NULL, errp)) {
            return;
        }

        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);

        if (cpu_mips_itu_supported(env)) {
            itu_present = true;
            env->itc_tag = mips_itu_get_tag_region(&itu);
        }
        qemu_register_reset(main_cpu_reset, cpu);
    }

    if (itu_present) {
        object_initialize_child(obj, "itu", &itu, TYPE_MIPS_ITU);
        object_property_set_uint(OBJECT(&itu), "num-fifo", 16,
                                &error_abort);
        object_property_set_uint(OBJECT(&itu), "num-semaphores", 16,
                                &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&itu), errp)) {
            return;
        }

        memory_region_add_subregion(&container, 0,
                           sysbus_mmio_get_region(SYS_BUS_DEVICE(&itu), 0));
    }

    object_initialize_child(obj, "cpc", &cpc, TYPE_MIPS_CPC);
    object_property_set_uint(OBJECT(&cpc), "num-vp", num_vp,
                            &error_abort);
    object_property_set_int(OBJECT(&cpc), "vp-start-running", 1,
                            &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&cpc), errp)) {
        return;
    }

    memory_region_add_subregion(&container, 0,
                            sysbus_mmio_get_region(SYS_BUS_DEVICE(&cpc), 0));

    object_initialize_child(obj, "gic", &gic, TYPE_MIPS_GIC);
    object_property_set_uint(OBJECT(&gic), "num-vp", num_vp,
                            &error_abort);
    object_property_set_uint(OBJECT(&gic), "num-irq", 128,
                            &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&gic), errp)) {
        return;
    }

    memory_region_add_subregion(&container, 0,
                            sysbus_mmio_get_region(SYS_BUS_DEVICE(&gic), 0));

    gcr_base = MIPS_CPU(first_cpu)->env.CP0_CMGCRBase << 4;

    object_initialize_child(obj, "gcr", &gcr, TYPE_MIPS_GCR);
    object_property_set_uint(OBJECT(&gcr), "num-vp", num_vp,
                            &error_abort);
    object_property_set_int(OBJECT(&gcr), "gcr-rev", 0x800,
                            &error_abort);
    object_property_set_int(OBJECT(&gcr), "gcr-base", gcr_base,
                            &error_abort);
    object_property_set_link(OBJECT(&gcr), "gic", OBJECT(&gic.mr),
                             &error_abort);
    object_property_set_link(OBJECT(&gcr), "cpc", OBJECT(&cpc.mr),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&gcr), errp)) {
        return;
    }

    memory_region_add_subregion(&container, gcr_base,
                            sysbus_mmio_get_region(SYS_BUS_DEVICE(&gcr), 0));
}

static const Property mips_cps_properties[] = {
    DEFINE_PROP_UINT32("num-vp", MIPSCPSState, num_vp, 1),
    DEFINE_PROP_UINT32("num-irq", MIPSCPSState, num_irq, 256),
    DEFINE_PROP_STRING("cpu-type", MIPSCPSState, cpu_type),
    DEFINE_PROP_BOOL("cpu-big-endian", MIPSCPSState, cpu_is_bigendian, false),
};

void MIPSCPSState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, mips_cps_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(MIPSCPSState, TYPE_MIPS_CPS, TYPE_SYS_BUS_DEVICE)
