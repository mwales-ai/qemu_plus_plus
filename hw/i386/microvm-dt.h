#ifndef HW_I386_MICROVM_DT_H
#define HW_I386_MICROVM_DT_H

#include "hw/i386/microvm.h"

#ifdef __cplusplus
extern "C" {
#endif

void dt_setup_microvm(MicrovmMachineState *mms);

#ifdef __cplusplus
}
#endif

#endif
