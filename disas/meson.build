common_ss.add(when: 'CONFIG_ALPHA_DIS', if_true: files('alpha.cpp'))
common_ss.add(when: 'CONFIG_HEXAGON_DIS', if_true: files('hexagon.cpp'))
common_ss.add(when: 'CONFIG_HPPA_DIS', if_true: files('hppa.cpp'))
common_ss.add(when: 'CONFIG_M68K_DIS', if_true: files('m68k.cpp'))
common_ss.add(when: 'CONFIG_MICROBLAZE_DIS', if_true: files('microblaze.cpp'))
common_ss.add(when: 'CONFIG_MIPS_DIS', if_true: files('mips.cpp', 'nanomips.cpp'))
common_ss.add(when: 'CONFIG_RISCV_DIS', if_true: files(
    'riscv.cpp',
    'riscv-xthead.cpp',
    'riscv-xventana.cpp'
))
common_ss.add(when: 'CONFIG_SH4_DIS', if_true: files('sh4.cpp'))
common_ss.add(when: 'CONFIG_SPARC_DIS', if_true: files('sparc.cpp'))
common_ss.add(when: 'CONFIG_XTENSA_DIS', if_true: files('xtensa.cpp'))
common_ss.add(when: capstone, if_true: [files('capstone.cpp'), capstone])
common_ss.add(when: 'CONFIG_TCG', if_true: files(
    'disas-host.cpp',
    'disas-target.cpp',
    'objdump.cpp'
))
common_ss.add(files('disas-common.cpp'))
system_ss.add(files('disas-mon.cpp'))
specific_ss.add(capstone)
