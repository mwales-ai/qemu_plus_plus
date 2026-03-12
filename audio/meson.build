system_ss.add(files(
  'audio.cpp',
  'mixeng.cpp',
  'noaudio.cpp',
  'wavaudio.cpp',
))

# deprecated since v10.2, to be removed
system_ss.add(files('audio-hmp-cmds.cpp', 'wavcapture.cpp'))

system_ss.add(when: coreaudio, if_true: files('coreaudio.m'))
system_ss.add(when: dsound, if_true: files('dsoundaudio.cpp', 'audio_win_int.cpp'))

audio_modules = {}
foreach m : [
  ['alsa', alsa, files('alsaaudio.c')],
  ['oss', oss, files('ossaudio.c')],
  ['pa', pulse, files('paaudio.cpp')],
  ['sdl', sdl, files('sdlaudio.cpp')],
  ['jack', jack, files('jackaudio.cpp')],
  ['sndio', sndio, files('sndioaudio.c')],
  ['pipewire', pipewire, files('pwaudio.c')],
  ['spice', spice, files('spiceaudio.cpp')]
]
  if m[1].found()
    module_ss = ss.source_set()
    module_ss.add(m[1], m[2])
    audio_modules += {m[0] : module_ss}
  endif
endforeach

if dbus_display
    module_ss = ss.source_set()
    module_ss.add(when: [gio, pixman],
                  if_true: [dbus_display1, files('dbusaudio.cpp')])
    audio_modules += {'dbus': module_ss}
endif

modules += {'audio': audio_modules}
