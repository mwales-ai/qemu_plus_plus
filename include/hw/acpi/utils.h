#ifndef HW_ACPI_UTILS_H
#define HW_ACPI_UTILS_H

#include "hw/nvram/fw_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

MemoryRegion *acpi_add_rom_blob(FWCfgCallback update, void *opaque,
                                GArray *blob, const char *name);
#ifdef __cplusplus
}
#endif

#endif
