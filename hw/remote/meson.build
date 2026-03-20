remote_ss = ss.source_set()

remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('machine.cpp'))
remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('mpqemu-link.cpp'))
remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('message.cpp'))
remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('remote-obj.cpp'))
remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('proxy.cpp'))
remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('iohub.cpp'))
remote_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('iommu.cpp'))

remote_ss.add(when: 'CONFIG_VFIO_USER_SERVER', if_true: libvfio_user_dep)
remote_ss.add(when: 'CONFIG_VFIO_USER_SERVER', if_true: files('vfio-user-obj.c'),
              if_false: files('vfio-user-obj-stub.cpp'))

specific_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('memory.cpp'))
specific_ss.add(when: 'CONFIG_MULTIPROCESS', if_true: files('proxy-memory-listener.cpp'))

system_ss.add_all(when: 'CONFIG_MULTIPROCESS', if_true: remote_ss)
