util_ss.add(files('osdep.c', 'cutils.c', 'unicode.cpp', 'qemu-timer-common.cpp'))
util_ss.add(files('thread-context.c'), numa)
if not config_host_data.get('CONFIG_ATOMIC64')
  util_ss.add(files('atomic64.c'))
endif
if host_os != 'windows'
  util_ss.add(files('aio-posix.c'))
  util_ss.add(files('fdmon-poll.c'))
  if config_host_data.get('CONFIG_EPOLL_CREATE1')
    util_ss.add(files('fdmon-epoll.c'))
  endif
  util_ss.add(files('compatfd.cpp'))
  util_ss.add(files('event_notifier-posix.c'))
  if host_os != 'emscripten'
    util_ss.add(files('mmap-alloc.cpp'))
  endif
  freebsd_dep = []
  if host_os == 'freebsd'
    freebsd_dep = util
  endif
  util_ss.add(files('oslib-posix.c'), freebsd_dep)
  util_ss.add(files('qemu-thread-posix.c'))
  util_ss.add(files('memfd.c'))
  util_ss.add(files('drm.cpp'))
else
  util_ss.add(files('aio-win32.c'))
  util_ss.add(files('event_notifier-win32.c'))
  util_ss.add(files('oslib-win32.c'))
  util_ss.add(files('qemu-thread-win32.c'))
  util_ss.add(winmm, pathcch, synchronization)
endif
util_ss.add(when: linux_io_uring, if_true: files('fdmon-io_uring.c'))
if glib_has_gslice
  util_ss.add(files('qtree.c'))
endif
util_ss.add(files('defer-call.cpp'))
util_ss.add(files('envlist.cpp', 'path.cpp', 'module.c'))
util_ss.add(files('event.cpp'))
util_ss.add(files('host-utils.cpp'))
util_ss.add(files('bitmap.cpp', 'bitops.cpp'))
util_ss.add(files('fifo8.c'))
util_ss.add(files('cacheflush.cpp'))
util_ss.add(files('error.c', 'error-report.c'))
util_ss.add(files('qemu-print.c'))
util_ss.add(files('id.cpp'))
util_ss.add(files('qemu-config.c', 'notify.cpp'))
util_ss.add(files('qemu-option.c', 'qemu-progress.cpp'))
util_ss.add(files('keyval.c'))
util_ss.add(files('crc32c.cpp'))
util_ss.add(files('uuid.cpp'))
util_ss.add(files('getauxval.cpp'))
util_ss.add(files('rcu.c'))
if have_membarrier
  util_ss.add(files('sys_membarrier.cpp'))
endif
util_ss.add(files('log.c'))
util_ss.add(files('qdist.cpp'))
util_ss.add(files('qht.c'))
util_ss.add(files('qsp.c'))
util_ss.add(files('range.cpp'))
util_ss.add(files('reserved-region.c'))
util_ss.add(files('stats64.cpp'))
util_ss.add(files('systemd.cpp'))
util_ss.add(files('transactions.cpp'))
util_ss.add(files('guest-random.cpp'))
util_ss.add(files('int128.cpp'))
util_ss.add(files('memalign.cpp'))
util_ss.add(files('interval-tree.cpp'))
util_ss.add(files('lockcnt.cpp'))

if have_user
  util_ss.add(files('selfmap.cpp'))
endif

if have_system
  util_ss.add(files('crc-ccitt.cpp'))
  util_ss.add(when: gio, if_true: files('dbus.c'))
  if host_os == 'linux'
    util_ss.add(files('userfaultfd.cpp'))
  endif
endif

if have_block or have_ga
  util_ss.add(files('aiocb.c', 'async.c'))
  util_ss.add(files('base64.cpp'))
  util_ss.add(files('main-loop.c'))
  util_ss.add(files('qemu-coroutine.c', 'qemu-coroutine-lock.c', 'qemu-coroutine-io.c'))
  util_ss.add(files(f'coroutine-@coroutine_backend@.c'))
  util_ss.add(files('thread-pool.c', 'qemu-timer.c'))
endif
if have_block or have_ga or have_user
  util_ss.add(files('qemu-sockets.c'))
endif
if have_block
  util_ss.add(files('aio-wait.c'))
  util_ss.add(files('buffer.cpp'))
  util_ss.add(files('bufferiszero.cpp'))
  util_ss.add(files('hbitmap.cpp'))
  util_ss.add(files('hexdump.cpp'))
  util_ss.add(files('iova-tree.c'))
  util_ss.add(files('iov.c'))
  util_ss.add(files('nvdimm-utils.c'))
  util_ss.add(files('block-helpers.cpp'))
  util_ss.add(files('qemu-coroutine-sleep.c'))
  util_ss.add(files('qemu-co-shared-resource.c'))
  util_ss.add(files('qemu-co-timeout.c'))
  util_ss.add(files('readline.cpp'))
  util_ss.add(files('throttle.c'))
  util_ss.add(files('timed-average.cpp'))
  if config_host_data.get('CONFIG_INOTIFY1')
    freebsd_dep = []
    if host_os == 'freebsd'
      freebsd_dep = inotify
    endif
    util_ss.add(files('filemonitor-inotify.c'), freebsd_dep)
  else
    util_ss.add(files('filemonitor-stub.cpp'))
  endif
  if host_os == 'linux'
    util_ss.add(files('vhost-user-server.c'), vhost_user)
    util_ss.add(files('vfio-helpers.c'))
    util_ss.add(files('chardev_open.cpp'))
  endif
  util_ss.add(files('yank.c'))
endif

if cpu == 'aarch64'
  util_ss.add(files('cpuinfo-aarch64.c'))
elif cpu in ['x86', 'x86_64']
  util_ss.add(files('cpuinfo-i386.cpp'))
elif cpu == 'loongarch64'
  util_ss.add(files('cpuinfo-loongarch.c'))
elif cpu in ['ppc', 'ppc64']
  util_ss.add(files('cpuinfo-ppc.c'))
elif cpu in ['riscv32', 'riscv64']
  util_ss.add(files('cpuinfo-riscv.c'))
elif cpu == 's390x'
  util_ss.add(files('s390x_pci_mmio.c'))
endif
