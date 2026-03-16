tricore_ss = ss.source_set()
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testboard.cpp'))
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testdevice.cpp'))
tricore_ss.add(when: 'CONFIG_TRIBOARD', if_true: files('triboard.cpp'))
tricore_ss.add(when: 'CONFIG_TC27X_SOC', if_true: files('tc27x_soc.c'))

hw_arch += {'tricore': tricore_ss}
