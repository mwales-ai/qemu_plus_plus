/*
 * QEMU++ Demo Timer Device
 *
 * A minimal timer device implemented using the C++ QOM wrappers.
 * Demonstrates that C++ classes work as first-class QEMU devices:
 *   - Registers as a QOM type via QEMU_SYSBUS_DEVICE_REGISTER
 *   - Uses C++ constructor/destructor instead of instance_init/finalize
 *   - Uses virtual realize()/reset() instead of function pointer assignment
 *   - Uses normal C++ member variables instead of a flat State struct
 *   - Works with existing MMIO, IRQ, and VMState infrastructure
 *
 * To test: -device cpp-demo-timer
 *
 * Copyright (c) 2026 QEMU++ Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

/*
 * offsetof() on non-standard-layout types (classes with virtual methods)
 * is "conditionally supported" in C++17 and works on GCC/Clang. Suppress
 * the warning since VMState and Property macros require it.
 */
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qom/cpp/device.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"

/* ========================================================================
 * Device class declaration — clean C++ with no QOM boilerplate
 * ======================================================================== */

/*
 * CppDemoTimer: the device struct.
 *
 * CRITICAL: the first member MUST be the parent QOM struct (SysBusDevice).
 * This is how QOM's inheritance works — the parent is physically embedded
 * at offset 0. Our C++ wrapper methods (realize/reset) are dispatched
 * through the QEMU_SYSBUS_DEVICE_REGISTER macro's generated callbacks.
 */
struct CppDemoTimer
{
    /* QOM parent — MUST be first member */
    SysBusDevice parent_obj;

    /* Static type name for QOM registration */
    static const char *staticTypeName() { return "cpp-demo-timer"; }

    /* C++ lifecycle methods — called by generated QOM callbacks */
    void cppInit();
    void cppFinalize();
    void realize(Error **errp);
    void reset();

    /* Called by QEMU_SYSBUS_DEVICE_REGISTER to set up class metadata */
    static void classInit(DeviceClass *dc);

    /* MMIO handlers */
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);

    /* Internal methods */
    void timerTick();
    void updateIrq();
    static void timerCallback(void *opaque);

    /* Device state — public for VMState/Property macro access */
    MemoryRegion theMmio;
    QEMUTimer *theTimer;
    qemu_irq theIrq;

    uint32_t theCount;
    uint32_t theReload;
    uint32_t theControl;
    uint32_t theStatus;
    uint64_t theFrequency;
};

/* ========================================================================
 * Register map
 * ======================================================================== */

enum CppDemoTimerReg
{
    REG_COUNT    = 0x00,   /* R:   current counter value */
    REG_RELOAD   = 0x04,   /* R/W: reload value */
    REG_CONTROL  = 0x08,   /* R/W: control register */
    REG_STATUS   = 0x0C,   /* R/W1C: status register */
};

#define CTRL_ENABLE     (1u << 0)
#define CTRL_IRQ_ENABLE (1u << 1)
#define STATUS_IRQ      (1u << 0)

/* ========================================================================
 * MMIO dispatch table — same as any C device
 * ======================================================================== */

static const MemoryRegionOps cpp_demo_timer_ops = {
    .read = CppDemoTimer::mmioRead,
    .write = CppDemoTimer::mmioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ========================================================================
 * VMState — works with C++ classes via offsetof, no changes needed
 * ======================================================================== */

static const VMStateField vmstate_cpp_demo_timer_fields[] = {
    VMSTATE_UINT32(theCount, CppDemoTimer),
    VMSTATE_UINT32(theReload, CppDemoTimer),
    VMSTATE_UINT32(theControl, CppDemoTimer),
    VMSTATE_UINT32(theStatus, CppDemoTimer),
    VMSTATE_TIMER_PTR(theTimer, CppDemoTimer),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_cpp_demo_timer = {
    .name = "cpp-demo-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_cpp_demo_timer_fields,
};

/* ========================================================================
 * Properties — same DEFINE_PROP macros work with C++ classes
 * ======================================================================== */

static const Property cpp_demo_timer_properties[] = {
    DEFINE_PROP_UINT64("frequency", CppDemoTimer, theFrequency, 1000000),
};

/* ========================================================================
 * Implementation — straightforward C++ methods
 * ======================================================================== */

void CppDemoTimer::cppInit()
{
    theTimer = nullptr;
    theCount = 0;
    theReload = 0xFFFFFFFF;
    theControl = 0;
    theStatus = 0;
    /* theFrequency set by property system */
}

void CppDemoTimer::cppFinalize()
{
    if (theTimer) {
        timer_free(theTimer);
        theTimer = nullptr;
    }
}

void CppDemoTimer::realize(Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);

    memory_region_init_io(&theMmio, OBJECT(this), &cpp_demo_timer_ops,
                          this, "cpp-demo-timer", 0x100);
    sysbus_init_mmio(sbd, &theMmio);
    sysbus_init_irq(sbd, &theIrq);

    theTimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timerCallback, this);
}

