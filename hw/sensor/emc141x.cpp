/*
 * SMSC EMC141X temperature sensor.
 *
 * Copyright (c) 2020 Bytedance Corporation
 * Written by John Wang <wangzhiqiang.bj@bytedance.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/sensor/emc141x_regs.h"

#define SENSORS_COUNT_MAX    4

struct EMC141XState {
    I2CSlave parent_obj;
    struct {
        uint8_t raw_temp_min;
        uint8_t raw_temp_current;
        uint8_t raw_temp_max;
    } sensor[SENSORS_COUNT_MAX];
    uint8_t len;
    uint8_t data;
    uint8_t pointer;

    static void getTemperature(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp);
    static void setTemperature(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp);
    void read();
    void write();
    static uint8_t rx(I2CSlave *i2c);
    static int tx(I2CSlave *i2c, uint8_t data);
    static int event(I2CSlave *i2c, enum i2c_event event);
    void reset();
    void init();
    static void classInit(ObjectClass *klass, const void *data);
};

struct EMC141XClass {
    I2CSlaveClass parent_class;
    uint8_t model;
    unsigned sensors_count;

    static void emc1413ClassInit(ObjectClass *klass, const void *data);
    static void emc1414ClassInit(ObjectClass *klass, const void *data);
};

#define TYPE_EMC141X "emc141x"
OBJECT_DECLARE_TYPE(EMC141XState, EMC141XClass, EMC141X)

void EMC141XState::getTemperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    EMC141XState *s = reinterpret_cast<EMC141XState *>(obj);
    EMC141XClass *sc = EMC141X_GET_CLASS(s);
    int64_t value;
    unsigned tempid;

    if (sscanf(name, "temperature%u", &tempid) != 1) {
        error_setg(errp, "error reading %s: %s", name, g_strerror(errno));
        return;
    }

    if (tempid >= sc->sensors_count) {
        error_setg(errp, "error reading %s", name);
        return;
    }

    value = s->sensor[tempid].raw_temp_current * 1000;

    visit_type_int(v, name, &value, errp);
}

void EMC141XState::setTemperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    EMC141XState *s = reinterpret_cast<EMC141XState *>(obj);
    EMC141XClass *sc = EMC141X_GET_CLASS(s);
    int64_t temp;
    unsigned tempid;

    if (!visit_type_int(v, name, &temp, errp)) {
        return;
    }

    if (sscanf(name, "temperature%u", &tempid) != 1) {
        error_setg(errp, "error reading %s: %s", name, g_strerror(errno));
        return;
    }

    if (tempid >= sc->sensors_count) {
        error_setg(errp, "error reading %s", name);
        return;
    }

    s->sensor[tempid].raw_temp_current = temp / 1000;
}

void EMC141XState::read()
{
    EMC141XClass *sc = EMC141X_GET_CLASS(this);
    switch (pointer) {
    case EMC141X_DEVICE_ID:
        data = sc->model;
        break;
    case EMC141X_MANUFACTURER_ID:
        data = MANUFACTURER_ID;
        break;
    case EMC141X_REVISION:
        data = REVISION;
        break;
    case EMC141X_TEMP_HIGH0:
        data = sensor[0].raw_temp_current;
        break;
    case EMC141X_TEMP_HIGH1:
        data = sensor[1].raw_temp_current;
        break;
    case EMC141X_TEMP_HIGH2:
        data = sensor[2].raw_temp_current;
        break;
    case EMC141X_TEMP_HIGH3:
        data = sensor[3].raw_temp_current;
        break;
    case EMC141X_TEMP_MAX_HIGH0:
        data = sensor[0].raw_temp_max;
        break;
    case EMC141X_TEMP_MAX_HIGH1:
        data = sensor[1].raw_temp_max;
        break;
    case EMC141X_TEMP_MAX_HIGH2:
        data = sensor[2].raw_temp_max;
        break;
    case EMC141X_TEMP_MAX_HIGH3:
        data = sensor[3].raw_temp_max;
        break;
    case EMC141X_TEMP_MIN_HIGH0:
        data = sensor[0].raw_temp_min;
        break;
    case EMC141X_TEMP_MIN_HIGH1:
        data = sensor[1].raw_temp_min;
        break;
    case EMC141X_TEMP_MIN_HIGH2:
        data = sensor[2].raw_temp_min;
        break;
    case EMC141X_TEMP_MIN_HIGH3:
        data = sensor[3].raw_temp_min;
        break;
    default:
        data = 0;
    }
}

void EMC141XState::write()
{
    switch (pointer) {
    case EMC141X_TEMP_MAX_HIGH0:
        sensor[0].raw_temp_max = data;
        break;
    case EMC141X_TEMP_MAX_HIGH1:
        sensor[1].raw_temp_max = data;
        break;
    case EMC141X_TEMP_MAX_HIGH2:
        sensor[2].raw_temp_max = data;
        break;
    case EMC141X_TEMP_MAX_HIGH3:
        sensor[3].raw_temp_max = data;
        break;
    case EMC141X_TEMP_MIN_HIGH0:
        sensor[0].raw_temp_min = data;
        break;
    case EMC141X_TEMP_MIN_HIGH1:
        sensor[1].raw_temp_min = data;
        break;
    case EMC141X_TEMP_MIN_HIGH2:
        sensor[2].raw_temp_min = data;
        break;
    case EMC141X_TEMP_MIN_HIGH3:
        sensor[3].raw_temp_min = data;
        break;
    default:
        data = 0;
    }
}

uint8_t EMC141XState::rx(I2CSlave *i2c)
{
    EMC141XState *s = reinterpret_cast<EMC141XState *>(i2c);

    if (s->len == 0) {
        s->len++;
        return s->data;
    } else {
        return 0xff;
    }
}

int EMC141XState::tx(I2CSlave *i2c, uint8_t data)
{
    EMC141XState *s = reinterpret_cast<EMC141XState *>(i2c);

    if (s->len == 0) {
        /* first byte is the reg pointer */
        s->pointer = data;
        s->len++;
    } else if (s->len == 1) {
        s->data = data;
        s->write();
    }

    return 0;
}

