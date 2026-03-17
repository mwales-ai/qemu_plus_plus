macio_ss = ss.source_set()
macio_ss.add(files('macio.cpp'))
macio_ss.add(when: 'CONFIG_CUDA', if_true: files('cuda.cpp'))
macio_ss.add(when: 'CONFIG_MACIO_GPIO', if_true: files('gpio.cpp'))
macio_ss.add(when: 'CONFIG_MAC_DBDMA', if_true: files('mac_dbdma.c'))
macio_ss.add(when: 'CONFIG_MAC_PMU', if_true: files('pmu.cpp'))

system_ss.add_all(when: 'CONFIG_MACIO', if_true: macio_ss)
