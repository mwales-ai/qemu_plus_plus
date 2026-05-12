/*
 * QEMU M48T59 and M48T08 NVRAM emulation (ISA bus interface)
 *
 * Copyright (c) 2003-2005, 2007 Jocelyn Mayer
 * Copyright (c) 2013 Herve Poussineau
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
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "hw/rtc/m48t59.h"
#include "m48t59-internal.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_M48TXX_ISA "isa-m48txx"
typedef struct M48txxISADeviceClass M48txxISADeviceClass;
typedef struct M48txxISAState M48txxISAState;
DECLARE_OBJ_CHECKERS(M48txxISAState, M48txxISADeviceClass,
                     M48TXX_ISA, TYPE_M48TXX_ISA)

struct M48txxISADeviceClass {
    DeviceClass parent_class;
    M48txxInfo info;
};

struct M48txxISAState {
    ISADevice parent_obj;
    M48t59State state;
    uint32_t io_base;
    uint8_t isairq;
    MemoryRegion io;

    static uint32_t nvramRead(Nvram *obj, uint32_t addr)
    {
        M48txxISAState *d = reinterpret_cast<M48txxISAState *>(obj);
        return m48t59_read(&d->state, addr);
    }

    static void nvramWrite(Nvram *obj, uint32_t addr, uint32_t val)
    {
        M48txxISAState *d = reinterpret_cast<M48txxISAState *>(obj);
        m48t59_write(&d->state, addr, val);
    }

    static void nvramToggleLock(Nvram *obj, int lock)
    {
        M48txxISAState *d = reinterpret_cast<M48txxISAState *>(obj);
        m48t59_toggle_lock(&d->state, lock);
    }

    void deviceReset()
    {
        M48t59State *NVRAM = &state;
        m48t59_reset_common(NVRAM);
    }

    void realize(Error **errp)
    {
        M48txxISADeviceClass *u = M48TXX_ISA_GET_CLASS(reinterpret_cast<DeviceState *>(this));
        ISADevice *isadev = reinterpret_cast<ISADevice *>(reinterpret_cast<DeviceState *>(this));
        M48t59State *s = &state;

        if (isairq >= ISA_NUM_IRQS) {
            error_setg(errp, "Maximum value for \"irq\" is: %u", ISA_NUM_IRQS - 1);
            return;
        }

        s->model = u->info.model;
        s->size = u->info.size;
        s->IRQ = isa_get_irq(isadev, isairq);
        m48t59_realize_common(s, errp);
        memory_region_init_io(&io, reinterpret_cast<Object *>(this), &m48t59_io_ops, s, "m48t59", 4);
        if (io_base != 0) {
            isa_register_ioport(isadev, &io, io_base);
        }
    }

    static void deviceReset_static(DeviceState *d)
    {
        M48txxISAState *isa = reinterpret_cast<M48txxISAState *>(d);
        isa->deviceReset();
    }

    static void deviceRealize(DeviceState *dev, Error **errp)
    {
        M48txxISAState *s = reinterpret_cast<M48txxISAState *>(dev);
        s->realize(errp);
    }

    static void classInit(ObjectClass *klass, const void *data);

    static void concreteClassInit(ObjectClass *klass, const void *data)
    {
        M48txxISADeviceClass *u = reinterpret_cast<M48txxISADeviceClass *>(klass);
        const M48txxInfo *info = static_cast<const M48txxInfo *>(data);

        u->info = *info;
    }

    static const Property m48t59_isa_properties[];
};

static M48txxInfo m48txx_isa_info[] = {
    {
        .bus_name = "isa-m48t59",
        .model = 59,
        .size = 0x2000,
    }
};

const Property M48txxISAState::m48t59_isa_properties[] = {
    DEFINE_PROP_INT32("base-year", M48txxISAState, state.base_year, 0),
    DEFINE_PROP_UINT32("iobase", M48txxISAState, io_base, 0x74),
    DEFINE_PROP_UINT8("irq", M48txxISAState, isairq, 8),
};

void M48txxISAState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    NvramClass *nc = reinterpret_cast<NvramClass *>(klass);

    dc->realize = deviceRealize;
    device_class_set_legacy_reset(dc, deviceReset_static);
    device_class_set_props(dc, m48t59_isa_properties);
    nc->read = nvramRead;
    nc->write = nvramWrite;
    nc->toggle_lock = nvramToggleLock;
}

static const InterfaceInfo m48txx_isa_interfaces[] = {
    { TYPE_NVRAM },
    { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS_IFACES(M48txxISAState,
                                                      TYPE_M48TXX_ISA,
                                                      TYPE_ISA_DEVICE,
                                                      M48txxISAState::classInit,
                                                      m48txx_isa_interfaces)

REGISTER_QEMU_OBJECT_CLASS_DATA_CS(isa_m48t59, M48txxISADeviceClass,
                                    "isa-m48t59", TYPE_M48TXX_ISA,
                                    M48txxISAState::concreteClassInit,
                                    &m48txx_isa_info[0])
