specific_ss.add(when: 'CONFIG_SYSTEM_ONLY', if_true: [files(
  'arch_init.c',
  'globals-target.c',
)])

system_ss.add(files(
  'vl.cpp',
), sdl, libpmem, libdaxctl)

system_ss.add(files(
  'balloon.cpp',
  'bootdevice.cpp',
  'cpus.cpp',
  'cpu-timers.cpp',
  'datadir.cpp',
  'dirtylimit.cpp',
  'dma-helpers.cpp',
  'exit-with-parent.cpp',
  'globals.cpp',
  'ioport.cpp',
  'ram-block-attributes.cpp',
  'memory_mapping.cpp',
  'memory.cpp',
  'physmem.cpp',
  'qdev-monitor.cpp',
  'qtest.cpp',
  'rtc.cpp',
  'runstate-action.cpp',
  'runstate-hmp-cmds.cpp',
  'runstate.cpp',
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
