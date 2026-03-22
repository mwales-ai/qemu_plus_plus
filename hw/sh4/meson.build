sh4_ss = ss.source_set()
sh4_ss.add(when: 'CONFIG_SH7750', if_true: files(
  'sh7750.cpp',
  'sh7750_regnames.cpp',
))
sh4_ss.add(when: 'CONFIG_R2D', if_true: files('r2d.cpp'))

hw_arch += {'sh4': sh4_ss}
