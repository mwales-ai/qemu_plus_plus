#ifndef HW_PARALLEL_H
#define HW_PARALLEL_H

#include "system/memory.h"
#include "hw/isa/isa.h"
#include "hw/irq.h"
#include "chardev/char-fe.h"
#include "chardev/char.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ParallelState {
    MemoryRegion iomem;
    uint8_t dataw;
    uint8_t datar;
    uint8_t status;
    uint8_t control;
    qemu_irq irq;
    int irq_pending;
    CharFrontend chr;
    int hw_driver;
    int epp_timeout;
    uint32_t last_read_offset; /* For debugging */
    /* Memory-mapped interface */
    int it_shift;

#ifdef __cplusplus
    /* methods */
    void updateIrq();
    void ioportWriteSw(uint32_t addr, uint32_t val);
    void ioportWriteHw(uint32_t addr, uint32_t val);
    void ioportEppdataWriteHw2(uint32_t addr, uint32_t val);
    void ioportEppdataWriteHw4(uint32_t addr, uint32_t val);
    uint32_t ioportReadSw(uint32_t addr);
    uint32_t ioportReadHw(uint32_t addr);
    uint32_t ioportEppdataReadHw2(uint32_t addr);
    uint32_t ioportEppdataReadHw4(uint32_t addr);
    void reset();
#endif
} ParallelState;

void parallel_hds_isa_init(ISABus *bus, int n);

bool parallel_mm_init(MemoryRegion *address_space,
                      hwaddr base, int it_shift, qemu_irq irq,
                      Chardev *chr);

#ifdef __cplusplus
}
#endif

#endif
