# SPDX-License-Identifier: GPL-2.0-or-later

vfio_ss = ss.source_set()
vfio_ss.add(files(
  'listener.cpp',
  'container.cpp',
  'container-legacy.cpp',
  'helpers.cpp',
))
vfio_ss.add(when: 'CONFIG_PSERIES', if_true: files('spapr.cpp'))
vfio_ss.add(when: 'CONFIG_VFIO_PCI', if_true: files(
  'pci-quirks.cpp',
  'pci.cpp',
))
vfio_ss.add(when: 'CONFIG_VFIO_CCW', if_true: files('ccw.cpp'))
vfio_ss.add(when: 'CONFIG_VFIO_AP', if_true: files('ap.cpp'))
vfio_ss.add(when: 'CONFIG_VFIO_IGD', if_true: files('igd.cpp'))

specific_ss.add_all(when: 'CONFIG_VFIO', if_true: vfio_ss)

system_ss.add(when: 'CONFIG_VFIO', if_true: files(
  'cpr.cpp',
  'cpr-legacy.cpp',
  'device.cpp',
  'migration.cpp',
  'migration-multifd.cpp',
  'region.cpp',
))
system_ss.add(when: ['CONFIG_VFIO', 'CONFIG_IOMMUFD'], if_true: files(
  'iommufd.cpp',
  'cpr-iommufd.cpp',
))
system_ss.add(when: 'CONFIG_IOMMUFD', if_false: files('iommufd-stubs.cpp'))
system_ss.add(when: 'CONFIG_VFIO_PCI', if_true: files(
  'display.cpp',
))
