/*
 * STM32F100 SoC
 *
 * Copyright (c) 2021 Alexandre Iooss <erdnaxe@crans.org>
 * Copyright (c) 2014 Alistair Francis <alistair@alistair23.me>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/arm/boot.h"
#include "system/address-spaces.h"
#include "hw/arm/stm32f100_soc.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/misc/unimp.h"
#include "system/system.h"

/* stm32f100_soc implementation is derived from stm32f205_soc */

static const uint32_t usart_addr[STM_NUM_USARTS] = { 0x40013800, 0x40004400,
    0x40004800 };
static const uint32_t spi_addr[STM_NUM_SPIS] = { 0x40013000, 0x40003800 };

static const int usart_irq[STM_NUM_USARTS] = {37, 38, 39};
static const int spi_irq[STM_NUM_SPIS] = {35, 36};

void STM32F100State::init()
{
    Object *obj = OBJECT(this);
    int i;

    object_initialize_child(obj, "armv7m", &armv7m, TYPE_ARMV7M);

    for (i = 0; i < STM_NUM_USARTS; i++) {
        object_initialize_child(obj, "usart[*]", &usart[i],
                                TYPE_STM32F2XX_USART);
    }

    for (i = 0; i < STM_NUM_SPIS; i++) {
        object_initialize_child(obj, "spi[*]", &spi[i], TYPE_STM32F2XX_SPI);
    }

    sysclk = qdev_init_clock_in(DEVICE(this), "sysclk", NULL, NULL, 0);
    refclk = qdev_init_clock_in(DEVICE(this), "refclk", NULL, NULL, 0);
}

void STM32F100State::realize(Error **errp)
{
    DeviceState *dev, *armv7m_dev;
    SysBusDevice *busdev;
    int i;

    MemoryRegion *system_memory = get_system_memory();

    /*
     * We use refclk internally and only define it with qdev_init_clock_in()
     * so it is correctly parented and not leaked on an init/deinit; it is not
     * intended as an externally exposed clock.
     */
    if (clock_has_source(refclk)) {
        error_setg(errp, "refclk clock must not be wired up by the board code");
        return;
    }

    if (!clock_has_source(sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }

    /*
     * TODO: ideally we should model the SoC RCC and its ability to
     * change the sysclk frequency and define different sysclk sources.
     */

    /* The refclk always runs at frequency HCLK / 8 */
    clock_set_mul_div(refclk, 8, 1);
    clock_set_source(refclk, sysclk);

    /*
     * Init flash region
     * Flash starts at 0x08000000 and then is aliased to boot memory at 0x0
     */
    memory_region_init_rom(&flash, OBJECT(this), "STM32F100.flash",
                           FLASH_SIZE, &error_fatal);
    memory_region_init_alias(&flash_alias, OBJECT(this),
                             "STM32F100.flash.alias", &flash, 0, FLASH_SIZE);
    memory_region_add_subregion(system_memory, FLASH_BASE_ADDRESS, &flash);
    memory_region_add_subregion(system_memory, 0, &flash_alias);

    /* Init SRAM region */
    memory_region_init_ram(&sram, NULL, "STM32F100.sram", SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(system_memory, SRAM_BASE_ADDRESS, &sram);

    /* Init ARMv7m */
    armv7m_dev = DEVICE(&armv7m);
    qdev_prop_set_uint32(armv7m_dev, "num-irq", 61);
    qdev_prop_set_uint8(armv7m_dev, "num-prio-bits", 4);
    qdev_prop_set_string(armv7m_dev, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m3"));
    qdev_prop_set_bit(armv7m_dev, "enable-bitband", true);
    qdev_connect_clock_in(armv7m_dev, "cpuclk", sysclk);
    qdev_connect_clock_in(armv7m_dev, "refclk", refclk);
    object_property_set_link(OBJECT(&armv7m), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&armv7m), errp)) {
        return;
    }

    /* Attach UART (uses USART registers) and USART controllers */
    for (i = 0; i < STM_NUM_USARTS; i++) {
        dev = DEVICE(&(usart[i]));
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&usart[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, usart_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m_dev, usart_irq[i]));
    }

    /* SPI 1 and 2 */
    for (i = 0; i < STM_NUM_SPIS; i++) {
        dev = DEVICE(&(spi[i]));
        if (!sysbus_realize(SYS_BUS_DEVICE(&spi[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, spi_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m_dev, spi_irq[i]));
    }

    create_unimplemented_device("timer[2]",  0x40000000, 0x400);
    create_unimplemented_device("timer[3]",  0x40000400, 0x400);
    create_unimplemented_device("timer[4]",  0x40000800, 0x400);
    create_unimplemented_device("timer[6]",  0x40001000, 0x400);
    create_unimplemented_device("timer[7]",  0x40001400, 0x400);
    create_unimplemented_device("RTC",       0x40002800, 0x400);
    create_unimplemented_device("WWDG",      0x40002C00, 0x400);
    create_unimplemented_device("IWDG",      0x40003000, 0x400);
    create_unimplemented_device("I2C1",      0x40005400, 0x400);
    create_unimplemented_device("I2C2",      0x40005800, 0x400);
    create_unimplemented_device("BKP",       0x40006C00, 0x400);
    create_unimplemented_device("PWR",       0x40007000, 0x400);
    create_unimplemented_device("DAC",       0x40007400, 0x400);
    create_unimplemented_device("CEC",       0x40007800, 0x400);
    create_unimplemented_device("AFIO",      0x40010000, 0x400);
    create_unimplemented_device("EXTI",      0x40010400, 0x400);
    create_unimplemented_device("GPIOA",     0x40010800, 0x400);
    create_unimplemented_device("GPIOB",     0x40010C00, 0x400);
    create_unimplemented_device("GPIOC",     0x40011000, 0x400);
    create_unimplemented_device("GPIOD",     0x40011400, 0x400);
    create_unimplemented_device("GPIOE",     0x40011800, 0x400);
    create_unimplemented_device("ADC1",      0x40012400, 0x400);
    create_unimplemented_device("timer[1]",  0x40012C00, 0x400);
    create_unimplemented_device("timer[15]", 0x40014000, 0x400);
    create_unimplemented_device("timer[16]", 0x40014400, 0x400);
    create_unimplemented_device("timer[17]", 0x40014800, 0x400);
    create_unimplemented_device("DMA",       0x40020000, 0x400);
    create_unimplemented_device("RCC",       0x40021000, 0x400);
    create_unimplemented_device("Flash Int", 0x40022000, 0x400);
    create_unimplemented_device("CRC",       0x40023000, 0x400);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(STM32F100State, TYPE_STM32F100_SOC, TYPE_SYS_BUS_DEVICE)
