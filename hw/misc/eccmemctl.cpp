/*
 * QEMU Sparc Sun4m ECC memory controller emulation
 *
 * Copyright (c) 2007 Robert Reif
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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"
#include "qom/object.h"

/* There are 3 versions of this chip used in SMP sun4m systems:
 * MCC (version 0, implementation 0) SS-600MP
 * EMC (version 0, implementation 1) SS-10
 * SMC (version 0, implementation 2) SS-10SX and SS-20
 *
 * Chipset docs:
 * "Sun-4M System Architecture (revision 2.0) by Chuck Narad", 950-1373-01,
 * http://mediacast.sun.com/users/Barton808/media/Sun4M_SystemArchitecture_edited2.pdf
 */

#define ECC_MCC        0x00000000
#define ECC_EMC        0x10000000
#define ECC_SMC        0x20000000

/* Register indexes */
#define ECC_MER        0               /* Memory Enable Register */
#define ECC_MDR        1               /* Memory Delay Register */
#define ECC_MFSR       2               /* Memory Fault Status Register */
#define ECC_VCR        3               /* Video Configuration Register */
#define ECC_MFAR0      4               /* Memory Fault Address Register 0 */
#define ECC_MFAR1      5               /* Memory Fault Address Register 1 */
#define ECC_DR         6               /* Diagnostic Register */
#define ECC_ECR0       7               /* Event Count Register 0 */
#define ECC_ECR1       8               /* Event Count Register 1 */

/* ECC fault control register */
#define ECC_MER_EE     0x00000001      /* Enable ECC checking */
#define ECC_MER_EI     0x00000002      /* Enable Interrupts on
                                          correctable errors */
#define ECC_MER_MRR0   0x00000004      /* SIMM 0 */
#define ECC_MER_MRR1   0x00000008      /* SIMM 1 */
#define ECC_MER_MRR2   0x00000010      /* SIMM 2 */
#define ECC_MER_MRR3   0x00000020      /* SIMM 3 */
#define ECC_MER_MRR4   0x00000040      /* SIMM 4 */
#define ECC_MER_MRR5   0x00000080      /* SIMM 5 */
#define ECC_MER_MRR6   0x00000100      /* SIMM 6 */
#define ECC_MER_MRR7   0x00000200      /* SIMM 7 */
#define ECC_MER_REU    0x00000100      /* Memory Refresh Enable (600MP) */
#define ECC_MER_MRR    0x000003fc      /* MRR mask */
#define ECC_MER_A      0x00000400      /* Memory controller addr map select */
#define ECC_MER_DCI    0x00000800      /* Disables Coherent Invalidate ACK */
#define ECC_MER_VER    0x0f000000      /* Version */
#define ECC_MER_IMPL   0xf0000000      /* Implementation */
#define ECC_MER_MASK_0 0x00000103      /* Version 0 (MCC) mask */
#define ECC_MER_MASK_1 0x00000bff      /* Version 1 (EMC) mask */
#define ECC_MER_MASK_2 0x00000bff      /* Version 2 (SMC) mask */

/* ECC memory delay register */
#define ECC_MDR_RRI    0x000003ff      /* Refresh Request Interval */
#define ECC_MDR_MI     0x00001c00      /* MIH Delay */
#define ECC_MDR_CI     0x0000e000      /* Coherent Invalidate Delay */
#define ECC_MDR_MDL    0x001f0000      /* MBus Master arbitration delay */
#define ECC_MDR_MDH    0x03e00000      /* MBus Master arbitration delay */
#define ECC_MDR_GAD    0x7c000000      /* Graphics Arbitration Delay */
#define ECC_MDR_RSC    0x80000000      /* Refresh load control */
#define ECC_MDR_MASK   0x7fffffff

/* ECC fault status register */
#define ECC_MFSR_CE    0x00000001      /* Correctable error */
#define ECC_MFSR_BS    0x00000002      /* C2 graphics bad slot access */
#define ECC_MFSR_TO    0x00000004      /* Timeout on write */
#define ECC_MFSR_UE    0x00000008      /* Uncorrectable error */
#define ECC_MFSR_DW    0x000000f0      /* Index of double word in block */
#define ECC_MFSR_SYND  0x0000ff00      /* Syndrome for correctable error */
#define ECC_MFSR_ME    0x00010000      /* Multiple errors */
#define ECC_MFSR_C2ERR 0x00020000      /* C2 graphics error */

