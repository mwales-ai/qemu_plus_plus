ppc_ss = ss.source_set()
ppc_ss.add(files(
  'ppc.cpp',
  'ppc_booke.cpp',
))
ppc_ss.add(when: 'CONFIG_FDT_PPC', if_true: files('fdt.cpp'))
ppc_ss.add(when: 'CONFIG_FW_CFG_PPC', if_true: files('fw_cfg.cpp'))

# IBM pSeries (sPAPR)
ppc_ss.add(when: 'CONFIG_PSERIES', if_true: files(
  'spapr.c',
  'spapr_caps.cpp',
  'spapr_vio.c',
  'spapr_events.cpp',
  'spapr_hcall.c',
  'spapr_nested.cpp',
  'spapr_iommu.cpp',
  'spapr_rtas.cpp',
  'spapr_pci.c',
  'spapr_rtc.cpp',
  'spapr_drc.c',
  'spapr_cpu_core.cpp',
  'spapr_ovec.cpp',
  'spapr_irq.cpp',
  'spapr_tpm_proxy.cpp',
  'spapr_nvdimm.cpp',
  'spapr_rtas_ddw.cpp',
  'spapr_numa.cpp',
  'spapr_fadump.cpp',
  'pef.cpp',
))
ppc_ss.add(when: ['CONFIG_PSERIES', 'CONFIG_TCG'], if_true: files(
  'spapr_vhyp_mmu.cpp',
))
ppc_ss.add(when: 'CONFIG_SPAPR_RNG', if_true: files('spapr_rng.cpp'))
if host_os == 'linux'
  ppc_ss.add(when: 'CONFIG_PSERIES', if_true: files(
    'spapr_pci_vfio.cpp',
  ))
endif

# IBM PowerNV
ppc_ss.add(when: 'CONFIG_POWERNV', if_true: files(
  'pnv.c',
  'pnv_xscom.c',
  'pnv_adu.c',
  'pnv_core.c',
  'pnv_i2c.c',
  'pnv_lpc.c',
  'pnv_psi.c',
  'pnv_chiptod.c',
  'pnv_occ.c',
  'pnv_sbe.c',
  'pnv_bmc.c',
  'pnv_homer.c',
  'pnv_pnor.c',
  'pnv_nest_pervasive.c',
  'pnv_n1_chiplet.c',
))
# PowerPC 4xx boards
ppc_ss.add(when: 'CONFIG_PPC405', if_true: files(
  'ppe42_machine.c'))
ppc_ss.add(when: 'CONFIG_PPC440', if_true: files(
  'ppc440_bamboo.c',
  'ppc440_uc.c'))
ppc_ss.add(when: 'CONFIG_PPC4XX', if_true: files(
  'ppc4xx_devs.c',
  'ppc4xx_sdram.c'))
ppc_ss.add(when: 'CONFIG_SAM460EX', if_true: files('sam460ex.c'))
# PReP
ppc_ss.add(when: 'CONFIG_PREP', if_true: files('prep.c'))
ppc_ss.add(when: 'CONFIG_PREP', if_true: files('prep_systemio.c'))
ppc_ss.add(when: 'CONFIG_RS6000_MC', if_true: files('rs6000_mc.c'))
# OldWorld PowerMac
ppc_ss.add(when: 'CONFIG_MAC_OLDWORLD', if_true: files('mac_oldworld.c'))
# NewWorld PowerMac
ppc_ss.add(when: 'CONFIG_MAC_NEWWORLD', if_true: files('mac_newworld.c'))
# e500
ppc_ss.add(when: 'CONFIG_E500PLAT', if_true: files('e500plat.c'))
ppc_ss.add(when: 'CONFIG_MPC8544DS', if_true: files('mpc8544ds.c'))
ppc_ss.add(when: 'CONFIG_E500', if_true: files(
  'e500.c',
  'mpc8544_guts.c',
  'ppce500_spin.c'
))
# PowerPC 440 Xilinx ML507 reference board.
ppc_ss.add(when: 'CONFIG_VIRTEX', if_true: files('virtex_ml507.c'))
# AmigaOne
ppc_ss.add(when: 'CONFIG_AMIGAONE', if_true: files('amigaone.c'))
# Pegasos
ppc_ss.add(when: 'CONFIG_PEGASOS', if_true: files('pegasos.c'))

ppc_ss.add(when: 'CONFIG_VOF', if_true: files('vof.cpp'))
ppc_ss.add(when: ['CONFIG_VOF', 'CONFIG_PSERIES'], if_true: files('spapr_vof.cpp'))

hw_arch += {'ppc': ppc_ss}
