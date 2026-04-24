#ifndef CHIPIDEA_H
#define CHIPIDEA_H

#include "hw/usb/hcd-ehci.h"
#include "qom/object.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ChipideaState {
    /*< private >*/
    EHCISysBusState parent_obj;

    MemoryRegion iomem[3];

#ifdef __cplusplus
    void init();
    static void classInit(DeviceClass *dc);
#endif
};

#define TYPE_CHIPIDEA "usb-chipidea"
OBJECT_DECLARE_SIMPLE_TYPE(ChipideaState, CHIPIDEA)

#ifdef __cplusplus
}
#endif

#endif /* CHIPIDEA_H */
