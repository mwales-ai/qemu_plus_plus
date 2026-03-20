system_ss.add(files('hid.cpp'))
system_ss.add(when: 'CONFIG_ADB', if_true: files('adb.cpp', 'adb-mouse.cpp', 'adb-kbd.cpp'))
system_ss.add(when: 'CONFIG_PCKBD', if_true: files('pckbd.c'))
system_ss.add(when: 'CONFIG_PL050', if_true: files('pl050.cpp'))
system_ss.add(when: 'CONFIG_PS2', if_true: files('ps2.c'))
system_ss.add(when: 'CONFIG_STELLARIS_GAMEPAD', if_true: files('stellaris_gamepad.cpp'))

system_ss.add(when: 'CONFIG_VIRTIO_INPUT', if_true: files('virtio-input.cpp'))
system_ss.add(when: 'CONFIG_VIRTIO_INPUT', if_true: files('virtio-input-hid.cpp'))
system_ss.add(when: 'CONFIG_VIRTIO_INPUT_HOST', if_true: files('virtio-input-host.cpp'))

system_ss.add(when: 'CONFIG_LASIPS2', if_true: files('lasips2.cpp'))
