qmp_ss.add(files('monitor.cpp', 'qmp.cpp', 'qmp-cmds-control.cpp'))

system_ss.add(files(
  'fds.cpp',
  'hmp-cmds.cpp',
  'hmp.cpp',
  'qemu-config-qmp.cpp',
))
system_ss.add([spice_headers, files('qmp-cmds.cpp')])

specific_ss.add(when: 'CONFIG_SYSTEM_ONLY',
		if_true: [files( 'hmp-cmds-target.c', 'hmp-target.c'), spice])
