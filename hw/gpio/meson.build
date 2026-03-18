system_ss.add(when: 'CONFIG_GPIO_KEY', if_true: files('gpio_key.cpp'))
system_ss.add(when: 'CONFIG_GPIO_MPC8XXX', if_true: files('mpc8xxx.cpp'))
system_ss.add(when: 'CONFIG_GPIO_PWR', if_true: files('gpio_pwr.cpp'))
system_ss.add(when: 'CONFIG_PCA9552', if_true: files('pca9552.cpp'))
system_ss.add(when: 'CONFIG_PCA9554', if_true: files('pca9554.cpp'))
system_ss.add(when: 'CONFIG_PL061', if_true: files('pl061.cpp'))
system_ss.add(when: 'CONFIG_ZAURUS_SCOOP', if_true: files('zaurus.cpp'))

system_ss.add(when: 'CONFIG_IMX', if_true: files('imx_gpio.cpp'))
system_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx_gpio.cpp'))
system_ss.add(when: 'CONFIG_NRF51_SOC', if_true: files('nrf51_gpio.cpp'))
system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_gpio.cpp'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files(
    'bcm2835_gpio.cpp',
    'bcm2838_gpio.cpp'
))
system_ss.add(when: 'CONFIG_STM32L4X5_SOC', if_true: files('stm32l4x5_gpio.c'))
system_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files('aspeed_gpio.c'))
system_ss.add(when: 'CONFIG_SIFIVE_GPIO', if_true: files('sifive_gpio.cpp'))
system_ss.add(when: 'CONFIG_PCF8574', if_true: files('pcf8574.cpp'))
