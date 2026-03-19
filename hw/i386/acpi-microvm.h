#ifndef HW_I386_ACPI_MICROVM_H
#define HW_I386_ACPI_MICROVM_H

#include "hw/i386/microvm.h"

#ifdef __cplusplus
extern "C" {
#endif

void acpi_setup_microvm(MicrovmMachineState *mms);

#ifdef __cplusplus
}
#endif

#endif
