authz_ss.add(genh)
authz_ss.add(files(
  'base.cpp',
  'list.cpp',
  'listfile.cpp',
  'simple.cpp',
))

authz_ss.add(when: pam, if_true: files('pamacct.cpp'))
