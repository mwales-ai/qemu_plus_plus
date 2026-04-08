/*
 * QEMU I/O port 0x92 (System Control Port A, to handle Fast Gate A20)
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "system/runstate.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/i386/pc.h"
#include "qom/object.h"

extern "C" {
#include "trace.h"
}

OBJECT_DECLARE_SIMPLE_TYPE(Port92State, PORT92)

struct Port92State {
    ISADevice parent_obj;

    MemoryRegion io;
    uint8_t outport;
    qemu_irq a20_out;

    static uint64_t ioRead(void *opaque, hwaddr addr, unsigned size)
    {
        Port92State *s = static_cast<Port92State *>(opaque);
        uint32_t ret;

        ret = s->outport;
        trace_port92_read(ret);

        return ret;
    }

    static void ioWrite(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
    {
        Port92State *s = static_cast<Port92State *>(opaque);
        int oldval = s->outport;

        trace_port92_write(val);
        s->outport = val;
        qemu_set_irq(s->a20_out, (val >> 1) & 1);
        if ((val & 1) && !(oldval & 1)) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    }

    void deviceReset()
    {
        outport &= ~1;
    }

    void initfn()
    {
        memory_region_init_io(&io, reinterpret_cast<Object *>(this), &port92_ops, this,
                              "port92", 1);

        outport = 0;

        qdev_init_gpio_out_named(reinterpret_cast<DeviceState *>(this), &a20_out, PORT92_A20_LINE, 1);
    }

    void realize(Error **errp)
    {
        ISADevice *isadev = ISA_DEVICE(reinterpret_cast<DeviceState *>(this));

        isa_register_ioport(isadev, &io, 0x92);
    }

    static void deviceReset_static(DeviceState *d)
    {
        Port92State *s = PORT92(d);
        s->deviceReset();
    }

    static void instanceInit(Object *obj)
    {
        Port92State *s = PORT92(obj);
        s->initfn();
    }

    static void deviceRealize(DeviceState *dev, Error **errp)
    {
        Port92State *s = PORT92(dev);
        s->realize(errp);
    }

    static void classInit(ObjectClass *klass, const void *data)
    {
        DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

        dc->realize = deviceRealize;
        device_class_set_legacy_reset(dc, deviceReset_static);
        dc->vmsd = &vmstate_port92_isa;
        /*
         * Reason: unlike ordinary ISA devices, this one needs additional
         * wiring: its A20 output line needs to be wired up with
         * qdev_connect_gpio_out_named().
         */
        dc->user_creatable = false;
    }

    static MemoryRegionOps port92_ops;
    static const VMStateDescription vmstate_port92_isa;
};

static const VMStateField vmstate_port92_isa_fields[] = {
    VMSTATE_UINT8(outport, Port92State),
    VMSTATE_END_OF_LIST()
};

const VMStateDescription Port92State::vmstate_port92_isa = {
    .name = "port92",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_port92_isa_fields,
};

MemoryRegionOps Port92State::port92_ops = {
    .read = Port92State::ioRead,
    .write = Port92State::ioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void __attribute__((constructor)) init_port92_ops(void)
{
    Port92State::port92_ops.impl.min_access_size = 1;
    Port92State::port92_ops.impl.max_access_size = 1;
}

static const TypeInfo port92_info = {
    .name          = TYPE_PORT92,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Port92State),
    .instance_init = Port92State::instanceInit,
    .class_init    = Port92State::classInit,
};

static void port92_register_types(void)
{
    type_register_static(&port92_info);
}

type_init(port92_register_types)
