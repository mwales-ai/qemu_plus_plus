pci_ss = ss.source_set()
pci_ss.add(when: 'CONFIG_PCI_BRIDGE', if_true: files('pci_bridge_dev.c'))
pci_ss.add(when: 'CONFIG_I82801B11', if_true: files('i82801b11.cpp'))
pci_ss.add(when: 'CONFIG_IOH3420', if_true: files('ioh3420.cpp'))
pci_ss.add(when: 'CONFIG_PCIE_PORT', if_true: files('pcie_root_port.cpp', 'gen_pcie_root_port.c'))
pci_ss.add(when: 'CONFIG_PCIE_PCI_BRIDGE', if_true: files('pcie_pci_bridge.c'))
pci_ss.add(when: 'CONFIG_PXB', if_true: files('pci_expander_bridge.c'),
                               if_false: files('pci_expander_bridge_stubs.cpp'))
pci_ss.add(when: 'CONFIG_XIO3130', if_true: files('xio3130_upstream.c', 'xio3130_downstream.c'))
pci_ss.add(when: 'CONFIG_CXL', if_true: files('cxl_root_port.c', 'cxl_upstream.cpp', 'cxl_downstream.cpp'))

# Sun4u
pci_ss.add(when: 'CONFIG_SIMBA', if_true: files('simba.cpp'))

system_ss.add_all(when: 'CONFIG_PCI', if_true: pci_ss)
