system_ss.add(files('fw_cfg-interface.cpp'))
system_ss.add(files('fw_cfg.cpp'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_otp.cpp'))
system_ss.add(when: 'CONFIG_CHRP_NVRAM', if_true: files('chrp_nvram.cpp'))
system_ss.add(when: 'CONFIG_DS1225Y', if_true: files('ds1225y.cpp'))
system_ss.add(when: 'CONFIG_NMC93XX_EEPROM', if_true: files('eeprom93xx.cpp'))
system_ss.add(when: 'CONFIG_AT24C', if_true: files('eeprom_at24c.cpp'))
system_ss.add(when: 'CONFIG_MAC_NVRAM', if_true: files('mac_nvram.cpp'))
system_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx_otp.cpp'))
system_ss.add(when: 'CONFIG_NRF51_SOC', if_true: files('nrf51_nvm.cpp'))
system_ss.add(when: 'CONFIG_XLNX_EFUSE_CRC', if_true: files('xlnx-efuse-crc.cpp'))
system_ss.add(when: 'CONFIG_XLNX_EFUSE', if_true: files('xlnx-efuse.cpp'))
system_ss.add(when: 'CONFIG_XLNX_EFUSE_VERSAL', if_true: files(
                                                   'xlnx-versal-efuse-cache.cpp',
                                                   'xlnx-versal-efuse-ctrl.cpp'))
system_ss.add(when: 'CONFIG_XLNX_EFUSE_ZYNQMP', if_true: files(
                                                   'xlnx-zynqmp-efuse.cpp'))
system_ss.add(when: 'CONFIG_XLNX_BBRAM', if_true: files('xlnx-bbram.cpp'))

specific_ss.add(when: 'CONFIG_PSERIES', if_true: files('spapr_nvram.cpp'))
specific_ss.add(when: 'CONFIG_ACPI', if_true: files('fw_cfg-acpi.cpp'))

system_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files(
  'aspeed_otp.cpp',
  ))