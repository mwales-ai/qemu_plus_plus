# core qdev-related obj files, also used by *-user and unit tests
hwcore_ss.add(files(
  'bus.cpp',
  'qdev-properties.cpp',
  'qdev.cpp',
  'resetcontainer.cpp',
  'resettable.cpp',
  'vmstate-if.cpp',
  # irq.c needed for qdev GPIO handling:
  'irq.cpp',
  'clock.cpp',
  'qdev-clock.cpp',
))

common_ss.add(files('cpu-common.cpp'))
common_ss.add(files('machine-smp.cpp'))
system_ss.add(when: 'CONFIG_FITLOADER', if_true: files('loader-fit.cpp'))
system_ss.add(when: 'CONFIG_GENERIC_LOADER', if_true: files('generic-loader.cpp'))
system_ss.add(when: 'CONFIG_GUEST_LOADER', if_true: files('guest-loader.cpp'))
system_ss.add(when: 'CONFIG_OR_IRQ', if_true: files('or-irq.cpp'))
system_ss.add(when: 'CONFIG_PLATFORM_BUS', if_true: files('platform-bus.cpp'))
system_ss.add(when: 'CONFIG_PTIMER', if_true: files('ptimer.cpp'))
system_ss.add(when: 'CONFIG_REGISTER', if_true: files('register.cpp'))
system_ss.add(when: 'CONFIG_SPLIT_IRQ', if_true: files('split-irq.cpp'))
system_ss.add(when: 'CONFIG_XILINX_AXI', if_true: files('stream.cpp'))
system_ss.add(when: 'CONFIG_PLATFORM_BUS', if_true: files('sysbus-fdt.cpp'))
system_ss.add(when: 'CONFIG_EIF', if_true: [files('eif.cpp'), zlib, libcbor, gnutls])

system_ss.add(files(
  'cpu-system.cpp',
  'fw-path-provider.cpp',
  'gpio.cpp',
  'hotplug.cpp',
  'loader.cpp',
  'machine-hmp-cmds.cpp',
  'machine-qmp-cmds.c',
  'machine.cpp',
  'nmi.cpp',
  'null-machine.cpp',
  'numa.c',
  'qdev-fw.cpp',
  'qdev-hotplug.cpp',
  'qdev-properties-system.cpp',
  'reset.cpp',
  'sysbus.cpp',
  'vm-change-state-handler.cpp',
  'clock-vmstate.cpp',
))
user_ss.add(files(
  'cpu-user.cpp',
  'qdev-user.cpp',
))
