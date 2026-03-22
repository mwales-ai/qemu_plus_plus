xtensa_ss = ss.source_set()
xtensa_ss.add(files(
  'mx_pic.cpp',
  'pic_cpu.cpp',
  'xtensa_memory.cpp',
))
xtensa_ss.add(when: 'CONFIG_XTENSA_SIM', if_true: files('sim.cpp'))
xtensa_ss.add(when: 'CONFIG_XTENSA_VIRT', if_true: files('virt.cpp'))
xtensa_ss.add(when: 'CONFIG_XTENSA_XTFPGA', if_true: files('xtfpga.cpp'))

hw_arch += {'xtensa': xtensa_ss}
