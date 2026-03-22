mips_ss = ss.source_set()
mips_ss.add(files('bootloader.cpp', 'mips_int.cpp'))
common_ss.add(when: 'CONFIG_FW_CFG_MIPS', if_true: files('fw_cfg.cpp'))
common_ss.add(when: 'CONFIG_LOONGSON3V', if_true: files('loongson3_bootp.cpp'))
mips_ss.add(when: 'CONFIG_LOONGSON3V', if_true: files('loongson3_virt.cpp'))
mips_ss.add(when: 'CONFIG_MALTA', if_true: files('malta.cpp'))
mips_ss.add(when: 'CONFIG_MIPS_CPS', if_true: files('cps.cpp'))

if 'CONFIG_TCG' in config_all_accel
mips_ss.add(when: 'CONFIG_JAZZ', if_true: files('jazz.cpp'))
mips_ss.add(when: 'CONFIG_FULOONG', if_true: files('fuloong2e.cpp'))
mips_ss.add(when: 'CONFIG_MIPS_BOSTON', if_true: files('boston.cpp'))
endif

hw_arch += {'mips': mips_ss}
