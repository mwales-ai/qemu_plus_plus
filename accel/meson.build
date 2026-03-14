common_ss.add(files('accel-common.cpp'))
specific_ss.add(files('accel-target.c'))
system_ss.add(files('accel-system.cpp', 'accel-blocker.cpp', 'accel-qmp.cpp', 'accel-irq.cpp'))
user_ss.add(files('accel-user.cpp'))

subdir('tcg')
if have_system
  subdir('hvf')
  subdir('qtest')
  subdir('kvm')
  subdir('xen')
  subdir('stubs')
  subdir('mshv')
endif

# qtest
system_ss.add(files('dummy-cpus.cpp'))
