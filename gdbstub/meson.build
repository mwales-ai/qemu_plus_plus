#
# The main gdbstub still relies on per-build definitions of various
# types. The bits pushed to system/user.c try to use guest agnostic
# types such as hwaddr.
#

# We build two versions of gdbstub, one for each mode
user_ss.add(files(
  'gdbstub.cpp',
  'syscalls.cpp',
  'user.cpp'
))

system_ss.add(files(
  'gdbstub.cpp',
  'syscalls.cpp',
  'system.cpp'
))

# The user-target is specialised by the guest
specific_ss.add(when: 'CONFIG_USER_ONLY', if_true: files('user-target.cpp'))