/* ECC fault address register 0 */
#define ECC_MFAR0_PADDR 0x0000000f     /* PA[32-35] */
#define ECC_MFAR0_TYPE  0x000000f0     /* Transaction type */
#define ECC_MFAR0_SIZE  0x00000700     /* Transaction size */
#define ECC_MFAR0_CACHE 0x00000800     /* Mapped cacheable */
#define ECC_MFAR0_LOCK  0x00001000     /* Error occurred in atomic cycle */
#define ECC_MFAR0_BMODE 0x00002000     /* Boot mode */
#define ECC_MFAR0_VADDR 0x003fc000     /* VA[12-19] (superset bits) */
#define ECC_MFAR0_S     0x08000000     /* Supervisor mode */
#define ECC_MFARO_MID   0xf0000000     /* Module ID */

/* ECC diagnostic register */
#define ECC_DR_CBX     0x00000001
#define ECC_DR_CB0     0x00000002
#define ECC_DR_CB1     0x00000004
#define ECC_DR_CB2     0x00000008
#define ECC_DR_CB4     0x00000010
#define ECC_DR_CB8     0x00000020
#define ECC_DR_CB16    0x00000040
#define ECC_DR_CB32    0x00000080
#define ECC_DR_DMODE   0x00000c00

#define ECC_NREGS      9
#define ECC_SIZE       (ECC_NREGS * sizeof(uint32_t))

#define ECC_DIAG_SIZE  4
#define ECC_DIAG_MASK  (ECC_DIAG_SIZE - 1)

#define TYPE_ECC_MEMCTL "eccmemctl"
OBJECT_DECLARE_SIMPLE_TYPE(ECCState, ECC_MEMCTL)

struct ECCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem, iomem_diag;
    qemu_irq irq;
    uint32_t regs[ECC_NREGS];
    uint8_t diag[ECC_DIAG_SIZE];
    uint32_t version;

    /* Static MMIO callbacks */
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);
    static uint64_t diagRead(void *opaque, hwaddr addr, unsigned size);
    static void diagWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);

    /* Instance methods */
    uint64_t readReg(hwaddr addr, unsigned size);
    void writeReg(hwaddr addr, uint64_t val, unsigned size);
    uint64_t readDiag(hwaddr addr, unsigned size);
    void writeDiag(hwaddr addr, uint64_t val, unsigned size);
    void initfn();
    void realize();
    void reset();

    /* Class init */
    static void classInit(ObjectClass *klass, const void *data);
};

void ECCState::mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    ECCState *s = static_cast<ECCState *>(opaque);
    s->writeReg(addr, val, size);
}

void ECCState::writeReg(hwaddr addr, uint64_t val, unsigned size)
{
    switch (addr >> 2) {
    case ECC_MER:
        if (version == ECC_MCC)
            regs[ECC_MER] = (val & ECC_MER_MASK_0);
        else if (version == ECC_EMC)
            regs[ECC_MER] = version | (val & ECC_MER_MASK_1);
        else if (version == ECC_SMC)
            regs[ECC_MER] = version | (val & ECC_MER_MASK_2);
        trace_ecc_mem_writel_mer(val);
        break;
    case ECC_MDR:
        regs[ECC_MDR] =  val & ECC_MDR_MASK;
        trace_ecc_mem_writel_mdr(val);
        break;
    case ECC_MFSR:
        regs[ECC_MFSR] =  val;
        qemu_irq_lower(irq);
        trace_ecc_mem_writel_mfsr(val);
        break;
    case ECC_VCR:
        regs[ECC_VCR] =  val;
        trace_ecc_mem_writel_vcr(val);
        break;
    case ECC_DR:
        regs[ECC_DR] =  val;
        trace_ecc_mem_writel_dr(val);
        break;
    case ECC_ECR0:
        regs[ECC_ECR0] =  val;
        trace_ecc_mem_writel_ecr0(val);
        break;
    case ECC_ECR1:
        regs[ECC_ECR0] =  val;
        trace_ecc_mem_writel_ecr1(val);
        break;
    }
}

uint64_t ECCState::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    ECCState *s = static_cast<ECCState *>(opaque);
    return s->readReg(addr, size);
}

