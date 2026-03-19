system_ss.add([files(
  'cryptodev-builtin.cpp',
  'cryptodev-hmp-cmds.cpp',
  'cryptodev.cpp',
  'hostmem-ram.cpp',
  'hostmem.cpp',
  'rng-builtin.cpp',
  'rng-egd.cpp',
  'rng.cpp',
  'confidential-guest-support.cpp',
), numa])

if host_os != 'windows'
  system_ss.add(files('rng-random.cpp'))
  if host_os != 'emscripten'
    system_ss.add(files('hostmem-file.cpp'))
    system_ss.add([files('hostmem-shm.cpp'), rt])
  endif
endif
if host_os == 'linux'
  system_ss.add(files('hostmem-memfd.cpp'))
  system_ss.add(files('host_iommu_device.cpp'))
endif
if keyutils.found()
    system_ss.add(keyutils, files('cryptodev-lkcf.cpp'))
endif
if have_vhost_user
  system_ss.add(when: 'CONFIG_VIRTIO', if_true: files('vhost-user.cpp'))
endif
system_ss.add(when: 'CONFIG_VIRTIO_CRYPTO', if_true: files('cryptodev-vhost.cpp'))
system_ss.add(when: 'CONFIG_IOMMUFD', if_true: files('iommufd.c'))
if have_vhost_user_crypto
  system_ss.add(when: 'CONFIG_VIRTIO_CRYPTO', if_true: files('cryptodev-vhost-user.cpp'))
endif
system_ss.add(when: gio, if_true: files('dbus-vmstate.c'))
system_ss.add(when: 'CONFIG_SGX', if_true: files('hostmem-epc.cpp'))
if igvm.found()
  system_ss.add(igvm)
  system_ss.add(files('igvm-cfg.cpp'), igvm)
  system_ss.add(files('igvm.cpp'), igvm)
endif

system_ss.add(when: 'CONFIG_SPDM_SOCKET', if_true: files('spdm-socket.cpp'))

subdir('tpm')
