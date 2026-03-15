smbios_ss = ss.source_set()
smbios_ss.add(files('smbios.c'))
smbios_ss.add(when: 'CONFIG_IPMI',
              if_true: files('smbios_type_38.c'),
              if_false: files('smbios_type_38-stub.cpp'))

smbios_ss.add(when: 'CONFIG_SMBIOS_LEGACY',
              if_true: files('smbios_legacy.cpp'),
              if_false: files('smbios_legacy_stub.cpp'))

system_ss.add_all(when: 'CONFIG_SMBIOS', if_true: smbios_ss)
system_ss.add(when: 'CONFIG_SMBIOS', if_false: files('smbios-stub.cpp'))
