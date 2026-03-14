system_ss.add([files('dump.cpp', 'dump-hmp-cmds.cpp'), snappy, lzo])
specific_ss.add(when: 'CONFIG_SYSTEM_ONLY', if_true: files('win_dump.c'))
