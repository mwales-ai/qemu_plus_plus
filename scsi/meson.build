block_ss.add(files('utils.cpp'))
if host_os == 'linux'
  block_ss.add(files('pr-manager.cpp', 'pr-manager-helper.cpp'))
else
  block_ss.add(files('pr-manager-stub.cpp'))
endif
