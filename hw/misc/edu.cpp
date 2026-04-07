/*
 * QEMU educational PCI device
 *
 * Copyright (c) 2012-2015 Jiri Slaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qapi/visitor.h"

/* ========================================================================
 * EDU Device — converted to C++ methods while keeping QOM compatibility
 *
 * Changes from original C version:
 *   - Free functions become struct methods (edu_raise_irq -> EduState::raiseIrq)
 *   - Static callbacks delegate to methods via static_cast<EduState*>(opaque)
 *   - QOM struct layout is IDENTICAL — parent_obj first, same field order
 *   - VMState, Properties, TypeInfo all unchanged
 * ======================================================================== */

#define TYPE_PCI_EDU_DEVICE "edu"

#define FACT_IRQ        0x00000001
#define DMA_IRQ         0x00000100

#define DMA_START       0x40000
#define DMA_SIZE        4096

struct EduState {
    /* QOM parent — MUST be first */
    PCIDevice pdev;

    /* MMIO region */
    MemoryRegion mmio;

    /* Factorial computation thread */
    QemuThread thread;
    QemuMutex thr_mutex;
    QemuCond thr_cond;
    bool stopping;

    /* Registers */
    uint32_t addr4;
    uint32_t fact;
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80
    uint32_t status;

    uint32_t irq_status;

    /* DMA engine */
#define EDU_DMA_RUN             0x1
#define EDU_DMA_DIR(cmd)        (((cmd) & 0x2) >> 1)
# define EDU_DMA_FROM_PCI       0
# define EDU_DMA_TO_PCI         1
#define EDU_DMA_IRQ             0x4
    struct dma_state {
        dma_addr_t src;
        dma_addr_t dst;
        dma_addr_t cnt;
        dma_addr_t cmd;
    } dma;
    QEMUTimer dma_timer;
    char dma_buf[DMA_SIZE];
    uint64_t dma_mask;

    /* C++ methods — replace free functions */
    bool msiEnabled();
    void raiseIrq(uint32_t val);
    void lowerIrq(uint32_t val);
    void checkRange(uint64_t addr, uint64_t size, uint64_t count);
    void clrDmaStatus();
    void dmaRw(int is_write, dma_addr_t *val, dma_addr_t *dma_addr,
               bool is_addr64);

    void realize(Error **errp);
    void uninit();
    void instanceInit();

    /* Static callbacks for QEMU infrastructure */
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);
    static void dmaTimerCb(void *opaque);
    static void *factThread(void *opaque);
    static void classInit(ObjectClass *klass, const void *data);
};

/* QOM type checking macro — same as original */
DECLARE_INSTANCE_CHECKER(EduState, EDU, TYPE_PCI_EDU_DEVICE)

/* ========================================================================
 * Implementation — methods on EduState instead of free functions
 * ======================================================================== */

bool EduState::msiEnabled()
{
    return msi_enabled(&pdev);
}

void EduState::raiseIrq(uint32_t val)
{
    irq_status |= val;
    if (irq_status) {
        if (msiEnabled()) {
            msi_notify(&pdev, 0);
        } else {
            pci_set_irq(&pdev, 1);
        }
    }
}

void EduState::lowerIrq(uint32_t val)
{
    irq_status &= ~val;
    if (!irq_status && !msiEnabled()) {
        pci_set_irq(&pdev, 0);
    }
}

void EduState::checkRange(uint64_t addr, uint64_t size1, uint64_t count)
{
    uint64_t max = (addr < DMA_START) ? DMA_START : (DMA_START + DMA_SIZE);

    if (addr + count < addr || addr + count > max) {
        return;
    }
}

void EduState::clrDmaStatus()
{
    dma.cmd &= ~EDU_DMA_RUN;
}

void EduState::dmaRw(int is_write, dma_addr_t *val, dma_addr_t *dma_addr,
                      bool is_addr64)
{
    if (*dma_addr + sizeof(dma_addr_t) > DMA_SIZE) {
        return;
    }
    if (is_write) {
        uint64_t dst = *val;
        memcpy(dma_buf + *dma_addr, &dst, sizeof(uint64_t));
    } else {
        uint64_t dst = 0;
        memcpy(&dst, dma_buf + *dma_addr, sizeof(uint64_t));
        *val = dst;
    }
    *dma_addr += sizeof(dma_addr_t);
}

void EduState::dmaTimerCb(void *opaque)
{
    EduState *edu = static_cast<EduState *>(opaque);
    bool raise_irq = false;

    if (!(edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_FROM_PCI) {
        uint64_t dst = edu->dma.dst;
        edu->checkRange(dst, DMA_SIZE, edu->dma.cnt);
        dst -= DMA_START;
        pci_dma_read(&edu->pdev, edu->dma.src,
                edu->dma_buf + dst, edu->dma.cnt);
    } else {
        uint64_t src = edu->dma.src;
        edu->checkRange(src, DMA_SIZE, edu->dma.cnt);
        src -= DMA_START;
        pci_dma_write(&edu->pdev, edu->dma.dst,
                edu->dma_buf + src, edu->dma.cnt);
    }

    edu->clrDmaStatus();

    if (edu->dma.cmd & EDU_DMA_IRQ) {
        raise_irq = true;
    }

    if (raise_irq) {
        edu->raiseIrq(DMA_IRQ);
    }
}

void *EduState::factThread(void *opaque)
{
    EduState *edu = static_cast<EduState *>(opaque);

    while (1) {
        uint32_t val, ret = 1;

        qemu_mutex_lock(&edu->thr_mutex);
        while ((qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) == 0 &&
                        !edu->stopping) {
            qemu_cond_wait(&edu->thr_cond, &edu->thr_mutex);
        }

        if (edu->stopping) {
            qemu_mutex_unlock(&edu->thr_mutex);
            break;
        }

        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);

        while (val > 0) {
            ret *= val--;
        }

        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = ret;
        qemu_mutex_unlock(&edu->thr_mutex);
        qatomic_and(&edu->status, ~EDU_STATUS_COMPUTING);

        smp_mb__after_rmw();

        if (qatomic_read(&edu->status) & EDU_STATUS_IRQFACT) {
            bql_lock();
            edu->raiseIrq(FACT_IRQ);
            bql_unlock();
        }
    }

    return NULL;
}