int EMC141XState::event(I2CSlave *i2c, enum i2c_event event)
{
    EMC141XState *s = reinterpret_cast<EMC141XState *>(i2c);

    if (event == I2C_START_RECV) {
        s->read();
    }

    s->len = 0;
    return 0;
}

static const VMStateDescription vmstate_emc141x = {
    .name = "EMC141X",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(len, EMC141XState),
        VMSTATE_UINT8(data, EMC141XState),
        VMSTATE_UINT8(pointer, EMC141XState),
        VMSTATE_I2C_SLAVE(parent_obj, EMC141XState),
        VMSTATE_END_OF_LIST()
    }
};

static void emc141x_reset(DeviceState *dev)
{
    EMC141XState *s = reinterpret_cast<EMC141XState *>(dev);
    s->reset();
}

void EMC141XState::reset()
{
    int i;

    for (i = 0; i < SENSORS_COUNT_MAX; i++) {
        sensor[i].raw_temp_max = 0x55;
    }
    pointer = 0;
    len = 0;
}

void EMC141XState::init()
{
    Object *obj = OBJECT(this);
    object_property_add(obj, "temperature0", "int",
                        EMC141XState::getTemperature,
                        EMC141XState::setTemperature, NULL, NULL);
    object_property_add(obj, "temperature1", "int",
                        EMC141XState::getTemperature,
                        EMC141XState::setTemperature, NULL, NULL);
    object_property_add(obj, "temperature2", "int",
                        EMC141XState::getTemperature,
                        EMC141XState::setTemperature, NULL, NULL);
    object_property_add(obj, "temperature3", "int",
                        EMC141XState::getTemperature,
                        EMC141XState::setTemperature, NULL, NULL);
}

void EMC141XState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    I2CSlaveClass *k = reinterpret_cast<I2CSlaveClass *>(klass);

    device_class_set_legacy_reset(dc, emc141x_reset);
    k->event = EMC141XState::event;
    k->recv = EMC141XState::rx;
    k->send = EMC141XState::tx;
    dc->vmsd = &vmstate_emc141x;
}

void EMC141XClass::emc1413ClassInit(ObjectClass *klass, const void *data)
{
    EMC141XClass *ec = reinterpret_cast<EMC141XClass *>(klass);

    EMC141XState::classInit(klass, data);
    ec->model = EMC1413_DEVICE_ID;
    ec->sensors_count = 3;
}

void EMC141XClass::emc1414ClassInit(ObjectClass *klass, const void *data)
{
    EMC141XClass *ec = reinterpret_cast<EMC141XClass *>(klass);

    EMC141XState::classInit(klass, data);
    ec->model = EMC1414_DEVICE_ID;
    ec->sensors_count = 4;
}

static const TypeInfo emc1413_info = {
    .name          = "emc1413",
    .parent        = TYPE_EMC141X,
    .class_init    = EMC141XClass::emc1413ClassInit,
};

static const TypeInfo emc1414_info = {
    .name          = "emc1414",
    .parent        = TYPE_EMC141X,
    .class_init    = EMC141XClass::emc1414ClassInit,
};

#include "qom/cpp/object.h"
/*
 * emc141x abstract base has instance_init; use manual registration with
 * trampoline_init so the init() member is called.
 */
static void EMC141XState_cpp_register_types(void)
{
    static TypeInfo info = {
        .name          = TYPE_EMC141X,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(EMC141XState),
        .instance_init = qemu_device_detail::trampoline_init<EMC141XState>,
        .is_abstract   = true,
        .class_size    = sizeof(EMC141XClass),
        .class_init    = qemu_device_detail::trampoline_class_init<EMC141XState>,
    };
    type_register_static(&info);
    type_register_static(&emc1413_info);
    type_register_static(&emc1414_info);
}

type_init(EMC141XState_cpp_register_types)
