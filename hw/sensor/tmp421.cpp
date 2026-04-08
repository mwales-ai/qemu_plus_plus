/*
 * Texas Instruments TMP421 temperature sensor.
 *
 * Copyright (c) 2016 IBM Corporation.
 *
 * Largely inspired by :
 *
 * Texas Instruments TMP105 temperature sensor.
 *
 * Copyright (C) 2008 Nokia Corporation
 * Written by Andrzej Zaborowski <andrew@openedhand.com>
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

/* Manufacturer / Device ID's */
#define TMP421_MANUFACTURER_ID          0x55
#define TMP421_DEVICE_ID                0x21
#define TMP422_DEVICE_ID                0x22
#define TMP423_DEVICE_ID                0x23

typedef struct DeviceInfo {
    int model;
    const char *name;
} DeviceInfo;

static const DeviceInfo devices[] = {
    { TMP421_DEVICE_ID, "tmp421" },
    { TMP422_DEVICE_ID, "tmp422" },
    { TMP423_DEVICE_ID, "tmp423" },
};

struct TMP421State {
    /*< private >*/
    I2CSlave i2c;
    /*< public >*/

    int16_t temperature[4];

    uint8_t status;
    uint8_t config[2];
    uint8_t rate;

    uint8_t len;
    uint8_t buf[2];
    uint8_t pointer;

    static void getTemperature(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp);
    static void setTemperature(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp);
    void readRegs();
    void writeRegs();
    static uint8_t rx(I2CSlave *i2c);
    static int tx(I2CSlave *i2c, uint8_t data);
    static int event(I2CSlave *i2c, enum i2c_event event);
    void reset();
    void realize(Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
};

struct TMP421Class {
    I2CSlaveClass parent_class;
    const DeviceInfo *dev;
};

#define TYPE_TMP421 "tmp421-generic"
OBJECT_DECLARE_TYPE(TMP421State, TMP421Class, TMP421)


/* the TMP421 registers */
#define TMP421_STATUS_REG               0x08
#define    TMP421_STATUS_BUSY             (1 << 7)
#define TMP421_CONFIG_REG_1             0x09
#define    TMP421_CONFIG_RANGE            (1 << 2)
#define    TMP421_CONFIG_SHUTDOWN         (1 << 6)
#define TMP421_CONFIG_REG_2             0x0A
#define    TMP421_CONFIG_RC               (1 << 2)
#define    TMP421_CONFIG_LEN              (1 << 3)
#define    TMP421_CONFIG_REN              (1 << 4)
#define    TMP421_CONFIG_REN2             (1 << 5)
#define    TMP421_CONFIG_REN3             (1 << 6)

#define TMP421_CONVERSION_RATE_REG      0x0B
#define TMP421_ONE_SHOT                 0x0F

#define TMP421_RESET                    0xFC
#define TMP421_MANUFACTURER_ID_REG      0xFE
#define TMP421_DEVICE_ID_REG            0xFF

#define TMP421_TEMP_MSB0                0x00
#define TMP421_TEMP_MSB1                0x01
#define TMP421_TEMP_MSB2                0x02
#define TMP421_TEMP_MSB3                0x03
#define TMP421_TEMP_LSB0                0x10
#define TMP421_TEMP_LSB1                0x11
#define TMP421_TEMP_LSB2                0x12
#define TMP421_TEMP_LSB3                0x13

static const int32_t mins[2] = { -40000, -55000 };
static const int32_t maxs[2] = { 127000, 150000 };

void TMP421State::getTemperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    TMP421State *s = reinterpret_cast<TMP421State *>(obj);
    bool ext_range = (s->config[0] & TMP421_CONFIG_RANGE);
    int offset = ext_range * 64 * 256;
    int64_t value;
    int tempid;

    if (sscanf(name, "temperature%d", &tempid) != 1) {
        error_setg(errp, "error reading %s: %s", name, g_strerror(errno));
        return;
    }

