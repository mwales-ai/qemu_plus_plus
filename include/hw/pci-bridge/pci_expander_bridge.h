/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PCI_EXPANDER_BRIDGE_H
#define PCI_EXPANDER_BRIDGE_H

#include "hw/cxl/cxl.h"

#ifdef __cplusplus
extern "C" {
#endif

void pxb_cxl_hook_up_registers(CXLState *state, PCIBus *bus, Error **errp);

#ifdef __cplusplus
}
#endif

#endif /* PCI_EXPANDER_BRIDGE_H */
