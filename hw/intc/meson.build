system_ss.add(files('intc.cpp'))
system_ss.add(when: 'CONFIG_ARM_GIC', if_true: files(
  'arm_gic.cpp',
  'arm_gic_common.cpp',
  'arm_gicv2m.cpp',
  'arm_gicv3_common.cpp',
  'arm_gicv3_its_common.cpp',
))
system_ss.add(when: 'CONFIG_ARM_GICV3', if_true: files(
  'arm_gicv3.cpp',
  'arm_gicv3_dist.cpp',
  'arm_gicv3_its.cpp',
  'arm_gicv3_redist.cpp',
))
system_ss.add(when: 'CONFIG_ALLWINNER_A10_PIC', if_true: files('allwinner-a10-pic.cpp'))
system_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files('aspeed_vic.c'))
system_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files('aspeed_intc.c'))
system_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4210_gic.cpp', 'exynos4210_combiner.cpp'))
system_ss.add(when: 'CONFIG_GOLDFISH_PIC', if_true: files('goldfish_pic.cpp'))
system_ss.add(when: 'CONFIG_HEATHROW_PIC', if_true: files('heathrow_pic.cpp'))
system_ss.add(when: 'CONFIG_I8259', if_true: files('i8259_common.cpp', 'i8259.cpp'))
system_ss.add(when: 'CONFIG_IMX', if_true: files('imx_avic.c', 'imx_gpcv2.cpp'))
system_ss.add(when: 'CONFIG_IOAPIC', if_true: files('ioapic_common.cpp'), if_false: files('ioapic-stub.cpp'))
system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_intc.c'))
system_ss.add(when: 'CONFIG_OPENPIC', if_true: files('openpic.c'))
system_ss.add(when: 'CONFIG_PL190', if_true: files('pl190.cpp'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_ic.cpp', 'bcm2836_control.cpp'))
system_ss.add(when: 'CONFIG_REALVIEW', if_true: files('realview_gic.cpp'))
system_ss.add(when: 'CONFIG_SLAVIO', if_true: files('slavio_intctl.cpp'))
system_ss.add(when: 'CONFIG_XILINX', if_true: files('xilinx_intc.cpp'))
system_ss.add(when: 'CONFIG_XLNX_ZYNQMP', if_true: files('xlnx-zynqmp-ipi.c'))
system_ss.add(when: 'CONFIG_XLNX_ZYNQMP_PMU', if_true: files('xlnx-pmu-iomod-intc.cpp'))

if config_all_devices.has_key('CONFIG_APIC') or \
   config_all_devices.has_key('CONFIG_I8259') or \
   config_all_devices.has_key('CONFIG_MC146818RTC')
    system_ss.add(files('kvm_irqcount.cpp'))
endif

specific_ss.add(when: 'CONFIG_APIC', if_true: files('apic.cpp', 'apic_common.cpp'))
arm_common_ss.add(when: 'CONFIG_ARM_GIC', if_true: files('arm_gicv3_cpuif_common.cpp'))
arm_common_ss.add(when: 'CONFIG_ARM_GICV3', if_true: files('arm_gicv3_cpuif.c'))
specific_ss.add(when: 'CONFIG_ARM_GIC_KVM', if_true: files('arm_gic_kvm.c'))
specific_ss.add(when: ['CONFIG_ARM_GIC_KVM', 'TARGET_AARCH64'], if_true: files('arm_gicv3_kvm.c', 'arm_gicv3_its_kvm.cpp'))
arm_common_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m_nvic.cpp'))
specific_ss.add(when: 'CONFIG_GRLIB', if_true: files('grlib_irqmp.cpp'))
specific_ss.add(when: 'CONFIG_IOAPIC', if_true: files('ioapic.cpp'))
specific_ss.add(when: 'CONFIG_LOONGSON_LIOINTC', if_true: files('loongson_liointc.cpp'))
specific_ss.add(when: 'CONFIG_MIPS_CPS', if_true: files('mips_gic.c'))
specific_ss.add(when: 'CONFIG_OMPIC', if_true: files('ompic.cpp'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_OPENPIC'],
		if_true: files('openpic_kvm.cpp'))
specific_ss.add(when: 'CONFIG_POWERNV', if_true: files('xics_pnv.cpp', 'pnv_xive.c', 'pnv_xive2.c'))
specific_ss.add(when: 'CONFIG_PPC_UIC', if_true: files('ppc-uic.cpp'))
specific_ss.add(when: 'CONFIG_RX_ICU', if_true: files('rx_icu.c'))
specific_ss.add(when: 'CONFIG_S390_FLIC', if_true: files('s390_flic.c'))
specific_ss.add(when: 'CONFIG_S390_FLIC_KVM', if_true: files('s390_flic_kvm.c'))
specific_ss.add(when: 'CONFIG_SH_INTC', if_true: files('sh_intc.c'))
specific_ss.add(when: 'CONFIG_RISCV_ACLINT', if_true: files('riscv_aclint.c'))
specific_ss.add(when: 'CONFIG_RISCV_APLIC', if_true: files('riscv_aplic.c'))
specific_ss.add(when: 'CONFIG_RISCV_IMSIC', if_true: files('riscv_imsic.c'))
specific_ss.add(when: 'CONFIG_SIFIVE_PLIC', if_true: files('sifive_plic.c'))
specific_ss.add(when: 'CONFIG_XICS', if_true: files('xics.c', 'xive2.c'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_XICS'],
		if_true: files('xics_kvm.c'))
specific_ss.add(when: 'CONFIG_PSERIES', if_true: files('xics_spapr.c', 'spapr_xive.c'))
specific_ss.add(when: 'CONFIG_XIVE', if_true: files('xive.c'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_XIVE'],
		if_true: files('spapr_xive_kvm.c'))
specific_ss.add(when: 'CONFIG_M68K_IRQC', if_true: files('m68k_irqc.cpp'))
specific_ss.add(when: 'CONFIG_LOONGSON_IPI_COMMON', if_true: files('loongson_ipi_common.c'))
specific_ss.add(when: 'CONFIG_LOONGSON_IPI', if_true: files('loongson_ipi.cpp'))
specific_ss.add(when: 'CONFIG_LOONGARCH_IPI', if_true: files('loongarch_ipi.cpp'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_IPI'],
                if_true: files('loongarch_ipi_kvm.cpp'))
specific_ss.add(when: 'CONFIG_LOONGARCH_PCH_PIC', if_true: files('loongarch_pch_pic.c', 'loongarch_pic_common.cpp'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_PCH_PIC'],
                if_true: files('loongarch_pic_kvm.cpp'))
specific_ss.add(when: 'CONFIG_LOONGARCH_PCH_MSI', if_true: files('loongarch_pch_msi.cpp'))
specific_ss.add(when: 'CONFIG_LOONGARCH_EXTIOI', if_true: files('loongarch_extioi.c', 'loongarch_extioi_common.cpp'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_EXTIOI'],
               if_true: files('loongarch_extioi_kvm.cpp'))
specific_ss.add(when: 'CONFIG_LOONGARCH_DINTC', if_true: files('loongarch_dintc.cpp'))