    if (tempid >= 4 || tempid < 0) {
        error_setg(errp, "error reading %s", name);
        return;
    }

    value = ((s->temperature[tempid] - offset) * 1000 + 128) / 256;

    visit_type_int(v, name, &value, errp);
}

/* Units are 0.001 centigrades relative to 0 C.  s->temperature is 8.8
 * fixed point, so units are 1/256 centigrades.  A simple ratio will do.
 */
void TMP421State::setTemperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    TMP421State *s = reinterpret_cast<TMP421State *>(obj);
    int64_t temp;
    bool ext_range = (s->config[0] & TMP421_CONFIG_RANGE);
    int offset = ext_range * 64 * 256;
    int tempid;

    if (!visit_type_int(v, name, &temp, errp)) {
        return;
    }

    if (temp >= maxs[ext_range] || temp < mins[ext_range]) {
        error_setg(errp, "value %" PRId64 ".%03" PRIu64 " C is out of range",
                   temp / 1000, temp % 1000);
        return;
    }

    if (sscanf(name, "temperature%d", &tempid) != 1) {
        error_setg(errp, "error reading %s: %s", name, g_strerror(errno));
        return;
    }

    if (tempid >= 4 || tempid < 0) {
        error_setg(errp, "error reading %s", name);
        return;
    }

    s->temperature[tempid] = (int16_t) ((temp * 256 - 128) / 1000) + offset;
}

