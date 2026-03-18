system_ss.add(when: 'CONFIG_DP8393X', if_true: files('dp8393x.cpp'))
system_ss.add(when: 'CONFIG_XEN_BUS', if_true: files('xen_nic.c'))
system_ss.add(when: 'CONFIG_NE2000_COMMON', if_true: files('ne2000.c'))

# PCI network cards
system_ss.add(when: 'CONFIG_NE2000_PCI', if_true: files('ne2000-pci.cpp'))
system_ss.add(when: 'CONFIG_EEPRO100_PCI', if_true: files('eepro100.cpp'))
system_ss.add(when: 'CONFIG_PCNET_PCI', if_true: files('pcnet-pci.cpp'))
system_ss.add(when: 'CONFIG_PCNET_COMMON', if_true: files('pcnet.cpp'))
system_ss.add(when: 'CONFIG_E1000_PCI', if_true: files('e1000.cpp', 'e1000x_common.cpp'))
system_ss.add(when: 'CONFIG_E1000E_PCI_EXPRESS', if_true: files('net_tx_pkt.cpp', 'net_rx_pkt.cpp'))
system_ss.add(when: 'CONFIG_E1000E_PCI_EXPRESS', if_true: files('e1000e.cpp', 'e1000e_core.cpp', 'e1000x_common.cpp'))
system_ss.add(when: 'CONFIG_IGB_PCI_EXPRESS', if_true: files('net_tx_pkt.cpp', 'net_rx_pkt.cpp'))
system_ss.add(when: 'CONFIG_IGB_PCI_EXPRESS', if_true: files('igb.cpp', 'igbvf.cpp', 'igb_core.cpp'))
system_ss.add(when: 'CONFIG_RTL8139_PCI', if_true: files('rtl8139.cpp'))
system_ss.add(when: 'CONFIG_TULIP', if_true: files('tulip.cpp'))
system_ss.add(when: 'CONFIG_VMXNET3_PCI', if_true: files('net_tx_pkt.cpp', 'net_rx_pkt.cpp'))
system_ss.add(when: 'CONFIG_VMXNET3_PCI', if_true: files('vmxnet3.cpp'))

system_ss.add(when: 'CONFIG_SMC91C111', if_true: files('smc91c111.c'))
system_ss.add(when: 'CONFIG_LAN9118', if_true: files('lan9118.cpp'))
system_ss.add(when: 'CONFIG_LAN9118_PHY', if_true: files('lan9118_phy.cpp'))
system_ss.add(when: 'CONFIG_NE2000_ISA', if_true: files('ne2000-isa.cpp'))
system_ss.add(when: 'CONFIG_OPENCORES_ETH', if_true: files('opencores_eth.cpp'))
system_ss.add(when: 'CONFIG_XGMAC', if_true: files('xgmac.c'))
system_ss.add(when: 'CONFIG_XILINX_AXI', if_true: files('xilinx_axienet.cpp'))
system_ss.add(when: 'CONFIG_ALLWINNER_EMAC', if_true: files('allwinner_emac.c'))
system_ss.add(when: 'CONFIG_ALLWINNER_SUN8I_EMAC', if_true: files('allwinner-sun8i-emac.c'))
system_ss.add(when: 'CONFIG_IMX_FEC', if_true: files('imx_fec.cpp'))
system_ss.add(when: 'CONFIG_MSF2', if_true: files('msf2-emac.c'))
system_ss.add(when: 'CONFIG_MARVELL_88W8618', if_true: files('mv88w8618_eth.c'))

system_ss.add(when: 'CONFIG_CADENCE', if_true: files('cadence_gem.cpp'))
system_ss.add(when: 'CONFIG_STELLARIS_ENET', if_true: files('stellaris_enet.cpp'))
system_ss.add(when: 'CONFIG_LANCE', if_true: files('lance.cpp'))
system_ss.add(when: 'CONFIG_LASI_82596', if_true: files('lasi_i82596.cpp'))
system_ss.add(when: 'CONFIG_I82596_COMMON', if_true: files('i82596.c'))
system_ss.add(when: 'CONFIG_SUNHME', if_true: files('sunhme.c'))
system_ss.add(when: 'CONFIG_FTGMAC100', if_true: files('ftgmac100.cpp'))
system_ss.add(when: 'CONFIG_SUNGEM', if_true: files('sungem.c'))
system_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx_emc.c', 'npcm_gmac.c'))
system_ss.add(when: 'CONFIG_NPCM8XX', if_true: files('npcm_pcs.cpp'))

system_ss.add(when: 'CONFIG_COLDFIRE', if_true: files('mcf_fec.cpp'))
specific_ss.add(when: 'CONFIG_PSERIES', if_true: files('spapr_llan.c'))
system_ss.add(when: 'CONFIG_XILINX_ETHLITE', if_true: files('xilinx_ethlite.cpp'))

system_ss.add(when: 'CONFIG_VIRTIO_NET', if_true: files('net_rx_pkt.cpp'))
specific_ss.add(when: 'CONFIG_VIRTIO_NET', if_true: files('virtio-net.cpp'))

if have_vhost_net
  system_ss.add(when: 'CONFIG_VIRTIO_NET', if_true: files('vhost_net.c'), if_false: files('vhost_net-stub.cpp'))
else
  system_ss.add(files('vhost_net-stub.cpp'))
endif

system_ss.add(when: 'CONFIG_ETSEC', if_true: files(
  'fsl_etsec/etsec.c',
  'fsl_etsec/miim.cpp',
  'fsl_etsec/registers.cpp',
  'fsl_etsec/rings.c',
))

system_ss.add(when: 'CONFIG_ROCKER', if_true: files(
  'rocker/rocker.c',
  'rocker/rocker_desc.cpp',
  'rocker/rocker_fp.cpp',
  'rocker/rocker_of_dpa.c',
  'rocker/rocker_world.cpp',
), if_false: files('rocker/qmp-norocker.cpp'))
system_ss.add(files('rocker/rocker-hmp-cmds.cpp'))

subdir('can')
