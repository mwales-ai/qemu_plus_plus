system_ss.add(when: 'CONFIG_TCG', if_true: files(
  'replay.cpp',
  'replay-internal.cpp',
  'replay-events.cpp',
  'replay-time.cpp',
  'replay-input.cpp',
  'replay-char.cpp',
  'replay-snapshot.cpp',
  'replay-net.cpp',
  'replay-audio.cpp',
  'replay-random.cpp',
  'replay-debugging.cpp',
), if_false: files('stubs-system.cpp'))