void TMP421State::readRegs()
{
    TMP421Class *sc = TMP421_GET_CLASS(this);

    len = 0;

    switch (pointer) {
    case TMP421_MANUFACTURER_ID_REG:
        buf[len++] = TMP421_MANUFACTURER_ID;
        break;
    case TMP421_DEVICE_ID_REG:
        buf[len++] = sc->dev->model;
        break;
    case TMP421_CONFIG_REG_1:
        buf[len++] = config[0];
        break;
    case TMP421_CONFIG_REG_2:
        buf[len++] = config[1];
        break;
    case TMP421_CONVERSION_RATE_REG:
        buf[len++] = rate;
        break;
    case TMP421_STATUS_REG:
        buf[len++] = status;
        break;

        /* FIXME: check for channel enablement in config registers */
    case TMP421_TEMP_MSB0:
        buf[len++] = (((uint16_t) temperature[0]) >> 8);
        buf[len++] = (((uint16_t) temperature[0]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_MSB1:
        buf[len++] = (((uint16_t) temperature[1]) >> 8);
        buf[len++] = (((uint16_t) temperature[1]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_MSB2:
        buf[len++] = (((uint16_t) temperature[2]) >> 8);
        buf[len++] = (((uint16_t) temperature[2]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_MSB3:
        buf[len++] = (((uint16_t) temperature[3]) >> 8);
        buf[len++] = (((uint16_t) temperature[3]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_LSB0:
        buf[len++] = (((uint16_t) temperature[0]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_LSB1:
        buf[len++] = (((uint16_t) temperature[1]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_LSB2:
        buf[len++] = (((uint16_t) temperature[2]) >> 0) & 0xf0;
        break;
    case TMP421_TEMP_LSB3:
        buf[len++] = (((uint16_t) temperature[3]) >> 0) & 0xf0;
        break;
    }
}

void TMP421State::writeRegs()
{
    switch (pointer) {
    case TMP421_CONVERSION_RATE_REG:
        rate = buf[0];
        break;
    case TMP421_CONFIG_REG_1:
        config[0] = buf[0];
        break;
    case TMP421_CONFIG_REG_2:
        config[1] = buf[0];
        break;
    case TMP421_RESET:
        reset();
        break;
    }
}

uint8_t TMP421State::rx(I2CSlave *i2c)
{
    TMP421State *s = reinterpret_cast<TMP421State *>(i2c);

    if (s->len < 2) {
        return s->buf[s->len++];
    } else {
        return 0xff;
    }
}

int TMP421State::tx(I2CSlave *i2c, uint8_t data)
{
    TMP421State *s = reinterpret_cast<TMP421State *>(i2c);

    if (s->len == 0) {
        /* first byte is the register pointer for a read or write
         * operation */
        s->pointer = data;
        s->len++;
    } else if (s->len == 1) {
        /* second byte is the data to write. The device only supports
         * one byte writes */
        s->buf[0] = data;
        s->writeRegs();
    }

    return 0;
}

int TMP421State::event(I2CSlave *i2c, enum i2c_event event)
{
    TMP421State *s = reinterpret_cast<TMP421State *>(i2c);

    if (event == I2C_START_RECV) {
        s->readRegs();
    }

    s->len = 0;
    return 0;
}

static const VMStateDescription vmstate_tmp421 = {
    .name = "TMP421",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(len, TMP421State),
        VMSTATE_UINT8_ARRAY(buf, TMP421State, 2),
        VMSTATE_UINT8(pointer, TMP421State),
        VMSTATE_UINT8_ARRAY(config, TMP421State, 2),
        VMSTATE_UINT8(status, TMP421State),
        VMSTATE_UINT8(rate, TMP421State),
        VMSTATE_INT16_ARRAY(temperature, TMP421State, 4),
        VMSTATE_I2C_SLAVE(i2c, TMP421State),
        VMSTATE_END_OF_LIST()
    }
};

void TMP421State::reset()
{
    TMP421Class *sc = TMP421_GET_CLASS(this);

    memset(temperature, 0, sizeof(temperature));
    pointer = 0;

    config[0] = 0; /* TMP421_CONFIG_RANGE */

     /* resistance correction and channel enablement */
    switch (sc->dev->model) {
    case TMP421_DEVICE_ID:
        config[1] = 0x1c;
        break;
    case TMP422_DEVICE_ID:
        config[1] = 0x3c;
        break;
    case TMP423_DEVICE_ID:
        config[1] = 0x7c;
        break;
    }

    rate = 0x7;       /* 8Hz */
    status = 0;
}

void TMP421State::realize(Error **errp)
{
    reset();
}

static void tmp421_realize_wrapper(DeviceState *dev, Error **errp)
{
    TMP421State *s = reinterpret_cast<TMP421State *>(dev);
    s->realize(errp);
}

void TMP421State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    I2CSlaveClass *k = reinterpret_cast<I2CSlaveClass *>(klass);
    TMP421Class *sc = TMP421_CLASS(klass);

    dc->realize = tmp421_realize_wrapper;
    k->event = TMP421State::event;
    k->recv = TMP421State::rx;
    k->send = TMP421State::tx;
    dc->vmsd = &vmstate_tmp421;
    sc->dev = (DeviceInfo *) data;

    object_class_property_add(klass, "temperature0", "int",
                              TMP421State::getTemperature,
                              TMP421State::setTemperature, NULL, NULL);
    object_class_property_add(klass, "temperature1", "int",
                              TMP421State::getTemperature,
                              TMP421State::setTemperature, NULL, NULL);
    object_class_property_add(klass, "temperature2", "int",
                              TMP421State::getTemperature,
                              TMP421State::setTemperature, NULL, NULL);
    object_class_property_add(klass, "temperature3", "int",
                              TMP421State::getTemperature,
                              TMP421State::setTemperature, NULL, NULL);
}

static const TypeInfo tmp421_info = {
    .name          = TYPE_TMP421,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TMP421State),
    .is_abstract   = true,
    .class_size    = sizeof(TMP421Class),
};

static void tmp421_register_types(void)
{
    int i;

    type_register_static(&tmp421_info);
    for (i = 0; i < ARRAY_SIZE(devices); ++i) {
        TypeInfo ti = {
            .name       = devices[i].name,
            .parent     = TYPE_TMP421,
            .class_init = TMP421State::classInit,
            .class_data = &devices[i],
        };
        type_register_static(&ti);
    }
}

type_init(tmp421_register_types)
