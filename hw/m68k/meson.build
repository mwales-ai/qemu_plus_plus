m68k_ss = ss.source_set()
m68k_ss.add(when: 'CONFIG_AN5206', if_true: files('an5206.cpp', 'mcf5206.cpp'))
m68k_ss.add(when: 'CONFIG_MCF5208', if_true: files('mcf5208.cpp', 'mcf_intc.cpp'))
m68k_ss.add(when: 'CONFIG_NEXTCUBE', if_true: files('next-kbd.cpp', 'next-cube.cpp'))
m68k_ss.add(when: 'CONFIG_Q800', if_true: files('q800.cpp', 'q800-glue.cpp'))
m68k_ss.add(when: 'CONFIG_M68K_VIRT', if_true: files('virt.cpp'))

hw_arch += {'m68k': m68k_ss}