void CppDemoTimer::reset()
{
    theCount = theReload;
    theControl = 0;
    theStatus = 0;
    timer_del(theTimer);
    qemu_set_irq(theIrq, 0);
}

void CppDemoTimer::timerTick()
{
    if (theCount == 0) {
        theCount = theReload;
        theStatus |= STATUS_IRQ;
        updateIrq();
    } else {
        theCount--;
    }

    if (theControl & CTRL_ENABLE) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t period = NANOSECONDS_PER_SECOND / theFrequency;
        timer_mod(theTimer, now + period);
    }
}

void CppDemoTimer::updateIrq()
{
    int level = (theStatus & STATUS_IRQ) && (theControl & CTRL_IRQ_ENABLE);
    qemu_set_irq(theIrq, level);
}

void CppDemoTimer::timerCallback(void *opaque)
{
    CppDemoTimer *self = static_cast<CppDemoTimer *>(opaque);
    self->timerTick();
}

uint64_t CppDemoTimer::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    CppDemoTimer *self = static_cast<CppDemoTimer *>(opaque);

    switch (addr) {
    case REG_COUNT:
        return self->theCount;
    case REG_RELOAD:
        return self->theReload;
    case REG_CONTROL:
        return self->theControl;
    case REG_STATUS:
        return self->theStatus;
    default:
        return 0;
    }
}

void CppDemoTimer::mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    CppDemoTimer *self = static_cast<CppDemoTimer *>(opaque);

    switch (addr) {
    case REG_RELOAD:
        self->theReload = static_cast<uint32_t>(val);
        break;

    case REG_CONTROL:
        {
            uint32_t old = self->theControl;
            self->theControl = static_cast<uint32_t>(val);

            /* Start timer on enable edge */
            if (!(old & CTRL_ENABLE) && (val & CTRL_ENABLE)) {
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                int64_t period = NANOSECONDS_PER_SECOND / self->theFrequency;
                timer_mod(self->theTimer, now + period);
            }

            /* Stop timer on disable */
            if ((old & CTRL_ENABLE) && !(val & CTRL_ENABLE)) {
                timer_del(self->theTimer);
            }

            self->updateIrq();
        }
        break;

    case REG_STATUS:
        /* Write-1-to-clear */
        self->theStatus &= ~static_cast<uint32_t>(val);
        self->updateIrq();
        break;
    }
}

void CppDemoTimer::classInit(DeviceClass *dc)
{
    dc->desc = "C++ Demo Timer Device";
    dc->vmsd = &vmstate_cpp_demo_timer;
    device_class_set_props(dc, cpp_demo_timer_properties);
}

/* ========================================================================
 * Registration — one macro replaces 30+ lines of QOM boilerplate
 * ======================================================================== */

/* ========================================================================
 * QOM registration — hand-written for now since the struct-based approach
 * doesn't use C++ virtual methods (no vtable pointer at offset 0).
 * This will be simplified once the base class design is finalized.
 * ======================================================================== */

static void cpp_demo_timer_realize(DeviceState *dev, Error **errp)
{
    CppDemoTimer *self = reinterpret_cast<CppDemoTimer *>(dev);
    self->realize(errp);
}

static void cpp_demo_timer_reset(DeviceState *dev)
{
    CppDemoTimer *self = reinterpret_cast<CppDemoTimer *>(dev);
    self->reset();
}

static void cpp_demo_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = cpp_demo_timer_realize;
    device_class_set_legacy_reset(dc, cpp_demo_timer_reset);
    CppDemoTimer::classInit(dc);
}

static void cpp_demo_timer_instance_init(Object *obj)
{
    CppDemoTimer *self = reinterpret_cast<CppDemoTimer *>(obj);
    self->cppInit();
}

static void cpp_demo_timer_instance_finalize(Object *obj)
{
    CppDemoTimer *self = reinterpret_cast<CppDemoTimer *>(obj);
    self->cppFinalize();
}

static const TypeInfo cpp_demo_timer_type_info = {
    .name          = CppDemoTimer::staticTypeName(),
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CppDemoTimer),
    .instance_init = cpp_demo_timer_instance_init,
    .instance_finalize = cpp_demo_timer_instance_finalize,
    .class_init    = cpp_demo_timer_class_init,
};

static void cpp_demo_timer_register_types(void)
{
    type_register_static(&cpp_demo_timer_type_info);
}

type_init(cpp_demo_timer_register_types)
