fsdev_ss = ss.source_set()
fsdev_ss.add(files('qemu-fsdev-opts.cpp', 'qemu-fsdev-throttle.cpp'))
fsdev_ss.add(when: ['CONFIG_FSDEV_9P'], if_true: files(
  '9p-iov-marshal.cpp',
  '9p-marshal.cpp',
  'qemu-fsdev.cpp',
), if_false: files('qemu-fsdev-dummy.cpp'))
if host_os in ['linux', 'darwin', 'freebsd']
  system_ss.add_all(fsdev_ss)
endif
