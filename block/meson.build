block_ss.add(genh)
block_ss.add(files(
  'accounting.cpp',
  'aio_task.c',
  'amend.cpp',
  'backup.cpp',
  'blkdebug.c',
  'blklogwrites.c',
  'blkverify.c',
  'block-backend.c',
  'block-copy.c',
  'commit.cpp',
  'copy-before-write.c',
  'copy-on-read.cpp',
  'create.cpp',
  'crypto.c',
  'dirty-bitmap.cpp',
  'filter-compress.cpp',
  'graph-lock.cpp',
  'io.c',
  'mirror.c',
  'nbd.c',
  'null.cpp',
  'preallocate.cpp',
  'progress_meter.cpp',
  'qapi.c',
  'qcow2.c',
  'qcow2-bitmap.cpp',
  'qcow2-cache.cpp',
  'qcow2-cluster.c',
  'qcow2-refcount.c',
  'qcow2-snapshot.cpp',
  'qcow2-threads.cpp',
  'quorum.c',
  'raw-format.cpp',
  'reqlist.c',
  'snapshot.c',
  'snapshot-access.cpp',
  'throttle.cpp',
  'throttle-groups.cpp',
  'write-threshold.cpp',
), zstd, zlib)

system_ss.add(when: 'CONFIG_TCG', if_true: files('blkreplay.c'))
system_ss.add(files('block-ram-registrar.c'))

if get_option('qcow1').allowed()
  block_ss.add(files('qcow.cpp'))
endif
if get_option('vdi').allowed()
  block_ss.add(files('vdi.cpp'))
endif
if get_option('vhdx').allowed()
  block_ss.add(files(
    'vhdx-endian.cpp',
    'vhdx-log.c',
    'vhdx.cpp'
  ))
endif
if get_option('vmdk').allowed()
  block_ss.add(files('vmdk.c'))
endif
if get_option('vpc').allowed()
  block_ss.add(files('vpc.cpp'))
endif
if get_option('cloop').allowed()
  block_ss.add(files('cloop.cpp'))
endif
if get_option('bochs').allowed()
  block_ss.add(files('bochs.cpp'))
endif
if get_option('vvfat').allowed()
  block_ss.add(files('vvfat.c'))
endif
if get_option('dmg').allowed()
  block_ss.add(files('dmg.cpp'))
endif
if get_option('qed').allowed()
  block_ss.add(files(
    'qed-check.cpp',
    'qed-cluster.cpp',
    'qed-l2-cache.cpp',
    'qed-table.cpp',
    'qed.c',
  ))
endif
if get_option('parallels').allowed()
  block_ss.add(files('parallels.cpp', 'parallels-ext.cpp'))
endif

if host_os == 'windows'
  block_ss.add(files('file-win32.c', 'win32-aio.c'))
else
  block_ss.add(files('file-posix.c'), coref, iokit)
endif
block_ss.add(when: libiscsi, if_true: files('iscsi-opts.cpp'))
if host_os == 'linux'
  block_ss.add(files('nvme.c'))
endif
if get_option('replication').allowed()
  block_ss.add(files('replication.cpp'))
endif
block_ss.add(when: libaio, if_true: files('linux-aio.c'))
block_ss.add(when: linux_io_uring, if_true: files('io_uring.c'))

block_modules = {}

modsrc = []
foreach m : [
  [blkio, 'blkio', files('blkio.c')],
  [curl, 'curl', files('curl.cpp')],
  [glusterfs, 'gluster', files('gluster.cpp')],
  [libiscsi, 'iscsi', files('iscsi.c')],
  [libnfs, 'nfs', files('nfs.c')],
  [libssh, 'ssh', files('ssh.cpp')],
  [rbd, 'rbd', files('rbd.c')],
]
  if m[0].found()
    module_ss = ss.source_set()
    module_ss.add(when: m[0], if_true: m[2])
    if enable_modules
      modsrc += m[2]
    endif
    block_modules += {m[1] : module_ss}
  endif
endforeach

# those are not exactly regular block modules, so treat them apart
if get_option('dmg').allowed()
  foreach m : [
    [liblzfse, 'dmg-lzfse', liblzfse, 'dmg-lzfse.cpp'],
    [libbzip2, 'dmg-bz2', [glib, libbzip2], 'dmg-bz2.cpp']
  ]
    if m[0].found()
      module_ss = ss.source_set()
      module_ss.add(when: m[2], if_true: files(m[3]))
      block_modules += {m[1] : module_ss}
    endif
  endforeach
endif

module_block_py = find_program('../scripts/modules/module_block.py')
module_block_h = custom_target('module_block.h',
                               output: 'module_block.h',
                               input: modsrc,
                               command: [module_block_py, '@OUTPUT0@', modsrc])
block_ss.add(module_block_h)

wrapper_py = find_program('../scripts/block-coroutine-wrapper.py')
block_gen_c = custom_target('block-gen.c',
                            output: 'block-gen.c',
                            input: files(
                                      '../include/block/block-io.h',
                                      '../include/block/dirty-bitmap.h',
                                      '../include/block/block_int-io.h',
                                      '../include/block/block-global-state.h',
                                      '../include/system/block-backend-global-state.h',
                                      '../include/system/block-backend-io.h',
                                      'coroutines.h'
                                      ),
                            command: [wrapper_py, '@OUTPUT@', '@INPUT@'])
block_ss.add(block_gen_c)

block_ss.add(files('stream.cpp'))

system_ss.add(files('qapi-system.cpp'))

subdir('export')
subdir('monitor')

modules += {'block': block_modules}
