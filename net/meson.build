system_ss.add(files(
  'announce.c',
  'checksum.cpp',
  'dgram.cpp',
  'dump.cpp',
  'eth.cpp',
  'filter-buffer.cpp',
  'filter-mirror.c',
  'filter.cpp',
  'hub.cpp',
  'net-hmp-cmds.cpp',
  'net.cpp',
  'queue.cpp',
  'socket.cpp',
  'stream.cpp',
  'stream_data.cpp',
  'util.cpp',
))

if get_option('replication').allowed() or \
    get_option('colo_proxy').allowed()
  system_ss.add(files('colo-compare.c'))
  system_ss.add(files('colo.c'))
else
  system_ss.add(files('colo-stubs.cpp'))
endif

if get_option('colo_proxy').allowed()
  system_ss.add(files('filter-rewriter.c'))
endif

system_ss.add(when: 'CONFIG_TCG', if_true: files('filter-replay.cpp'))

if have_l2tpv3
  system_ss.add(files('l2tpv3.cpp'))
endif
if enable_passt
  system_ss.add(files('passt.cpp'))
endif
system_ss.add(when: slirp, if_true: files('slirp.cpp'))
system_ss.add(when: vde, if_true: files('vde.cpp'))
if have_netmap
  system_ss.add(files('netmap.cpp'))
endif

system_ss.add(when: [libxdp, libbpf], if_true: files('af-xdp.cpp'))

if have_vhost_net_user
  system_ss.add(when: 'CONFIG_VIRTIO_NET', if_true: files('vhost-user.c'), if_false: files('vhost-user-stub.cpp'))
endif

if host_os == 'windows'
  system_ss.add(files('tap-win32.cpp'))
elif host_os == 'linux'
  system_ss.add(files('tap.cpp', 'tap-linux.cpp'))
elif host_os in bsd_oses
  system_ss.add(files('tap.cpp', 'tap-bsd.cpp'))
elif host_os == 'sunos'
  system_ss.add(files('tap.cpp', 'tap-solaris.cpp'))
else
  system_ss.add(files('tap.cpp', 'tap-stub.cpp'))
endif
if have_vhost_net_vdpa
  system_ss.add(when: 'CONFIG_VIRTIO_NET', if_true: files('vhost-vdpa.c'), if_false: files('vhost-vdpa-stub.cpp'))
endif

vmnet_files = files(
  'vmnet-common.m',
  'vmnet-bridged.m',
  'vmnet-host.cpp',
  'vmnet-shared.cpp'
)
system_ss.add(when: vmnet, if_true: vmnet_files)
subdir('can')
