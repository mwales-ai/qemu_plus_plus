system_ss.add(files('hardware-info.cpp', 'ovmf-log.cpp'))

uefi_vars_ss = ss.source_set()
if (config_all_devices.has_key('CONFIG_UEFI_VARS'))
  uefi_vars_ss.add(files('var-service-core.cpp',
                         'var-service-json.cpp',
                         'var-service-vars.cpp',
                         'var-service-auth.cpp',
                         'var-service-guid.cpp',
                         'var-service-utils.cpp',
                         'var-service-policy.cpp',
                         'var-service-sysbus.cpp'))
  uefi_vars_ss.add(when: gnutls,
                   if_true: files('var-service-pkcs7.cpp'),
                   if_false: files('var-service-pkcs7-stub.cpp'))
  uefi_vars_ss.add(files('var-service-siglist.cpp'))
endif

modules += { 'hw-uefi' : {
    'vars'     : uefi_vars_ss,
}}
