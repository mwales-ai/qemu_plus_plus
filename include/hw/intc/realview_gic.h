/*
 * ARM RealView Emulation Baseboard Interrupt Controller
 *
 * Copyright (c) 2006-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#ifndef HW_INTC_REALVIEW_GIC_H
#define HW_INTC_REALVIEW_GIC_H

#include "hw/sysbus.h"
#include "hw/intc/arm_gic.h"
#include "qom/object.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE_REALVIEW_GIC "realview_gic"
OBJECT_DECLARE_SIMPLE_TYPE(RealViewGICState, REALVIEW_GIC)

struct RealViewGICState {
    SysBusDevice parent_obj;

    MemoryRegion container;

    GICState gic;

#ifdef __cplusplus
    void init();
    void realize(Error **errp);
    void classInit(DeviceClass *dc);
#endif
};

#ifdef __cplusplus
}
#endif

#endif
