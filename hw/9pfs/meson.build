fs_ss = ss.source_set()
fs_ss.add(files(
  '9p-local.c',
  '9p-posix-acl.cpp',
  '9p-synth.cpp',
  '9p-util-generic.cpp',
  '9p-xattr-user.cpp',
  '9p-xattr.cpp',
  '9p.c',
  'codir.cpp',
  'cofile.cpp',
  'cofs.cpp',
  'coth.cpp',
  'coxattr.cpp',
))
if host_os == 'darwin'
  fs_ss.add(files('9p-util-darwin.cpp'))
elif host_os == 'freebsd'
  fs_ss.add(files('9p-util-freebsd.cpp'))
elif host_os == 'linux'
  fs_ss.add(files('9p-util-linux.cpp'))
endif
fs_ss.add(when: 'CONFIG_XEN_BUS', if_true: files('xen-9p-backend.c'))
system_ss.add_all(when: 'CONFIG_FSDEV_9P', if_true: fs_ss)

specific_ss.add(when: 'CONFIG_VIRTIO_9P', if_true: files('virtio-9p-device.cpp'))
