
#ifndef HW_I386_ACPI_BUILD_H
#define HW_I386_ACPI_BUILD_H
#include "hw/acpi/acpi-defs.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const struct AcpiGenericAddress x86_nvdimm_acpi_dsmio;

void acpi_setup(void);
Object *acpi_get_i386_pci_host(void);

#ifdef __cplusplus
}
#endif

#endif
