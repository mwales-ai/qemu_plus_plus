common_ss.add(when: 'CONFIG_SEMIHOSTING', if_false: files('stubs-all.cpp'))
user_ss.add(when: 'CONFIG_SEMIHOSTING', if_true: files(
  'user.cpp',
  'guestfd.cpp'))
system_ss.add(when: 'CONFIG_SEMIHOSTING', if_true: files(
  'config.cpp',
  'console.cpp',
  'guestfd.cpp',
  'uaccess.cpp',
  'syscalls.cpp',
), if_false: files(
  'stubs-system.cpp',
))
system_ss.add(when: 'CONFIG_ARM_COMPATIBLE_SEMIHOSTING',
  if_true: files('arm-compat-semi.cpp'),
  if_false: files('arm-compat-semi-stub.cpp'))

specific_ss.add(when: ['CONFIG_SEMIHOSTING', 'CONFIG_USER_ONLY'],
                if_true: files('syscalls.cpp'))
specific_ss.add(when: ['CONFIG_ARM_COMPATIBLE_SEMIHOSTING', 'CONFIG_USER_ONLY'],
		if_true: files('arm-compat-semi.cpp'))
