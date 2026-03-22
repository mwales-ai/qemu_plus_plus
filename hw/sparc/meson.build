sparc_ss = ss.source_set()
sparc_ss.add(when: 'CONFIG_LEON3', if_true: files('leon3.cpp'))
sparc_ss.add(when: 'CONFIG_SUN4M', if_true: files('sun4m.cpp'))
sparc_ss.add(when: 'CONFIG_SUN4M', if_true: files('sun4m_iommu.cpp'))

hw_arch += {'sparc': sparc_ss}
