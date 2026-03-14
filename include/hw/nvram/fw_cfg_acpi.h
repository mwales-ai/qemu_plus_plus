/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ACPI support for fw_cfg
 *
 */

#ifndef FW_CFG_ACPI_H
#define FW_CFG_ACPI_H

#include "exec/hwaddr.h"

#ifdef __cplusplus
extern "C" {
#endif

void fw_cfg_acpi_dsdt_add(Aml *scope, const MemMapEntry *fw_cfg_memmap);

#ifdef __cplusplus
}
#endif

#endif
