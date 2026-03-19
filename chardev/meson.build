chardev_ss.add(files(
  'char-fe.cpp',
  'char-file.cpp',
  'char-io.cpp',
  'char-mux.cpp',
  'char-hub.cpp',
  'char-null.cpp',
  'char-pipe.c',
  'char-ringbuf.cpp',
  'char-serial.cpp',
  'char-socket.c',
  'char-stdio.cpp',
  'char-udp.cpp',
  'char.c',
))
if host_os == 'windows'
  chardev_ss.add(files(
    'char-console.cpp',
    'char-win-stdio.c',
    'char-win.c',
  ))
else
  chardev_ss.add(files(
      'char-fd.cpp',
      'char-parallel.cpp',
      'char-pty.cpp',
    ), util)
endif

chardev_ss = chardev_ss.apply({})

system_ss.add(files(
    'char-hmp-cmds.c',
    'msmouse.cpp',
    'wctablet.cpp',
    'testdev.cpp'))

chardev_modules = {}

if brlapi.found()
  module_ss = ss.source_set()
  module_ss.add(when: [brlapi], if_true: [files('baum.cpp'), pixman])
  chardev_modules += { 'baum': module_ss }
endif

if spice.found()
  module_ss = ss.source_set()
  module_ss.add(when: [spice], if_true: files('spice.cpp'))
  chardev_modules += { 'spice': module_ss }
endif

modules += { 'chardev': chardev_modules }
