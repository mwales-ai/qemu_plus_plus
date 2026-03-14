# Files needed by unit tests
migration_files = files(
  'migration-stats.cpp',
  'page_cache.cpp',
  'xbzrle.cpp',
  'vmstate-types.cpp',
  'vmstate.cpp',
  'qemu-file.cpp',
  'yank_functions.cpp',
)

system_ss.add(files(
  'block-dirty-bitmap.cpp',
  'block-active.cpp',
  'channel.cpp',
  'channel-block.cpp',
  'cpr.c',
  'cpr-transfer.cpp',
  'cpr-exec.cpp',
  'cpu-throttle.cpp',
  'dirtyrate.cpp',
  'exec.cpp',
  'fd.cpp',
  'file.cpp',
  'global_state.cpp',
  'migration-hmp-cmds.cpp',
  'migration.cpp',
  'multifd.cpp',
  'multifd-device-state.cpp',
  'multifd-nocomp.cpp',
  'multifd-zlib.cpp',
  'multifd-zero-page.cpp',
  'options.cpp',
  'postcopy-ram.cpp',
  'ram.cpp',
  'savevm.cpp',
  'socket.cpp',
  'tls.cpp',
  'threadinfo.cpp',
), gnutls, zlib)

if get_option('replication').allowed()
  system_ss.add(files('colo-failover.cpp', 'colo.cpp'))
else
  system_ss.add(files('colo-stubs.cpp'))
endif

system_ss.add(when: rdma, if_true: files('rdma.cpp'))
system_ss.add(when: zstd, if_true: files('multifd-zstd.cpp'))
system_ss.add(when: qpl, if_true: files('multifd-qpl.cpp'))
system_ss.add(when: uadk, if_true: files('multifd-uadk.cpp'))
system_ss.add(when: qatzip, if_true: files('multifd-qatzip.cpp'))
system_ss.add(when: 'CONFIG_VFIO',
              if_true: files('vfio.cpp'),
              if_false: files('vfio-stub.cpp'))
