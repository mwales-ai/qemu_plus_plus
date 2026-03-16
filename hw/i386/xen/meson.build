i386_ss.add(when: 'CONFIG_XEN', if_true: files(
  'xen_apic.cpp',
  'xen_pvdevice.cpp',
))
i386_ss.add(when: ['CONFIG_XEN', xen], if_true: files(
  'xen-hvm.c',
  'xen-pvh.c',
))

i386_ss.add(when: 'CONFIG_XEN_BUS', if_true: files(
  'xen_platform.c',
))
