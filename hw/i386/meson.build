i386_ss = ss.source_set()
i386_ss.add(files(
  'fw_cfg.cpp',
  'e820_memory_layout.cpp',
  'monitor.cpp',
  'multiboot.cpp',
  'x86.cpp',
  'x86-cpu.cpp',
))

i386_ss.add(when: 'CONFIG_APIC', if_true: files('vapic.cpp'))
i386_ss.add(when: 'CONFIG_X86_IOMMU', if_true: files('x86-iommu.cpp'),
                                      if_false: files('x86-iommu-stub.cpp'))
i386_ss.add(when: 'CONFIG_AMD_IOMMU', if_true: files('amd_iommu.c'),
                                      if_false: files('amd_iommu-stub.cpp'))
i386_ss.add(when: 'CONFIG_I440FX', if_true: files('pc_piix.c'))
i386_ss.add(when: 'CONFIG_ISAPC', if_true: files('isapc.cpp'))
i386_ss.add(when: 'CONFIG_MICROVM', if_true: files('x86-common.cpp', 'microvm.c', 'acpi-microvm.cpp', 'microvm-dt.cpp'))
i386_ss.add(when: 'CONFIG_NITRO_ENCLAVE', if_true: files('nitro_enclave.cpp'))
i386_ss.add(when: 'CONFIG_Q35', if_true: files('pc_q35.cpp'))
i386_ss.add(when: 'CONFIG_VMMOUSE', if_true: files('vmmouse.cpp'))
i386_ss.add(when: 'CONFIG_VMPORT', if_true: files('vmport.cpp'))
i386_ss.add(when: 'CONFIG_VTD', if_true: files('intel_iommu.c'))
i386_ss.add(when: 'CONFIG_SGX', if_true: files('sgx-epc.cpp','sgx.cpp'),
                                if_false: files('sgx-stub.cpp'))

i386_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-common.cpp'))
i386_ss.add(when: 'CONFIG_PC', if_true: files(
  'x86-common.cpp',
  'pc.cpp',
  'pc_sysfw.cpp',
  'acpi-build.cpp',
  'port92.cpp'))
i386_ss.add(when: 'CONFIG_X86_FW_OVMF', if_true: files('pc_sysfw_ovmf.cpp'),
                                        if_false: files('pc_sysfw_ovmf-stubs.cpp'))
i386_ss.add(when: 'CONFIG_TDX', if_true: files('tdvf.cpp', 'tdvf-hob.cpp'))

subdir('kvm')
subdir('xen')

i386_ss.add_all(xenpv_ss)

hw_arch += {'i386': i386_ss}
