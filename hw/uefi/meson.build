system_ss.add(files('hardware-info.cpp', 'ovmf-log.c'))

uefi_vars_ss = ss.source_set()
if (config_all_devices.has_key('CONFIG_UEFI_VARS'))
  uefi_vars_ss.add(files('var-service-core.c',
                         'var-service-json.cpp',
                         'var-service-vars.c',
                         'var-service-auth.cpp',
                         'var-service-guid.cpp',
                         'var-service-utils.c',
                         'var-service-policy.c',
                         'var-service-sysbus.c'))
  uefi_vars_ss.add(when: gnutls,
                   if_true: files('var-service-pkcs7.c'),
                   if_false: files('var-service-pkcs7-stub.cpp'))
  uefi_vars_ss.add(files('var-service-siglist.c'))
endif

modules += { 'hw-uefi' : {
    'vars'     : uefi_vars_ss,
}}
