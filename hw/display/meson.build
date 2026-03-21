hw_display_modules = {}

system_ss.add(when: 'CONFIG_DDC', if_true: files('i2c-ddc.cpp'))
system_ss.add(when: 'CONFIG_EDID', if_true: files('edid-generate.cpp', 'edid-region.cpp'))

system_ss.add(when: 'CONFIG_FW_CFG_DMA', if_true: files('ramfb.cpp'), if_false: files('ramfb-stubs.cpp'))
system_ss.add(when: 'CONFIG_FW_CFG_DMA', if_true: files('ramfb-standalone.cpp'))

system_ss.add(when: 'CONFIG_VGA_CIRRUS', if_true: files('cirrus_vga.cpp'))
system_ss.add(when: ['CONFIG_VGA_CIRRUS', 'CONFIG_VGA_ISA'], if_true: files('cirrus_vga_isa.cpp'))
system_ss.add(when: 'CONFIG_G364FB', if_true: files('g364fb.cpp'))
system_ss.add(when: 'CONFIG_JAZZ_LED', if_true: files('jazz_led.cpp'))
system_ss.add(when: 'CONFIG_PL110', if_true: files('pl110.cpp'))
system_ss.add(when: 'CONFIG_SII9022', if_true: files('sii9022.cpp'))
system_ss.add(when: 'CONFIG_SSD0303', if_true: files('ssd0303.cpp'))
system_ss.add(when: 'CONFIG_SSD0323', if_true: files('ssd0323.cpp'))
system_ss.add(when: 'CONFIG_XEN_BUS', if_true: files('xenfb.cpp'))

system_ss.add(when: 'CONFIG_VGA_PCI', if_true: files('vga-pci.cpp'))
system_ss.add(when: 'CONFIG_VGA_ISA', if_true: files('vga-isa.cpp'))
system_ss.add(when: 'CONFIG_VGA_MMIO', if_true: files('vga-mmio.cpp'))
system_ss.add(when: 'CONFIG_VMWARE_VGA', if_true: files('vmware_vga.cpp'))
system_ss.add(when: 'CONFIG_BOCHS_DISPLAY', if_true: files('bochs-display.cpp'))

system_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4210_fimd.cpp'))
system_ss.add(when: 'CONFIG_FRAMEBUFFER', if_true: files('framebuffer.cpp'))

system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_fb.cpp'))
system_ss.add(when: 'CONFIG_SM501', if_true: files('sm501.cpp'))
system_ss.add(when: 'CONFIG_TCX', if_true: files('tcx.cpp'))
system_ss.add(when: 'CONFIG_CG3', if_true: files('cg3.cpp'))
system_ss.add(when: 'CONFIG_MACFB', if_true: files('macfb.cpp'))
system_ss.add(when: 'CONFIG_NEXTCUBE', if_true: files('next-fb.cpp'))

system_ss.add(when: 'CONFIG_VGA', if_true: files('vga.cpp'))
system_ss.add(when: 'CONFIG_VIRTIO', if_true: files('virtio-dmabuf.cpp'))
system_ss.add(when: 'CONFIG_DM163', if_true: files('dm163.cpp'))

if (config_all_devices.has_key('CONFIG_VGA_CIRRUS') or
    config_all_devices.has_key('CONFIG_VGA_PCI') or
    config_all_devices.has_key('CONFIG_VMWARE_VGA') or
    config_all_devices.has_key('CONFIG_ATI_VGA')
   )
  system_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.cpp'),
                                      if_false: files('acpi-vga-stub.cpp'))
endif

if config_all_devices.has_key('CONFIG_QXL')
  qxl_ss = ss.source_set()
  qxl_ss.add(when: 'CONFIG_QXL', if_true: [files('qxl.cpp', 'qxl-logger.cpp', 'qxl-render.cpp'),
                                           pixman, spice])
  qxl_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.cpp'),
                                  if_false: files('acpi-vga-stub.cpp'))
  hw_display_modules += {'qxl': qxl_ss}
endif

system_ss.add(when: 'CONFIG_DPCD', if_true: files('dpcd.cpp'))
system_ss.add(when: 'CONFIG_XLNX_DISPLAYPORT', if_true: files('xlnx_dp.c'))

system_ss.add(when: 'CONFIG_ARTIST', if_true: files('artist.cpp'))

system_ss.add(when: 'CONFIG_ATI_VGA', if_true: [files('ati.cpp', 'ati_2d.cpp', 'ati_dbg.cpp'), pixman])

system_ss.add(when: [pvg, 'CONFIG_MAC_PVG_PCI'],     if_true: [files('apple-gfx.m', 'apple-gfx-pci.m')])
system_ss.add(when: [pvg, 'CONFIG_MAC_PVG_MMIO'],    if_true: [files('apple-gfx.m', 'apple-gfx-mmio.m')])

