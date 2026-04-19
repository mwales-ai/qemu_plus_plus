/*
 * Allwinner A10 SoC emulation
 *
 * Copyright (C) 2013 Li Guang
 * Written by Li Guang <lig.fnst@cn.fujitsu.com>
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

#include "hw/char/serial-mm.h"
#include "hw/usb/hcd-ohci.h"

extern "C" {
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/misc/unimp.h"
#include "system/system.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "target/arm/cpu-qom.h"
}

#include "hw/arm/allwinner-a10.h"

#define AW_A10_SRAM_A_BASE      0x00000000
#define AW_A10_DRAMC_BASE       0x01c01000
#define AW_A10_MMC0_BASE        0x01c0f000
#define AW_A10_CCM_BASE         0x01c20000
#define AW_A10_PIC_REG_BASE     0x01c20400
#define AW_A10_PIT_REG_BASE     0x01c20c00
#define AW_A10_UART0_REG_BASE   0x01c28000
#define AW_A10_SPI0_BASE        0x01c05000
#define AW_A10_EMAC_BASE        0x01c0b000
#define AW_A10_EHCI_BASE        0x01c14000
#define AW_A10_OHCI_BASE        0x01c14400
#define AW_A10_SATA_BASE        0x01c18000
#define AW_A10_WDT_BASE         0x01c20c90
#define AW_A10_RTC_BASE         0x01c20d00
#define AW_A10_I2C0_BASE        0x01c2ac00

extern "C"
void allwinner_a10_bootrom_setup(AwA10State *s, BlockBackend *blk)
{
    const int64_t rom_size = 32 * KiB;
    g_autofree uint8_t *buffer = g_new0(uint8_t, rom_size);

    if (blk_pread(blk, 8 * KiB, rom_size, buffer, static_cast<BdrvRequestFlags>(0)) < 0) {
        error_report("%s: failed to read BlockBackend data", __func__);
        exit(1);
    }

    rom_add_blob("allwinner-a10.bootrom", buffer, rom_size,
                  rom_size, AW_A10_SRAM_A_BASE,
                  NULL, NULL, NULL, NULL, false);
}

void AwA10State::init()
{
    Object *obj = OBJECT(this);

    object_initialize_child(obj, "cpu", &cpu,
                            ARM_CPU_TYPE_NAME("cortex-a8"));

    object_initialize_child(obj, "intc", &intc, TYPE_AW_A10_PIC);

    object_initialize_child(obj, "timer", &timer, TYPE_AW_A10_PIT);

    object_initialize_child(obj, "ccm", &ccm, TYPE_AW_A10_CCM);

    object_initialize_child(obj, "dramc", &dramc, TYPE_AW_A10_DRAMC);

    object_initialize_child(obj, "emac", &emac, TYPE_AW_EMAC);

    object_initialize_child(obj, "sata", &sata, TYPE_ALLWINNER_AHCI);

    object_initialize_child(obj, "i2c0", &i2c0, TYPE_AW_I2C);

    object_initialize_child(obj, "spi0", &spi0, TYPE_AW_A10_SPI);

    for (size_t i = 0; i < AW_A10_NUM_USB; i++) {
        object_initialize_child(obj, "ehci[*]", &ehci[i],
                                TYPE_PLATFORM_EHCI);
        object_initialize_child(obj, "ohci[*]", &ohci[i], TYPE_SYSBUS_OHCI);
    }

    object_initialize_child(obj, "mmc0", &mmc0, TYPE_AW_SDHOST_SUN4I);

    object_initialize_child(obj, "rtc", &rtc, TYPE_AW_RTC_SUN4I);

    object_initialize_child(obj, "wdt", &wdt, TYPE_AW_WDT_SUN4I);
}

void AwA10State::realize(Error **errp)
{
    DeviceState *dev = DEVICE(this);
    SysBusDevice *sysbusdev;

    if (!qdev_realize(DEVICE(&cpu), NULL, errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&intc), errp)) {
        return;
    }
    sysbusdev = SYS_BUS_DEVICE(&intc);
    sysbus_mmio_map(sysbusdev, 0, AW_A10_PIC_REG_BASE);
    sysbus_connect_irq(sysbusdev, 0,
                       qdev_get_gpio_in(DEVICE(&cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(sysbusdev, 1,
                       qdev_get_gpio_in(DEVICE(&cpu), ARM_CPU_FIQ));
    qdev_pass_gpios(DEVICE(&intc), dev, NULL);

    if (!sysbus_realize(SYS_BUS_DEVICE(&timer), errp)) {
        return;
    }
    sysbusdev = SYS_BUS_DEVICE(&timer);
    sysbus_mmio_map(sysbusdev, 0, AW_A10_PIT_REG_BASE);
    sysbus_connect_irq(sysbusdev, 0, qdev_get_gpio_in(dev, 22));
    sysbus_connect_irq(sysbusdev, 1, qdev_get_gpio_in(dev, 23));
    sysbus_connect_irq(sysbusdev, 2, qdev_get_gpio_in(dev, 24));
    sysbus_connect_irq(sysbusdev, 3, qdev_get_gpio_in(dev, 25));
    sysbus_connect_irq(sysbusdev, 4, qdev_get_gpio_in(dev, 67));
    sysbus_connect_irq(sysbusdev, 5, qdev_get_gpio_in(dev, 68));

    memory_region_init_ram(&sram_a, OBJECT(dev), "sram A", 48 * KiB,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x00000000, &sram_a);
    create_unimplemented_device("a10-sram-ctrl", 0x01c00000, 4 * KiB);

    /* Clock Control Module */
    sysbus_realize(SYS_BUS_DEVICE(&ccm), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&ccm), 0, AW_A10_CCM_BASE);

    /* DRAM Control Module */
    sysbus_realize(SYS_BUS_DEVICE(&dramc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&dramc), 0, AW_A10_DRAMC_BASE);

    qemu_configure_nic_device(DEVICE(&emac), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&emac), errp)) {
        return;
    }
    sysbusdev = SYS_BUS_DEVICE(&emac);
    sysbus_mmio_map(sysbusdev, 0, AW_A10_EMAC_BASE);
    sysbus_connect_irq(sysbusdev, 0, qdev_get_gpio_in(dev, 55));

    if (!sysbus_realize(SYS_BUS_DEVICE(&sata), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&sata), 0, AW_A10_SATA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&sata), 0, qdev_get_gpio_in(dev, 56));

    /* FIXME use a qdev chardev prop instead of serial_hd() */
    serial_mm_init(get_system_memory(), AW_A10_UART0_REG_BASE, 2,
                   qdev_get_gpio_in(dev, 1),
                   115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    for (size_t i = 0; i < AW_A10_NUM_USB; i++) {
        g_autofree char *bus = g_strdup_printf("usb-bus.%zu", i);

        object_property_set_bool(OBJECT(&ehci[i]), "companion-enable",
                                 true, &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(&ehci[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&ehci[i]), 0,
                        AW_A10_EHCI_BASE + i * 0x8000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&ehci[i]), 0,
                           qdev_get_gpio_in(dev, 39 + i));

        object_property_set_str(OBJECT(&ohci[i]), "masterbus", bus,
                                &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(&ohci[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&ohci[i]), 0,
                        AW_A10_OHCI_BASE + i * 0x8000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&ohci[i]), 0,
                           qdev_get_gpio_in(dev, 64 + i));
    }

    /* SD/MMC */
    object_property_set_link(OBJECT(&mmc0), "dma-memory",
                             OBJECT(get_system_memory()), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&mmc0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&mmc0), 0, AW_A10_MMC0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&mmc0), 0, qdev_get_gpio_in(dev, 32));
    object_property_add_alias(OBJECT(this), "sd-bus", OBJECT(&mmc0),
                              "sd-bus");

    /* RTC */
    sysbus_realize(SYS_BUS_DEVICE(&rtc), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&rtc), 0, AW_A10_RTC_BASE, 10);

    /* I2C */
    sysbus_realize(SYS_BUS_DEVICE(&i2c0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&i2c0), 0, AW_A10_I2C0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&i2c0), 0, qdev_get_gpio_in(dev, 7));

    /* SPI */
    sysbus_realize(SYS_BUS_DEVICE(&spi0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&spi0), 0, AW_A10_SPI0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&spi0), 0, qdev_get_gpio_in(dev, 10));

    /* WDT */
    sysbus_realize(SYS_BUS_DEVICE(&wdt), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&wdt), 0, AW_A10_WDT_BASE, 1);
}

void AwA10State::classInit(DeviceClass *dc)
{
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(AwA10State, TYPE_AW_A10, TYPE_DEVICE)
