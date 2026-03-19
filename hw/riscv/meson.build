riscv_ss = ss.source_set()
riscv_ss.add(files('boot.cpp'))
riscv_ss.add(when: 'CONFIG_RISCV_NUMA', if_true: files('numa.cpp'))
riscv_ss.add(files('riscv_hart.cpp'))
riscv_ss.add(when: 'CONFIG_OPENTITAN', if_true: files('opentitan.cpp'))
riscv_ss.add(when: 'CONFIG_RISCV_VIRT', if_true: files('virt.cpp'))
riscv_ss.add(when: 'CONFIG_SHAKTI_C', if_true: files('shakti_c.cpp'))
riscv_ss.add(when: 'CONFIG_SIFIVE_E', if_true: files('sifive_e.cpp'))
riscv_ss.add(when: 'CONFIG_SIFIVE_U', if_true: files('sifive_u.cpp'))
riscv_ss.add(when: 'CONFIG_SPIKE', if_true: files('spike.cpp'))
riscv_ss.add(when: 'CONFIG_MICROCHIP_PFSOC', if_true: files('microchip_pfsoc.cpp'))
riscv_ss.add(when: 'CONFIG_ACPI', if_true: files('virt-acpi-build.cpp'))
riscv_ss.add(when: 'CONFIG_RISCV_IOMMU', if_true: files(
	'riscv-iommu.cpp', 'riscv-iommu-pci.cpp', 'riscv-iommu-sys.cpp', 'riscv-iommu-hpm.cpp'))
riscv_ss.add(when: 'CONFIG_MICROBLAZE_V', if_true: files('microblaze-v-generic.cpp'))
riscv_ss.add(when: 'CONFIG_XIANGSHAN_KUNMINGHU', if_true: files('xiangshan_kmh.cpp'))

hw_arch += {'riscv': riscv_ss}