if config_all_devices.has_key('CONFIG_VIRTIO_GPU')
  virtio_gpu_ss = ss.source_set()
  virtio_gpu_ss.add(when: 'CONFIG_VIRTIO_GPU',
                    if_true: [files('virtio-gpu-base.cpp', 'virtio-gpu.cpp'), pixman])
  if host_os == 'linux'
    virtio_gpu_ss.add(files('virtio-gpu-udmabuf.cpp'))
  else
    virtio_gpu_ss.add(files('virtio-gpu-udmabuf-stubs.cpp'))
  endif
  virtio_gpu_ss.add(when: 'CONFIG_VHOST_USER_GPU', if_true: files('vhost-user-gpu.cpp'))
  hw_display_modules += {'virtio-gpu': virtio_gpu_ss}

  if virgl.found() and opengl.found()
    virtio_gpu_gl_ss = ss.source_set()
    virtio_gpu_gl_ss.add(when: ['CONFIG_VIRTIO_GPU', virgl, opengl],
                         if_true: [files('virtio-gpu-gl.cpp', 'virtio-gpu-virgl.c'), pixman, virgl])
    hw_display_modules += {'virtio-gpu-gl': virtio_gpu_gl_ss}
  endif

  if rutabaga.found()
    virtio_gpu_rutabaga_ss = ss.source_set()
    virtio_gpu_rutabaga_ss.add(when: ['CONFIG_VIRTIO_GPU', rutabaga],
                               if_true: [files('virtio-gpu-rutabaga.c'), pixman])
    hw_display_modules += {'virtio-gpu-rutabaga': virtio_gpu_rutabaga_ss}
  endif
endif

if config_all_devices.has_key('CONFIG_VIRTIO_PCI')
  virtio_gpu_pci_ss = ss.source_set()
  virtio_gpu_pci_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI'],
                        if_true: [files('virtio-gpu-pci.cpp'), pixman])
  virtio_gpu_pci_ss.add(when: ['CONFIG_VHOST_USER_GPU', 'CONFIG_VIRTIO_PCI'],
                        if_true: files('vhost-user-gpu-pci.cpp'))
  hw_display_modules += {'virtio-gpu-pci': virtio_gpu_pci_ss}

  if virgl.found() and opengl.found()
    virtio_gpu_pci_gl_ss = ss.source_set()
    virtio_gpu_pci_gl_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI', virgl, opengl],
                             if_true: [files('virtio-gpu-pci-gl.cpp'), pixman])
    hw_display_modules += {'virtio-gpu-pci-gl': virtio_gpu_pci_gl_ss}
  endif
  if rutabaga.found()
    virtio_gpu_pci_rutabaga_ss = ss.source_set()
    virtio_gpu_pci_rutabaga_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI', rutabaga],
                                   if_true: [files('virtio-gpu-pci-rutabaga.cpp'), pixman])
    hw_display_modules += {'virtio-gpu-pci-rutabaga': virtio_gpu_pci_rutabaga_ss}
  endif
endif

if config_all_devices.has_key('CONFIG_VIRTIO_VGA')
  virtio_vga_ss = ss.source_set()
  virtio_vga_ss.add(when: 'CONFIG_VIRTIO_VGA',
                    if_true: [files('virtio-vga.cpp'), pixman])
  virtio_vga_ss.add(when: 'CONFIG_VHOST_USER_VGA',
                    if_true: files('vhost-user-vga.cpp'))
  virtio_vga_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.cpp'),
                                         if_false: files('acpi-vga-stub.cpp'))
  hw_display_modules += {'virtio-vga': virtio_vga_ss}

  if virgl.found() and opengl.found()
    virtio_vga_gl_ss = ss.source_set()
    virtio_vga_gl_ss.add(when: ['CONFIG_VIRTIO_VGA', virgl, opengl],
                         if_true: [files('virtio-vga-gl.cpp'), pixman])
    virtio_vga_gl_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.cpp'),
                                              if_false: files('acpi-vga-stub.cpp'))
    hw_display_modules += {'virtio-vga-gl': virtio_vga_gl_ss}
  endif

  if rutabaga.found()
    virtio_vga_rutabaga_ss = ss.source_set()
    virtio_vga_rutabaga_ss.add(when: ['CONFIG_VIRTIO_VGA', rutabaga],
                               if_true: [files('virtio-vga-rutabaga.cpp'), pixman])
    virtio_vga_rutabaga_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.cpp'),
                                                    if_false: files('acpi-vga-stub.cpp'))
    hw_display_modules += {'virtio-vga-rutabaga': virtio_vga_rutabaga_ss}
  endif
endif

system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_lcdc.cpp'))

modules += { 'hw-display': hw_display_modules }
