/*
 * QEMU Crystal CS4231 audio chip emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"

extern "C" {
#include "qemu/module.h"
#include "trace.h"
}

/*
 * In addition to Crystal CS4231 there is a DMA controller on Sparc.
 */
#define CS_SIZE 0x40
#define CS_REGS 16
#define CS_DREGS 32
#define CS_MAXDREG (CS_DREGS - 1)

#define TYPE_CS4231 "sun-CS4231"
typedef struct CSState CSState;
DECLARE_INSTANCE_CHECKER(CSState, CS4231,
                         TYPE_CS4231)

#define CS_RAP(s) ((s)->regs[0] & CS_MAXDREG)
#define CS_VER 0xa0
#define CS_CDC_VER 0x8a

struct CSState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[CS_REGS];
    uint8_t dregs[CS_DREGS];

    void reset()
    {
        memset(regs, 0, CS_REGS * 4);
        memset(dregs, 0, CS_DREGS);
        dregs[12] = CS_CDC_VER;
        dregs[25] = CS_VER;
    }


    static uint64_t memRead(void *opaque, hwaddr addr, unsigned size)
    {
        CSState *s = static_cast<CSState *>(opaque);
        uint32_t saddr, ret;

        saddr = addr >> 2;
        switch (saddr) {
        case 1:
            switch (CS_RAP(s)) {
            case 3: // Write only
                ret = 0;
                break;
            default:
                ret = s->dregs[CS_RAP(s)];
                break;
            }
            trace_cs4231_mem_readl_dreg(CS_RAP(s), ret);
            break;
        default:
            ret = s->regs[saddr];
            trace_cs4231_mem_readl_reg(saddr, ret);
            break;
        }
        return ret;
    }

    static void memWrite(void *opaque, hwaddr addr,
                         uint64_t val, unsigned size)
    {
        CSState *s = static_cast<CSState *>(opaque);
        uint32_t saddr;

        saddr = addr >> 2;
        trace_cs4231_mem_writel_reg(saddr, s->regs[saddr], val);
        switch (saddr) {
        case 1:
            trace_cs4231_mem_writel_dreg(CS_RAP(s), s->dregs[CS_RAP(s)], val);
            switch(CS_RAP(s)) {
            case 11:
            case 25: // Read only
                break;
            case 12:
                val &= 0x40;
                val |= CS_CDC_VER; // Codec version
                s->dregs[CS_RAP(s)] = val;
                break;
            default:
                s->dregs[CS_RAP(s)] = val;
                break;
            }
            break;
        case 2: // Read only
            break;
        case 4:
            if (val & 1) {
                s->reset();
            }
            val &= 0x7f;
            s->regs[saddr] = val;
            break;
        default:
            s->regs[saddr] = val;
            break;
        }
    }

    static const MemoryRegionOps memOps;

    void init()
    {
        memory_region_init_io(&iomem, OBJECT(this), &memOps, this, "cs4321",
                              CS_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(this), &iomem);
        sysbus_init_irq(SYS_BUS_DEVICE(this), &irq);
    }

    static void classInit(DeviceClass *dc)
    {
        dc->vmsd = &vmstate_cs4231;
    }

    static const VMStateDescription vmstate_cs4231;
};

const MemoryRegionOps CSState::memOps = {
    .read = CSState::memRead,
    .write = CSState::memWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateField vmstate_cs4231_fields[] = {
    VMSTATE_UINT32_ARRAY(regs, CSState, CS_REGS),
    VMSTATE_UINT8_ARRAY(dregs, CSState, CS_DREGS),
    VMSTATE_END_OF_LIST()
};

const VMStateDescription CSState::vmstate_cs4231 = {
    .name ="cs4231",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_cs4231_fields,
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(CSState, TYPE_CS4231, TYPE_SYS_BUS_DEVICE)
