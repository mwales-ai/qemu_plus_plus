blockdev_ss.add(files('export.cpp'))

if have_vhost_user_blk_server
    blockdev_ss.add(files('vhost-user-blk-server.cpp', 'virtio-blk-handler.cpp'))
endif

blockdev_ss.add(when: fuse, if_true: files('fuse.c'))

if have_vduse_blk_export
    blockdev_ss.add(files('vduse-blk.cpp', 'virtio-blk-handler.cpp'))
    blockdev_ss.add(libvduse)
endif
