#ifndef MV64361_H
#define MV64361_H

#define TYPE_MV64361 "mv64361"

#ifdef __cplusplus
extern "C" {
#endif

PCIBus *mv64361_get_pci_bus(DeviceState *dev, int n);

#ifdef __cplusplus
}
#endif

#endif
