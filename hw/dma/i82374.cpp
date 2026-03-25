/*
 * QEMU Intel 82374 emulation (Enhanced DMA controller)
 *
 * Copyright (c) 2010 Herve Poussineau
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
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/dma/i8257.h"
#include "qom/object.h"

#define TYPE_I82374 "i82374"
OBJECT_DECLARE_SIMPLE_TYPE(I82374State, I82374)

//#define DEBUG_I82374

#ifdef DEBUG_I82374
#define DPRINTF(fmt, ...) \
do { fprintf(stderr, "i82374: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
do {} while (0)
#endif
#define BADF(fmt, ...) \
do { fprintf(stderr, "i82374 ERROR: " fmt , ## __VA_ARGS__); } while (0)

struct I82374State {
    ISADevice parent_obj;

    uint32_t iobase;
    uint8_t commands[8];
    PortioList port_list;

    static uint32_t readIsr(void *opaque, uint32_t nport)
    {
        uint32_t val = 0;

        BADF("%s: %08x\n", __func__, nport);

        DPRINTF("%s: %08x=%08x\n", __func__, nport, val);
        return val;
    }

    static void writeCommand(void *opaque, uint32_t nport, uint32_t data)
    {
        DPRINTF("%s: %08x=%08x\n", __func__, nport, data);

        if (data != 0x42) {
            /* Not Stop S/G command */
            BADF("%s: %08x=%08x\n", __func__, nport, data);
        }
    }

    static uint32_t readStatus(void *opaque, uint32_t nport)
    {
        uint32_t val = 0;

        BADF("%s: %08x\n", __func__, nport);

        DPRINTF("%s: %08x=%08x\n", __func__, nport, val);
        return val;
    }

    static void writeDescriptor(void *opaque, uint32_t nport, uint32_t data)
    {
        DPRINTF("%s: %08x=%08x\n", __func__, nport, data);

        BADF("%s: %08x=%08x\n", __func__, nport, data);
    }

    static uint32_t readDescriptor(void *opaque, uint32_t nport)
    {
        uint32_t val = 0;

        BADF("%s: %08x\n", __func__, nport);

        DPRINTF("%s: %08x=%08x\n", __func__, nport, val);
        return val;
    }

    void realize(Error **errp)
    {
        ISABus *isa_bus = isa_bus_from_device(ISA_DEVICE(DEVICE(this)));

        if (isa_bus_get_dma(isa_bus, 0)) {
            error_setg(errp, "DMA already initialized on ISA bus");
            return;
        }
        i8257_dma_init(OBJECT(this), isa_bus, true);

        portio_list_init(&port_list, OBJECT(this), i82374_portio_list, this,
                         "i82374");
        portio_list_add(&port_list, isa_address_space_io(&parent_obj),
                        iobase);

        memset(commands, 0, sizeof(commands));
    }

    static void deviceRealize(DeviceState *dev, Error **errp)
    {
        I82374State *s = I82374(dev);
        s->realize(errp);
    }

    static void classInit(ObjectClass *klass, const void *data);

    static const MemoryRegionPortio i82374_portio_list[];
    static const VMStateDescription vmstate_i82374;
    static const Property i82374_properties[];
};

const VMStateDescription I82374State::vmstate_i82374 = {
    .name = "i82374",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(commands, I82374State, 8),
        VMSTATE_END_OF_LIST()
    },
};

const MemoryRegionPortio I82374State::i82374_portio_list[] = {
    { .offset = 0x0A, .len = 1, .size = 1, .read = I82374State::readIsr, },
    { .offset = 0x10, .len = 8, .size = 1, .write = I82374State::writeCommand, },
    { .offset = 0x18, .len = 8, .size = 1, .read = I82374State::readStatus, },
    { .offset = 0x20, .len = 0x20, .size = 1,
      .read = I82374State::readDescriptor, .write = I82374State::writeDescriptor, },
    PORTIO_END_OF_LIST(),
};

const Property I82374State::i82374_properties[] = {
    DEFINE_PROP_UINT32("iobase", I82374State, iobase, 0x400),
};

void I82374State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deviceRealize;
    dc->vmsd = &vmstate_i82374;
    device_class_set_props(dc, i82374_properties);
    dc->desc = "Intel 82374 DMA controller";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo i82374_info = {
    .name  = TYPE_I82374,
    .parent = TYPE_ISA_DEVICE,
    .instance_size  = sizeof(I82374State),
    .class_init = I82374State::classInit,
};

static void i82374_register_types(void)
{
    type_register_static(&i82374_info);
}

type_init(i82374_register_types)