uint64_t ECCState::readReg(hwaddr addr, unsigned size)
{
    uint32_t ret = 0;

    switch (addr >> 2) {
    case ECC_MER:
        ret = regs[ECC_MER];
        trace_ecc_mem_readl_mer(ret);
        break;
    case ECC_MDR:
        ret = regs[ECC_MDR];
        trace_ecc_mem_readl_mdr(ret);
        break;
    case ECC_MFSR:
        ret = regs[ECC_MFSR];
        trace_ecc_mem_readl_mfsr(ret);
        break;
    case ECC_VCR:
        ret = regs[ECC_VCR];
        trace_ecc_mem_readl_vcr(ret);
        break;
    case ECC_MFAR0:
        ret = regs[ECC_MFAR0];
        trace_ecc_mem_readl_mfar0(ret);
        break;
    case ECC_MFAR1:
        ret = regs[ECC_MFAR1];
        trace_ecc_mem_readl_mfar1(ret);
        break;
    case ECC_DR:
        ret = regs[ECC_DR];
        trace_ecc_mem_readl_dr(ret);
        break;
    case ECC_ECR0:
        ret = regs[ECC_ECR0];
        trace_ecc_mem_readl_ecr0(ret);
        break;
    case ECC_ECR1:
        ret = regs[ECC_ECR0];
        trace_ecc_mem_readl_ecr1(ret);
        break;
    }
    return ret;
}

static const MemoryRegionOps ecc_mem_ops = {
    .read = ECCState::mmioRead,
    .write = ECCState::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void ECCState::diagWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    ECCState *s = static_cast<ECCState *>(opaque);
    s->writeDiag(addr, val, size);
}

void ECCState::writeDiag(hwaddr addr, uint64_t val, unsigned size)
{
    trace_ecc_diag_mem_writeb(addr, val);
    diag[addr & ECC_DIAG_MASK] = val;
}

uint64_t ECCState::diagRead(void *opaque, hwaddr addr, unsigned size)
{
    ECCState *s = static_cast<ECCState *>(opaque);
    return s->readDiag(addr, size);
}

uint64_t ECCState::readDiag(hwaddr addr, unsigned size)
{
    uint32_t ret = diag[(int)addr];

    trace_ecc_diag_mem_readb(addr, ret);
    return ret;
}

static const MemoryRegionOps ecc_diag_mem_ops = {
    .read = ECCState::diagRead,
    .write = ECCState::diagWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const VMStateDescription vmstate_ecc = {
    .name ="ECC",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ECCState, ECC_NREGS),
        VMSTATE_BUFFER(diag, ECCState),
        VMSTATE_UINT32(version, ECCState),
        VMSTATE_END_OF_LIST()
    }
};

void ECCState::reset()
{
    if (version == ECC_MCC) {
        regs[ECC_MER] &= ECC_MER_REU;
    } else {
        regs[ECC_MER] &= (ECC_MER_VER | ECC_MER_IMPL | ECC_MER_MRR |
                             ECC_MER_DCI);
    }
    regs[ECC_MDR] = 0x20;
    regs[ECC_MFSR] = 0;
    regs[ECC_VCR] = 0;
    regs[ECC_MFAR0] = 0x07c00000;
    regs[ECC_MFAR1] = 0;
    regs[ECC_DR] = 0;
    regs[ECC_ECR0] = 0;
    regs[ECC_ECR1] = 0;
}

static void ecc_reset(DeviceState *d)
{
    ECCState *s = reinterpret_cast<ECCState *>(d);
    s->reset();
}

void ECCState::initfn()
{
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(this);

    sysbus_init_irq(dev, &irq);

    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this), &ecc_mem_ops, this, "ecc",
                          ECC_SIZE);
    sysbus_init_mmio(dev, &iomem);
}

static void ecc_init(Object *obj)
{
    ECCState *s = reinterpret_cast<ECCState *>(obj);
    s->initfn();
}

void ECCState::realize()
{
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    regs[0] = version;

    if (version == ECC_MCC) { // SS-600MP only
        memory_region_init_io(&iomem_diag, reinterpret_cast<Object *>(this), &ecc_diag_mem_ops,
                              this, "ecc.diag", ECC_DIAG_SIZE);
        sysbus_init_mmio(sbd, &iomem_diag);
    }
}

static void ecc_realize(DeviceState *dev, Error **errp)
{
    ECCState *s = reinterpret_cast<ECCState *>(dev);
    s->realize();
}

static const Property ecc_properties[] = {
    DEFINE_PROP_UINT32("version", ECCState, version, -1),
};

void ECCState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ecc_realize;
    device_class_set_legacy_reset(dc, ecc_reset);
    dc->vmsd = &vmstate_ecc;
    device_class_set_props(dc, ecc_properties);
}

static const TypeInfo ecc_info = {
    .name          = TYPE_ECC_MEMCTL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ECCState),
    .instance_init = ecc_init,
    .class_init    = ECCState::classInit,
};


static void ecc_register_types(void)
{
    type_register_static(&ecc_info);
}

type_init(ecc_register_types)
