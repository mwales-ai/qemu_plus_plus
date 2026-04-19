/*
 * Copyright (c) 2013 Jean-Christophe Dubois <jcd@tribudubois.net>
 *
 * i.MX31 SOC emulation.
 *
 * Based on hw/arm/fsl-imx31.c
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/fsl-imx31.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "hw/qdev-properties.h"
#include "chardev/char.h"
#include "target/arm/cpu-qom.h"

void FslIMX31State::init()
{
    Object *obj = OBJECT(this);
    int i;

    object_initialize_child(obj, "cpu", &cpu, ARM_CPU_TYPE_NAME("arm1136"));

    object_initialize_child(obj, "avic", &avic, TYPE_IMX_AVIC);

    object_initialize_child(obj, "ccm", &ccm, TYPE_IMX31_CCM);

    for (i = 0; i < FSL_IMX31_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &uart[i], TYPE_IMX_SERIAL);
    }

    object_initialize_child(obj, "gpt", &gpt, TYPE_IMX31_GPT);

    for (i = 0; i < FSL_IMX31_NUM_EPITS; i++) {
        object_initialize_child(obj, "epit[*]", &epit[i], TYPE_IMX_EPIT);
    }

    for (i = 0; i < FSL_IMX31_NUM_I2CS; i++) {
        object_initialize_child(obj, "i2c[*]", &i2c[i], TYPE_IMX_I2C);
    }

    for (i = 0; i < FSL_IMX31_NUM_GPIOS; i++) {
        object_initialize_child(obj, "gpio[*]", &gpio[i], TYPE_IMX_GPIO);
    }

    object_initialize_child(obj, "wdt", &wdt, TYPE_IMX2_WDT);
}

void FslIMX31State::realize(Error **errp)
{
    DeviceState *dev = DEVICE(this);
    uint16_t i;

    if (!qdev_realize(DEVICE(&cpu), NULL, errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&avic), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&avic), 0, FSL_IMX31_AVIC_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&avic), 0,
                       qdev_get_gpio_in(DEVICE(&cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&avic), 1,
                       qdev_get_gpio_in(DEVICE(&cpu), ARM_CPU_FIQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&ccm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&ccm), 0, FSL_IMX31_CCM_ADDR);

    /* Initialize all UARTS */
    for (i = 0; i < FSL_IMX31_NUM_UARTS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } serial_table[FSL_IMX31_NUM_UARTS] = {
            { FSL_IMX31_UART1_ADDR, FSL_IMX31_UART1_IRQ },
            { FSL_IMX31_UART2_ADDR, FSL_IMX31_UART2_IRQ },
        };

        qdev_prop_set_chr(DEVICE(&uart[i]), "chardev", serial_hd(i));

        if (!sysbus_realize(SYS_BUS_DEVICE(&uart[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&uart[i]), 0, serial_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&uart[i]), 0,
                           qdev_get_gpio_in(DEVICE(&avic),
                                            serial_table[i].irq));
    }

    gpt.ccm = IMX_CCM(&ccm);

    if (!sysbus_realize(SYS_BUS_DEVICE(&gpt), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&gpt), 0, FSL_IMX31_GPT_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&gpt), 0,
                       qdev_get_gpio_in(DEVICE(&avic), FSL_IMX31_GPT_IRQ));

    /* Initialize all EPIT timers */
    for (i = 0; i < FSL_IMX31_NUM_EPITS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } epit_table[FSL_IMX31_NUM_EPITS] = {
            { FSL_IMX31_EPIT1_ADDR, FSL_IMX31_EPIT1_IRQ },
            { FSL_IMX31_EPIT2_ADDR, FSL_IMX31_EPIT2_IRQ },
        };

        epit[i].ccm = IMX_CCM(&ccm);

        if (!sysbus_realize(SYS_BUS_DEVICE(&epit[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&epit[i]), 0, epit_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&epit[i]), 0,
                           qdev_get_gpio_in(DEVICE(&avic),
                                            epit_table[i].irq));
    }

    /* Initialize all I2C */
    for (i = 0; i < FSL_IMX31_NUM_I2CS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } i2c_table[FSL_IMX31_NUM_I2CS] = {
            { FSL_IMX31_I2C1_ADDR, FSL_IMX31_I2C1_IRQ },
            { FSL_IMX31_I2C2_ADDR, FSL_IMX31_I2C2_IRQ },
            { FSL_IMX31_I2C3_ADDR, FSL_IMX31_I2C3_IRQ }
        };

        /* Initialize the I2C */
        if (!sysbus_realize(SYS_BUS_DEVICE(&i2c[i]), errp)) {
            return;
        }
        /* Map I2C memory */
        sysbus_mmio_map(SYS_BUS_DEVICE(&i2c[i]), 0, i2c_table[i].addr);
        /* Connect I2C IRQ to PIC */
        sysbus_connect_irq(SYS_BUS_DEVICE(&i2c[i]), 0,
                           qdev_get_gpio_in(DEVICE(&avic),
                                            i2c_table[i].irq));
    }

    /* Initialize all GPIOs */
    for (i = 0; i < FSL_IMX31_NUM_GPIOS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } gpio_table[FSL_IMX31_NUM_GPIOS] = {
            { FSL_IMX31_GPIO1_ADDR, FSL_IMX31_GPIO1_IRQ },
            { FSL_IMX31_GPIO2_ADDR, FSL_IMX31_GPIO2_IRQ },
            { FSL_IMX31_GPIO3_ADDR, FSL_IMX31_GPIO3_IRQ }
        };

        object_property_set_bool(OBJECT(&gpio[i]), "has-edge-sel", false,
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&gpio[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&gpio[i]), 0, gpio_table[i].addr);
        /* Connect GPIO IRQ to PIC */
        sysbus_connect_irq(SYS_BUS_DEVICE(&gpio[i]), 0,
                           qdev_get_gpio_in(DEVICE(&avic),
                                            gpio_table[i].irq));
    }

    /* Watchdog */
    sysbus_realize(SYS_BUS_DEVICE(&wdt), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&wdt), 0, FSL_IMX31_WDT_ADDR);

    /* On a real system, the first 16k is a `secure boot rom' */
    if (!memory_region_init_rom(&secure_rom, OBJECT(dev), "imx31.secure_rom",
                                FSL_IMX31_SECURE_ROM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), FSL_IMX31_SECURE_ROM_ADDR,
                                &secure_rom);

    /* There is also a 16k ROM */
    if (!memory_region_init_rom(&rom, OBJECT(dev), "imx31.rom",
                                FSL_IMX31_ROM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), FSL_IMX31_ROM_ADDR,
                                &rom);

    /* initialize internal RAM (16 KB) */
    if (!memory_region_init_ram(&iram, NULL, "imx31.iram",
                                FSL_IMX31_IRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), FSL_IMX31_IRAM_ADDR,
                                &iram);

    /* internal RAM (16 KB) is aliased over 256 MB - 16 KB */
    memory_region_init_alias(&iram_alias, OBJECT(dev), "imx31.iram_alias",
                             &iram, 0, FSL_IMX31_IRAM_ALIAS_SIZE);
    memory_region_add_subregion(get_system_memory(), FSL_IMX31_IRAM_ALIAS_ADDR,
                                &iram_alias);
}

void FslIMX31State::classInit(DeviceClass *dc)
{
    dc->desc = "i.MX31 SOC";
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(FslIMX31State, TYPE_FSL_IMX31, TYPE_DEVICE)
