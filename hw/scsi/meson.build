scsi_ss = ss.source_set()
specific_scsi_ss = ss.source_set()
virtio_scsi_ss = ss.source_set()
specific_virtio_scsi_ss = ss.source_set()

scsi_ss.add(files(
  'emulation.cpp',
  'scsi-bus.cpp',
  'scsi-disk.cpp',
  'scsi-generic.cpp',
))
scsi_ss.add(when: 'CONFIG_ESP', if_true: files('esp.cpp'))
scsi_ss.add(when: 'CONFIG_ESP_PCI', if_true: files('esp-pci.cpp'))
scsi_ss.add(when: 'CONFIG_LSI_SCSI_PCI', if_true: files('lsi53c895a.cpp'))
scsi_ss.add(when: 'CONFIG_NCR710_SCSI', if_true: files('ncr53c710.cpp', 'lasi_ncr710.cpp'))
scsi_ss.add(when: 'CONFIG_MEGASAS_SCSI_PCI', if_true: files('megasas.cpp'))
scsi_ss.add(when: 'CONFIG_MPTSAS_SCSI_PCI', if_true: files('mptsas.cpp', 'mptconfig.cpp', 'mptendian.cpp'))
scsi_ss.add(when: 'CONFIG_VMW_PVSCSI_SCSI_PCI', if_true: files('vmw_pvscsi.cpp'))

virtio_scsi_ss.add(files('virtio-scsi-dataplane.cpp'))
virtio_scsi_ss.add(when: 'CONFIG_VHOST_SCSI', if_true: files('vhost-scsi.cpp'))
virtio_scsi_ss.add(when: 'CONFIG_VHOST_USER_SCSI', if_true: files('vhost-user-scsi.cpp'))

specific_virtio_scsi_ss.add(files('virtio-scsi.cpp'))
specific_virtio_scsi_ss.add(when: 'CONFIG_VHOST_SCSI_COMMON', if_true: files('vhost-scsi-common.cpp'))

specific_scsi_ss.add_all(when: 'CONFIG_VIRTIO_SCSI', if_true: specific_virtio_scsi_ss)
scsi_ss.add_all(when: 'CONFIG_VIRTIO_SCSI', if_true: virtio_scsi_ss)

specific_scsi_ss.add(when: 'CONFIG_SPAPR_VSCSI', if_true: files('spapr_vscsi.cpp'))

system_ss.add_all(when: 'CONFIG_SCSI', if_true: scsi_ss)
specific_ss.add_all(when: 'CONFIG_SCSI', if_true: specific_scsi_ss)
