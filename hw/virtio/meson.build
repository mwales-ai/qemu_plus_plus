system_virtio_ss = ss.source_set()
system_virtio_ss.add(files('virtio-bus.cpp'))
system_virtio_ss.add(files('iothread-vq-mapping.cpp'))
system_virtio_ss.add(files('virtio-config-io.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_PCI', if_true: files('virtio-pci.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_MMIO', if_true: files('virtio-mmio.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_CRYPTO', if_true: files('virtio-crypto.cpp'))
system_virtio_ss.add(when: 'CONFIG_VHOST_VSOCK_COMMON', if_true: files('vhost-vsock-common.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_IOMMU', if_true: files('virtio-iommu.cpp'))
system_virtio_ss.add(when: 'CONFIG_VHOST_VDPA_DEV', if_true: files('vdpa-dev.c'))

specific_virtio_ss = ss.source_set()
specific_virtio_ss.add(files('virtio.cpp'))
specific_virtio_ss.add(files('virtio-qmp.c'))

if have_vhost
  system_virtio_ss.add(files('vhost.cpp'))
  system_virtio_ss.add(files('vhost-backend.cpp', 'vhost-iova-tree.cpp'))
  if have_vhost_user
    # fixme - this really should be generic
    specific_virtio_ss.add(files('vhost-user.cpp'))
    system_virtio_ss.add(files('vhost-user-base.cpp'))

    # MMIO Stubs
    system_virtio_ss.add(when: 'CONFIG_VHOST_USER_TEST', if_true: files('vhost-user-test-device.cpp'))
    system_virtio_ss.add(when: 'CONFIG_VHOST_USER_GPIO', if_true: files('vhost-user-gpio.cpp'))
    system_virtio_ss.add(when: 'CONFIG_VHOST_USER_I2C', if_true: files('vhost-user-i2c.cpp'))
    system_virtio_ss.add(when: 'CONFIG_VHOST_USER_RNG', if_true: files('vhost-user-rng.cpp'))
    system_virtio_ss.add(when: 'CONFIG_VHOST_USER_SND', if_true: files('vhost-user-snd.cpp'))
    system_virtio_ss.add(when: 'CONFIG_VHOST_USER_INPUT', if_true: files('vhost-user-input.cpp'))

    # PCI Stubs
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_TEST'],
                         if_true: files('vhost-user-test-device-pci.cpp'))
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_GPIO'],
                         if_true: files('vhost-user-gpio-pci.cpp'))
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_I2C'],
                         if_true: files('vhost-user-i2c-pci.cpp'))
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_RNG'],
                         if_true: files('vhost-user-rng-pci.cpp'))
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_SND'],
                         if_true: files('vhost-user-snd-pci.cpp'))
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_INPUT'],
                         if_true: files('vhost-user-input-pci.cpp'))
  endif
  if have_vhost_vdpa
    system_virtio_ss.add(files('vhost-vdpa.c'))
    system_virtio_ss.add(files('vhost-shadow-virtqueue.c'))
  endif
else
  system_virtio_ss.add(files('vhost-stub.cpp'))
endif
system_virtio_ss.add(when: 'CONFIG_VHOST_USER_VSOCK', if_true: files('vhost-user-vsock.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_RNG', if_true: files('virtio-rng.cpp'))

specific_virtio_ss.add(when: 'CONFIG_VIRTIO_BALLOON', if_true: files('virtio-balloon.cpp'))
specific_virtio_ss.add(when: 'CONFIG_VHOST_USER_FS', if_true: files('vhost-user-fs.cpp'))
specific_virtio_ss.add(when: 'CONFIG_VIRTIO_PMEM', if_true: files('virtio-pmem.cpp'))
specific_virtio_ss.add(when: 'CONFIG_VHOST_VSOCK', if_true: files('vhost-vsock.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_MEM', if_true: files('virtio-mem.cpp'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_NSM', if_true: files('virtio-nsm.c'))
system_virtio_ss.add(when: 'CONFIG_VIRTIO_NSM', if_true: [files('cbor-helpers.cpp'), libcbor])
system_virtio_ss.add(when: 'CONFIG_VHOST_USER_SCMI', if_true: files('vhost-user-scmi.c'))

virtio_pci_ss = ss.source_set()
virtio_pci_ss.add(when: 'CONFIG_VHOST_VSOCK', if_true: files('vhost-vsock-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_USER_VSOCK', if_true: files('vhost-user-vsock-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_USER_BLK', if_true: files('vhost-user-blk-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_USER_SCSI', if_true: files('vhost-user-scsi-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_SCSI', if_true: files('vhost-scsi-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_USER_FS', if_true: files('vhost-user-fs-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_USER_SCMI', if_true: files('vhost-user-scmi-pci.cpp'))

virtio_pci_ss.add(when: 'CONFIG_VIRTIO_CRYPTO', if_true: files('virtio-crypto-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_INPUT_HOST', if_true: files('virtio-input-host-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_INPUT', if_true: files('virtio-input-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_RNG', if_true: files('virtio-rng-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_NSM', if_true: [files('virtio-nsm-pci.cpp', 'cbor-helpers.cpp'), libcbor])
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_BALLOON', if_true: files('virtio-balloon-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_9P', if_true: files('virtio-9p-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_SCSI', if_true: files('virtio-scsi-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_BLK', if_true: files('virtio-blk-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_NET', if_true: files('virtio-net-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_SERIAL', if_true: files('virtio-serial-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_PMEM', if_true: files('virtio-pmem-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_IOMMU', if_true: files('virtio-iommu-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_MEM', if_true: files('virtio-mem-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VHOST_VDPA_DEV', if_true: files('vdpa-dev-pci.cpp'))
virtio_pci_ss.add(when: 'CONFIG_VIRTIO_MD', if_true: files('virtio-md-pci.c'))

system_virtio_ss.add_all(when: 'CONFIG_VIRTIO_PCI', if_true: virtio_pci_ss)

system_ss.add_all(when: 'CONFIG_VIRTIO', if_true: system_virtio_ss)
system_ss.add(when: 'CONFIG_VIRTIO', if_false: files('vhost-stub.cpp'))
system_ss.add(when: 'CONFIG_VIRTIO', if_false: files('virtio-stub.cpp'))
system_ss.add(when: ['CONFIG_VIRTIO_MD', 'CONFIG_VIRTIO_PCI'],
              if_false: files('virtio-md-stubs.cpp'))

system_ss.add(files('virtio-hmp-cmds.cpp'))

specific_ss.add_all(when: 'CONFIG_VIRTIO', if_true: specific_virtio_ss)
system_ss.add(when: 'CONFIG_ACPI', if_true: files('virtio-acpi.cpp'))