uint64_t EduState::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    EduState *edu = static_cast<EduState *>(opaque);
    uint64_t val = ~0ULL;

    if (addr < 0x80 && size != 4) {
        return val;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return val;
    }

    switch (addr) {
    case 0x00:
        val = 0x010000edu;
        break;
    case 0x04:
        val = edu->addr4;
        break;
    case 0x08:
        qemu_mutex_lock(&edu->thr_mutex);
        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        val = qatomic_read(&edu->status);
        break;
    case 0x24:
        val = edu->irq_status;
        break;
    case 0x80:
        edu->dmaRw(0, &val, &edu->dma.src, false);
        break;
    case 0x88:
        edu->dmaRw(0, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        edu->dmaRw(0, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        edu->dmaRw(0, &val, &edu->dma.cmd, false);
        break;
    }

    return val;
}

void EduState::mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    EduState *edu = static_cast<EduState *>(opaque);

    if (addr < 0x80 && size != 4) {
        return;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return;
    }

    switch (addr) {
    case 0x04:
        edu->addr4 = ~val;
        break;
    case 0x08:
        if (qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) {
            break;
        }
        /* EDU_STATUS_COMPUTING cannot go 0->1 concurrently, because
         * it is only set in this function and it is under the BQL.
         */
        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = val;
        qatomic_or(&edu->status, EDU_STATUS_COMPUTING);
        qemu_cond_signal(&edu->thr_cond);
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        if (val & EDU_STATUS_IRQFACT) {
            qatomic_or(&edu->status, EDU_STATUS_IRQFACT);
        } else {
            qatomic_and(&edu->status, ~EDU_STATUS_IRQFACT);
        }
        break;
    case 0x60:
        edu->raiseIrq(val);
        break;
    case 0x64:
        edu->lowerIrq(val);
        break;
    case 0x80:
        edu->dmaRw(1, &val, &edu->dma.src, false);
        break;
    case 0x88:
        edu->dmaRw(1, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        edu->dmaRw(1, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        if (!(val & EDU_DMA_RUN)) {
            break;
        }
        edu->dmaRw(1, &val, &edu->dma.cmd, false);
        timer_mod(&edu->dma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        break;
    }
}

static const MemoryRegionOps edu_mmio_ops = {
    .read = EduState::mmioRead,
    .write = EduState::mmioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/* ========================================================================
 * Lifecycle methods
 * ======================================================================== */

void EduState::realize(Error **errp)
{
    uint8_t *pci_conf = pdev.config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (msi_init(&pdev, 0, 1, true, false, errp)) {
        return;
    }

    timer_init_ms(&dma_timer, QEMU_CLOCK_VIRTUAL, dmaTimerCb, this);

    qemu_mutex_init(&thr_mutex);
    qemu_cond_init(&thr_cond);
    qemu_thread_create(&thread, "edu", factThread,
                       this, QEMU_THREAD_JOINABLE);

    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this), &edu_mmio_ops, this,
                    "edu-mmio", 1 * MiB);
    pci_register_bar(&pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &mmio);
}

void EduState::uninit()
{
    qemu_mutex_lock(&thr_mutex);
    stopping = true;
    qemu_mutex_unlock(&thr_mutex);
    qemu_cond_signal(&thr_cond);
    qemu_thread_join(&thread);

    qemu_cond_destroy(&thr_cond);
    qemu_mutex_destroy(&thr_mutex);

    timer_del(&dma_timer);
    msi_uninit(&pdev);
}

void EduState::instanceInit()
{
    dma_mask = (1UL << 28) - 1;
    object_property_add_uint64_ptr(reinterpret_cast<Object *>(this), "dma_mask",
                                   &dma_mask, OBJ_PROP_FLAG_READWRITE);
}

/* ========================================================================
 * QOM registration — thin callbacks delegate to C++ methods
 * ======================================================================== */

static void pci_edu_realize(PCIDevice *pdev, Error **errp)
{
    reinterpret_cast<EduState *>(pdev)->realize(errp);
}

static void pci_edu_uninit(PCIDevice *pdev)
{
    reinterpret_cast<EduState *>(pdev)->uninit();
}

static void edu_instance_init(Object *obj)
{
    reinterpret_cast<EduState *>(obj)->instanceInit();
}

void EduState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->realize = pci_edu_realize;
    k->exit = pci_edu_uninit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0x11e8;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const InterfaceInfo edu_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo edu_types[] = {
    {
        .name          = TYPE_PCI_EDU_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(EduState),
        .instance_init = edu_instance_init,
        .class_init    = EduState::classInit,
        .interfaces    = edu_interfaces,
    }
};

DEFINE_TYPES(edu_types)
