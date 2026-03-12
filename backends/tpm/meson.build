if have_tpm
  system_ss.add(files('tpm_backend.cpp'))
  system_ss.add(files('tpm_util.cpp'))
  system_ss.add(when: 'CONFIG_TPM_PASSTHROUGH', if_true: files('tpm_passthrough.cpp'))
  system_ss.add(when: 'CONFIG_TPM_EMULATOR', if_true: files('tpm_emulator.cpp'))
endif
