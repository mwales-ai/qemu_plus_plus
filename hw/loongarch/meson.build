loongarch_ss = ss.source_set()
loongarch_ss.add(files(
    'boot.cpp',
))
common_ss.add(when: 'CONFIG_LOONGARCH_VIRT', if_true: files('fw_cfg.cpp'))
loongarch_ss.add(when: 'CONFIG_LOONGARCH_VIRT', if_true: files(
  'virt-fdt-build.cpp',
  'virt.cpp'))
loongarch_ss.add(when: 'CONFIG_ACPI', if_true: files('virt-acpi-build.cpp'))

hw_arch += {'loongarch': loongarch_ss}
