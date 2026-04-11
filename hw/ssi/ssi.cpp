/*
 * QEMU Synchronous Serial Interface support
 *
 * Copyright (c) 2009 CodeSourcery.
 * Copyright (c) 2012 Peter A.G. Crosthwaite (peter.crosthwaite@petalogix.com)
 * Copyright (c) 2012 PetaLogix Pty Ltd.
 * Written by Paul Brook
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qom/cpp/object.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"

extern "C" {
#include "qemu/module.h"
}

struct SSIBus {
    BusState parent_obj;
};

#define TYPE_SSI_BUS "SSI"
OBJECT_DECLARE_SIMPLE_TYPE(SSIBus, SSI_BUS)

extern "C"
DeviceState *ssi_get_cs(SSIBus *bus, uint8_t cs_index)
{
    BusState *b = BUS(bus);
    BusChild *kid;

    QTAILQ_FOREACH(kid, &b->children, sibling) {
        SSIPeripheral *kid_ssi = SSI_PERIPHERAL(kid->child);
        if (kid_ssi->cs_index == cs_index) {
            return kid->child;
        }
    }

    return NULL;
}

static bool ssi_bus_check_address(BusState *b, DeviceState *dev, Error **errp)
{
    SSIPeripheral *s = SSI_PERIPHERAL(dev);

    if (ssi_get_cs(SSI_BUS(b), s->cs_index)) {
        error_setg(errp, "CS index '0x%x' in use by a %s device", s->cs_index,
                   object_get_typename(OBJECT(dev)));
        return false;
    }

    return true;
}

static void ssi_bus_class_init(ObjectClass *klass, const void *data)
{
    BusClass *k = BUS_CLASS(klass);

    k->check_address = ssi_bus_check_address;
}

static const TypeInfo ssi_bus_info = {
    .name = TYPE_SSI_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(SSIBus),
    .class_init = ssi_bus_class_init,
};

static void ssi_cs_default(void *opaque, int n, int level)
{
    SSIPeripheral *s = SSI_PERIPHERAL(opaque);
    bool cs = !!level;
    assert(n == 0);
    if (s->cs != cs) {
        s->spc->set_cs(s, cs);
    }
    s->cs = cs;
}

/* Default virtual method implementations for SSIPeripheralClass */
void SSIPeripheralClass::realize(SSIPeripheral *dev, Error **errp) {}
uint32_t SSIPeripheralClass::transfer(SSIPeripheral *dev, uint32_t val) { return 0; }
int SSIPeripheralClass::set_cs(SSIPeripheral *dev, bool select) { return 0; }

uint32_t SSIPeripheralClass::transfer_raw(SSIPeripheral *dev, uint32_t val)
{
    SSIPeripheralClass *ssc = dev->spc;

    if ((dev->cs && ssc->cs_polarity == SSI_CS_HIGH) ||
        (!dev->cs && ssc->cs_polarity == SSI_CS_LOW) ||
        ssc->cs_polarity == SSI_CS_NONE) {
        return ssc->transfer(dev, val);
    }
    return 0;
}

static void ssi_peripheral_realize(DeviceState *dev, Error **errp)
{
    SSIPeripheral *s = SSI_PERIPHERAL(dev);
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_GET_CLASS(s);

    if (ssc->cs_polarity != SSI_CS_NONE) {
        qdev_init_gpio_in_named(dev, ssi_cs_default, SSI_GPIO_CS, 1);
    }
    s->spc = ssc;

    ssc->realize(s, errp);
}

static const Property ssi_peripheral_properties[] = {
    DEFINE_PROP_UINT8("cs", SSIPeripheral, cs_index, 0),
};

static void ssi_peripheral_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    qom_fixup_vtable<SSIPeripheralClass>(klass);

    dc->realize = ssi_peripheral_realize;
    dc->bus_type = TYPE_SSI_BUS;
    device_class_set_props(dc, ssi_peripheral_properties);
}

static const TypeInfo ssi_peripheral_info = {
    .name = TYPE_SSI_PERIPHERAL,
    .parent = TYPE_DEVICE,
    .is_abstract = true,
    .class_size = sizeof(SSIPeripheralClass),
    .class_init = ssi_peripheral_class_init,
};

extern "C"
bool ssi_realize_and_unref(DeviceState *dev, SSIBus *bus, Error **errp)
{
    return qdev_realize_and_unref(dev, &bus->parent_obj, errp);
}

extern "C"
DeviceState *ssi_create_peripheral(SSIBus *bus, const char *name)
{
    DeviceState *dev = qdev_new(name);

    ssi_realize_and_unref(dev, bus, &error_fatal);
    return dev;
}

extern "C"
SSIBus *ssi_create_bus(DeviceState *parent, const char *name)
{
    BusState *bus;
    bus = qbus_new(TYPE_SSI_BUS, parent, name);
    return SSI_BUS(bus);
}

extern "C"
uint32_t ssi_transfer(SSIBus *bus, uint32_t val)
{
    BusState *b = BUS(bus);
    BusChild *kid;
    uint32_t r = 0;

    QTAILQ_FOREACH(kid, &b->children, sibling) {
        SSIPeripheral *p = SSI_PERIPHERAL(kid->child);
        r |= p->spc->transfer_raw(p, val);
    }

    return r;
}

static const VMStateField vmstate_ssi_peripheral_fields[] = {
    VMSTATE_BOOL(cs, SSIPeripheral),
    VMSTATE_END_OF_LIST()
};

extern "C"
const VMStateDescription vmstate_ssi_peripheral = {
    .name = "SSISlave",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_ssi_peripheral_fields,
};

static void ssi_peripheral_register_types(void)
{
    type_register_static(&ssi_bus_info);
    type_register_static(&ssi_peripheral_info);
}

type_init(ssi_peripheral_register_types)
