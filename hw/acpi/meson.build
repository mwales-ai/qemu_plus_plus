acpi_ss = ss.source_set()
acpi_ss.add(files(
  'acpi_interface.cpp',
  'aml-build.c',
  'bios-linker-loader.cpp',
  'core.c',
  'utils.cpp',
))
acpi_ss.add(when: 'CONFIG_ACPI_CPU_HOTPLUG', if_true: files('cpu.cpp', 'cpu_hotplug.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_CPU_HOTPLUG', if_false: files('acpi-cpu-hotplug-stub.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_MEMORY_HOTPLUG', if_true: files('memory_hotplug.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_MEMORY_HOTPLUG', if_false: files('acpi-mem-hotplug-stub.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_NVDIMM', if_true: files('nvdimm.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_NVDIMM', if_false: files('acpi-nvdimm-stub.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_PCI', if_true: files('pci.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_CXL', if_true: files('cxl.cpp'), if_false: files('cxl-stub.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_VMGENID', if_true: files('vmgenid.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_VMCLOCK', if_true: files('vmclock.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_HW_REDUCED', if_true: files('generic_event_device.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_HMAT', if_true: files('hmat.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_APEI', if_true: files('ghes.cpp'), if_false: files('ghes-stub.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_PIIX4', if_true: files('piix4.c'))
acpi_ss.add(when: 'CONFIG_ACPI_PCI_BRIDGE', if_true: files('pci-bridge.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_PCIHP', if_true: files('pcihp.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_PCIHP', if_false: files('acpi-pci-hotplug-stub.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_VIOT', if_true: files('viot.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_ICH9', if_true: files('ich9.c', 'ich9_tco.cpp', 'ich9_timer.cpp'))
acpi_ss.add(when: 'CONFIG_ACPI_ERST', if_true: files('erst.cpp'))
acpi_ss.add(when: 'CONFIG_IPMI', if_true: files('ipmi.cpp'), if_false: files('ipmi-stub.cpp'))
acpi_ss.add(when: 'CONFIG_PC', if_false: files('acpi-x86-stub.cpp'))
if have_tpm
  acpi_ss.add(files('tpm.cpp'))
endif
system_ss.add(when: 'CONFIG_ACPI', if_false: files('acpi-stub.cpp', 'aml-build-stub.cpp', 'ghes-stub.cpp', 'acpi_interface.cpp'))
system_ss.add(when: 'CONFIG_ACPI_PCI_BRIDGE', if_false: files('pci-bridge-stub.cpp'))
system_ss.add_all(when: 'CONFIG_ACPI', if_true: acpi_ss)
system_ss.add(when: 'CONFIG_GHES_CPER', if_true: files('ghes_cper.cpp'))
system_ss.add(when: 'CONFIG_GHES_CPER', if_false: files('ghes_cper_stub.cpp'))
system_ss.add(files('acpi-qmp-cmds.cpp'))
