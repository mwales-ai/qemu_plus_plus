nubus_ss = ss.source_set()
nubus_ss.add(files('nubus-device.cpp'))
nubus_ss.add(files('nubus-bus.cpp'))
nubus_ss.add(files('nubus-bridge.cpp'))
nubus_ss.add(files('nubus-virtio-mmio.cpp'))
nubus_ss.add(when: 'CONFIG_Q800', if_true: files('mac-nubus-bridge.cpp'))

system_ss.add_all(when: 'CONFIG_NUBUS', if_true: nubus_ss)
