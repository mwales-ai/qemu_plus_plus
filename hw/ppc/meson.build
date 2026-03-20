ppc_ss = ss.source_set()
ppc_ss.add(files(
  'ppc.cpp',
  'ppc_booke.cpp',
))
ppc_ss.add(when: 'CONFIG_FDT_PPC', if_true: files('fdt.cpp'))
ppc_ss.add(when: 'CONFIG_FW_CFG_PPC', if_true: files('fw_cfg.cpp'))

# IBM pSeries (sPAPR)
ppc_ss.add(when: 'CONFIG_PSERIES', if_true: files(
  'spapr.cpp',
  'spapr_caps.cpp',
  'spapr_vio.cpp',
  'spapr_events.cpp',
  'spapr_hcall.cpp',
  'spapr_nested.cpp',
  'spapr_iommu.cpp',
  'spapr_rtas.cpp',
  'spapr_pci.cpp',
  'spapr_rtc.cpp',
  'spapr_drc.cpp',
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
  'pnv.cpp',
  'pnv_xscom.cpp',
  'pnv_adu.cpp',
  'pnv_core.cpp',
  'pnv_i2c.cpp',
  'pnv_lpc.cpp',
  'pnv_psi.cpp',
  'pnv_chiptod.cpp',
  'pnv_occ.cpp',
  'pnv_sbe.cpp',
  'pnv_bmc.cpp',
  'pnv_homer.cpp',
  'pnv_pnor.cpp',
  'pnv_nest_pervasive.cpp',
  'pnv_n1_chiplet.cpp',
))
# PowerPC 4xx boards
ppc_ss.add(when: 'CONFIG_PPC405', if_true: files(
  'ppe42_machine.cpp'))
ppc_ss.add(when: 'CONFIG_PPC440', if_true: files(
  'ppc440_bamboo.cpp',
  'ppc440_uc.cpp'))
ppc_ss.add(when: 'CONFIG_PPC4XX', if_true: files(
  'ppc4xx_devs.cpp',
  'ppc4xx_sdram.cpp'))
ppc_ss.add(when: 'CONFIG_SAM460EX', if_true: files('sam460ex.cpp'))
# PReP
ppc_ss.add(when: 'CONFIG_PREP', if_true: files('prep.cpp'))
ppc_ss.add(when: 'CONFIG_PREP', if_true: files('prep_systemio.cpp'))
ppc_ss.add(when: 'CONFIG_RS6000_MC', if_true: files('rs6000_mc.cpp'))
# OldWorld PowerMac
ppc_ss.add(when: 'CONFIG_MAC_OLDWORLD', if_true: files('mac_oldworld.cpp'))
# NewWorld PowerMac
ppc_ss.add(when: 'CONFIG_MAC_NEWWORLD', if_true: files('mac_newworld.cpp'))
# e500
ppc_ss.add(when: 'CONFIG_E500PLAT', if_true: files('e500plat.cpp'))
ppc_ss.add(when: 'CONFIG_MPC8544DS', if_true: files('mpc8544ds.cpp'))
ppc_ss.add(when: 'CONFIG_E500', if_true: files(
  'e500.cpp',
  'mpc8544_guts.cpp',
  'ppce500_spin.cpp'
))
# PowerPC 440 Xilinx ML507 reference board.
ppc_ss.add(when: 'CONFIG_VIRTEX', if_true: files('virtex_ml507.cpp'))
# AmigaOne
ppc_ss.add(when: 'CONFIG_AMIGAONE', if_true: files('amigaone.cpp'))
# Pegasos
ppc_ss.add(when: 'CONFIG_PEGASOS', if_true: files('pegasos.cpp'))

ppc_ss.add(when: 'CONFIG_VOF', if_true: files('vof.cpp'))
ppc_ss.add(when: ['CONFIG_VOF', 'CONFIG_PSERIES'], if_true: files('spapr_vof.cpp'))

hw_arch += {'ppc': ppc_ss}
