specific_ss.add(when: 'CONFIG_SYSTEM_ONLY', if_true: [files(
  'arch_init.c',
  'globals-target.c',
)])

system_ss.add(files(
  'vl.c',
), sdl, libpmem, libdaxctl)

system_ss.add(files(
  'balloon.c',
  'bootdevice.cpp',
  'cpus.c',
  'cpu-timers.cpp',
  'datadir.c',
  'dirtylimit.c',
  'dma-helpers.c',
  'exit-with-parent.cpp',
  'globals.cpp',
  'ioport.c',
  'ram-block-attributes.c',
  'memory_mapping.cpp',
  'memory.c',
  'physmem.c',
  'qdev-monitor.cpp',
  'qtest.cpp',
  'rtc.cpp',
  'runstate-action.cpp',
  'runstate-hmp-cmds.cpp',
  'runstate.c',
  'tpm-hmp-cmds.cpp',
  'watchpoint.cpp',
))

if have_tpm
  system_ss.add(files('tpm.cpp'))
endif

system_ss.add(when: seccomp, if_true: files('qemu-seccomp.cpp'))
system_ss.add(when: 'CONFIG_DEVICE_TREE',
              if_true: [fdt, files('device_tree.cpp')],
              if_false: files('device_tree-stub.cpp'))
if host_os == 'linux'
  system_ss.add(files('async-teardown.cpp'))
endif
