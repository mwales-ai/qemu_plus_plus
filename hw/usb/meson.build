hw_usb_modules = {}

# usb subsystem core
system_ss.add(when: 'CONFIG_USB', if_true: files(
  'bus.c',
  'combined-packet.cpp',
  'core.c',
  'desc.c',
  'desc-msos.cpp',
  'libhw.cpp',
  'pcap.cpp',
), if_false: files('bus-stub.cpp'))

# usb host adapters
system_ss.add(when: 'CONFIG_USB_UHCI', if_true: files('hcd-uhci.cpp'))
system_ss.add(when: 'CONFIG_USB_OHCI', if_true: files('hcd-ohci.cpp'))
system_ss.add(when: 'CONFIG_USB_OHCI_PCI', if_true: files('hcd-ohci-pci.cpp'))
system_ss.add(when: 'CONFIG_USB_OHCI_SYSBUS', if_true: files('hcd-ohci-sysbus.cpp'))
system_ss.add(when: 'CONFIG_USB_EHCI', if_true: files('hcd-ehci.cpp'))
system_ss.add(when: 'CONFIG_USB_EHCI_PCI', if_true: files('hcd-ehci-pci.cpp'))
system_ss.add(when: 'CONFIG_USB_EHCI_SYSBUS', if_true: files('hcd-ehci-sysbus.cpp'))
system_ss.add(when: 'CONFIG_USB_XHCI', if_true: files('hcd-xhci.cpp'))
system_ss.add(when: 'CONFIG_USB_XHCI_PCI', if_true: files('hcd-xhci-pci.cpp'))
system_ss.add(when: 'CONFIG_USB_XHCI_SYSBUS', if_true: files('hcd-xhci-sysbus.cpp'))
system_ss.add(when: 'CONFIG_USB_XHCI_NEC', if_true: files('hcd-xhci-nec.cpp'))
system_ss.add(when: 'CONFIG_USB_DWC2', if_true: files('hcd-dwc2.c'))
system_ss.add(when: 'CONFIG_USB_DWC3', if_true: files('hcd-dwc3.c'))
system_ss.add(when: 'CONFIG_USB_CHIPIDEA', if_true: files('chipidea.cpp'))

system_ss.add(when: 'CONFIG_IMX_USBPHY', if_true: files('imx-usb-phy.cpp'))
system_ss.add(when: 'CONFIG_VT82C686', if_true: files('vt82c686-uhci-pci.cpp'))
system_ss.add(when: 'CONFIG_XLNX_VERSAL', if_true: files('xlnx-versal-usb2-ctrl-regs.c'))
system_ss.add(when: 'CONFIG_XLNX_USB_SUBSYS', if_true: files('xlnx-usb-subsystem.cpp'))

# emulated usb devices
system_ss.add(when: 'CONFIG_USB_HUB', if_true: files('dev-hub.cpp'))
system_ss.add(when: 'CONFIG_USB_HID', if_true: files('dev-hid.cpp'))
system_ss.add(when: 'CONFIG_USB_TABLET_WACOM', if_true: files('dev-wacom.c'))
system_ss.add(when: 'CONFIG_USB_STORAGE_CORE', if_true: files('dev-storage.c'))
system_ss.add(when: 'CONFIG_USB_STORAGE_BOT', if_true: files('dev-storage-bot.cpp'))
system_ss.add(when: 'CONFIG_USB_STORAGE_CLASSIC', if_true: files('dev-storage-classic.cpp'))
system_ss.add(when: 'CONFIG_USB_STORAGE_UAS', if_true: files('dev-uas.cpp'))
system_ss.add(when: 'CONFIG_USB_AUDIO', if_true: files('dev-audio.cpp'))
system_ss.add(when: 'CONFIG_USB_SERIAL', if_true: files('dev-serial.cpp'))
system_ss.add(when: 'CONFIG_USB_NETWORK', if_true: files('dev-network.cpp'))
if host_os != 'windows'
  system_ss.add(when: 'CONFIG_USB_STORAGE_MTP', if_true: files('dev-mtp.cpp'))
endif

# smartcard
system_ss.add(when: 'CONFIG_USB_SMARTCARD', if_true: files('dev-smartcard-reader.cpp'))

if cacard.found()
  usbsmartcard_ss = ss.source_set()
  usbsmartcard_ss.add(when: 'CONFIG_USB_SMARTCARD',
                      if_true: [cacard, files('ccid-card-emulated.c', 'ccid-card-passthru.c')])
  hw_usb_modules += {'smartcard': usbsmartcard_ss}
endif

# U2F
system_ss.add(when: 'CONFIG_USB_U2F', if_true: files('u2f.c'))
if host_os == 'linux'
  system_ss.add(when: 'CONFIG_USB_U2F', if_true: [libudev, files('u2f-passthru.c')])
endif
if u2f.found()
  system_ss.add(when: 'CONFIG_USB_U2F', if_true: [u2f, files('u2f-emulated.c')])
endif

# CanoKey
if canokey.found()
  system_ss.add(when: 'CONFIG_USB_CANOKEY', if_true: [canokey, files('canokey.cpp')])
endif

# usb redirect
if usbredir.found()
  usbredir_ss = ss.source_set()
  usbredir_ss.add(when: 'CONFIG_USB',
                  if_true: [usbredir, files('redirect.cpp', 'quirks.cpp')])
  hw_usb_modules += {'redirect': usbredir_ss}
endif

# usb pass-through
if libusb.found()
  usbhost_ss = ss.source_set()
  usbhost_ss.add(when: ['CONFIG_USB', libusb],
                 if_true: files('host-libusb.c'))
  hw_usb_modules += {'host': usbhost_ss}
endif

system_ss.add(when: ['CONFIG_USB', 'CONFIG_XEN_BUS', libusb], if_true: files('xen-usb.c'))

modules += { 'hw-usb': hw_usb_modules }
