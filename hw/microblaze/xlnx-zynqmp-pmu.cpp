/*
 * Xilinx Zynq MPSoC PMU (Power Management Unit) emulation
 *
 * Copyright (C) 2017 Xilinx Inc
 * Written by Alistair Francis <alistair.francis@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/boards.h"
#include "cpu.h"
#include "boot.h"

#include "hw/intc/xlnx-zynqmp-ipi.h"
#include "hw/intc/xlnx-pmu-iomod-intc.h"
#include "qom/object.h"

/* Define the PMU device */

#define TYPE_XLNX_ZYNQMP_PMU_SOC "xlnx-zynqmp-pmu-soc"
OBJECT_DECLARE_SIMPLE_TYPE(XlnxZynqMPPMUSoCState, XLNX_ZYNQMP_PMU_SOC)

#define XLNX_ZYNQMP_PMU_ROM_SIZE    0x8000
#define XLNX_ZYNQMP_PMU_ROM_ADDR    0xFFD00000
#define XLNX_ZYNQMP_PMU_RAM_ADDR    0xFFDC0000

#define XLNX_ZYNQMP_PMU_INTC_ADDR   0xFFD40000

#define XLNX_ZYNQMP_PMU_NUM_IPIS    4

static const uint64_t ipi_addr[XLNX_ZYNQMP_PMU_NUM_IPIS] = {
    0xFF340000, 0xFF350000, 0xFF360000, 0xFF370000,
};
static const uint64_t ipi_irq[XLNX_ZYNQMP_PMU_NUM_IPIS] = {
    19, 20, 21, 22,
};

struct XlnxZynqMPPMUSoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    MicroBlazeCPU cpu;
    XlnxPMUIOIntc intc;
    XlnxZynqMPIPI ipi[XLNX_ZYNQMP_PMU_NUM_IPIS];

    void realize(Error **errp)
    {
        object_property_set_uint(OBJECT(&cpu), "base-vectors",
                                 XLNX_ZYNQMP_PMU_ROM_ADDR, &error_abort);
        object_property_set_bool(OBJECT(&cpu), "use-stack-protection", true,
                                 &error_abort);
        object_property_set_uint(OBJECT(&cpu), "use-fpu", 0, &error_abort);
        object_property_set_uint(OBJECT(&cpu), "use-hw-mul", 0, &error_abort);
        object_property_set_bool(OBJECT(&cpu), "use-barrel", true,
                                 &error_abort);
        object_property_set_bool(OBJECT(&cpu), "use-msr-instr", true,
                                 &error_abort);
        object_property_set_bool(OBJECT(&cpu), "use-pcmp-instr", true,
                                 &error_abort);
        object_property_set_bool(OBJECT(&cpu), "use-mmu", false, &error_abort);
        object_property_set_bool(OBJECT(&cpu), "little-endian", true,
                                 &error_abort);
        object_property_set_str(OBJECT(&cpu), "version", "8.40.b",
                                &error_abort);
        object_property_set_uint(OBJECT(&cpu), "pvr", 0, &error_abort);
        if (!qdev_realize(DEVICE(&cpu), NULL, errp)) {
            return;
        }

        object_property_set_uint(OBJECT(&intc), "intc-intr-size", 0x10,
                                 &error_abort);
        object_property_set_uint(OBJECT(&intc), "intc-level-edge", 0x0,
                                 &error_abort);
        object_property_set_uint(OBJECT(&intc), "intc-positive", 0xffff,
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&intc), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&intc), 0, XLNX_ZYNQMP_PMU_INTC_ADDR);
        sysbus_connect_irq(SYS_BUS_DEVICE(&intc), 0,
                           qdev_get_gpio_in(DEVICE(&cpu), MB_CPU_IRQ));

        /* Connect the IPI device */
        for (int i = 0; i < XLNX_ZYNQMP_PMU_NUM_IPIS; i++) {
            sysbus_realize(SYS_BUS_DEVICE(&ipi[i]), &error_abort);
            sysbus_mmio_map(SYS_BUS_DEVICE(&ipi[i]), 0, ipi_addr[i]);
            sysbus_connect_irq(SYS_BUS_DEVICE(&ipi[i]), 0,
                               qdev_get_gpio_in(DEVICE(&intc), ipi_irq[i]));
        }
    }

    void init()
    {
        object_initialize_child(OBJECT(this), "pmu-cpu", &cpu, TYPE_MICROBLAZE_CPU);

        object_initialize_child(OBJECT(this), "intc", &intc, TYPE_XLNX_PMU_IO_INTC);

        for (int i = 0; i < XLNX_ZYNQMP_PMU_NUM_IPIS; i++) {
            char *name = g_strdup_printf("ipi%d", i);
            object_initialize_child(OBJECT(this), name, &ipi[i], TYPE_XLNX_ZYNQMP_IPI);
            g_free(name);
        }
    }

    static void classInit(DeviceClass *dc)
    {
        dc->user_creatable = false;
    }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(XlnxZynqMPPMUSoCState, TYPE_XLNX_ZYNQMP_PMU_SOC, TYPE_DEVICE)

/* Define the PMU Machine */

static void xlnx_zynqmp_pmu_init(MachineState *machine)
{
    XlnxZynqMPPMUSoCState *pmu = g_new0(XlnxZynqMPPMUSoCState, 1);
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *pmu_rom = g_new(MemoryRegion, 1);
    MemoryRegion *pmu_ram = g_new(MemoryRegion, 1);

    /* Create the ROM */
    memory_region_init_rom(pmu_rom, NULL, "xlnx-zynqmp-pmu.rom",
                           XLNX_ZYNQMP_PMU_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, XLNX_ZYNQMP_PMU_ROM_ADDR,
                                pmu_rom);

    /* Create the RAM */
    memory_region_init_ram(pmu_ram, NULL, "xlnx-zynqmp-pmu.ram",
                           machine->ram_size, &error_fatal);
    memory_region_add_subregion(address_space_mem, XLNX_ZYNQMP_PMU_RAM_ADDR,
                                pmu_ram);

    /* Create the PMU device */
    object_initialize_child(reinterpret_cast<Object *>(machine), "pmu", pmu,
                            TYPE_XLNX_ZYNQMP_PMU_SOC);
    qdev_realize(reinterpret_cast<DeviceState *>(pmu), NULL, &error_fatal);

    /* Load the kernel */
    microblaze_load_kernel(&pmu->cpu, true, XLNX_ZYNQMP_PMU_RAM_ADDR,
                           machine->ram_size,
                           machine->initrd_filename,
                           machine->dtb,
                           NULL);
}

static void xlnx_zynqmp_pmu_machine_init(MachineClass *mc)
{
    mc->desc = "Xilinx ZynqMP PMU machine (little endian)";
    mc->init = xlnx_zynqmp_pmu_init;
}

DEFINE_MACHINE("xlnx-zynqmp-pmu", xlnx_zynqmp_pmu_machine_init)
