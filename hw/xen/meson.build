system_ss.add(when: ['CONFIG_XEN_BUS'], if_true: files(
  'xen-backend.cpp',
  'xen-bus-helper.cpp',
  'xen-bus.cpp',
  'xen-legacy-backend.cpp',
  'xen_devconfig.cpp',
  'xen_pvdev.cpp',
))

xen_common_ss = ss.source_set()
xen_common_ss.add(files(
  'xen-hvm-common.cpp',
  'xen-mapcache.cpp',
  'xen-operations.cpp',
  'xen-pvh-common.cpp',
))

if have_xen_pci_passthrough
  xen_common_ss.add(files(
    'xen-host-pci-device.cpp',
    'xen_pt.cpp',
    'xen_pt_config_init.cpp',
    'xen_pt_graphics.cpp',
    'xen_pt_load_rom.cpp',
    'xen_pt_msi.cpp',
  ))
else
  xen_common_ss.add(files('xen_pt_stub.cpp'))
endif

system_ss.add_all(when: ['CONFIG_XEN', xen], if_true: xen_common_ss)
system_ss.add(when: ['CONFIG_XEN', xen], if_false: files('xen_stubs.cpp'))
