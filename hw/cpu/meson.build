system_ss.add(files('core.c'))
system_ss.add(when: 'CONFIG_CPU_CLUSTER', if_true: files('cluster.cpp'))

system_ss.add(when: 'CONFIG_ARM11MPCORE', if_true: files('arm11mpcore.cpp'))
system_ss.add(when: 'CONFIG_REALVIEW', if_true: files('realview_mpcore.cpp'))
system_ss.add(when: 'CONFIG_A9MPCORE', if_true: files('a9mpcore.cpp'))
specific_ss.add(when: 'CONFIG_A15MPCORE', if_true: files('a15mpcore.c'))
