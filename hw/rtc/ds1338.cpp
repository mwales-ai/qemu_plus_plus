/*
 * MAXIM DS1338 I2C RTC+NVRAM
 *
 * Copyright (c) 2009 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/bcd.h"
#include "qom/object.h"
#include "system/rtc.h"
#include "trace.h"
#include "qom/cpp/object.h"

/* Size of NVRAM including both the user-accessible area and the
 * secondary register area.
 */
#define NVRAM_SIZE 64

/* Flags definitions */
#define SECONDS_CH 0x80
#define HOURS_12   0x40
#define HOURS_PM   0x20
#define CTRL_OSF   0x20

#define TYPE_DS1338 "ds1338"
OBJECT_DECLARE_SIMPLE_TYPE(DS1338State, DS1338)

struct DS1338State {
    I2CSlave parent_obj;

    int64_t offset;
    uint8_t wday_offset;
    uint8_t nvram[NVRAM_SIZE];
    int32_t ptr;
    bool addr_byte;

    void captureCurrentTime()
    {
        /* Capture the current time into the secondary registers
         * which will be actually read by the data transfer operation.
         */
        struct tm now;
        qemu_get_timedate(&now, offset);
        nvram[0] = to_bcd(now.tm_sec);
        nvram[1] = to_bcd(now.tm_min);
        if (nvram[2] & HOURS_12) {
            int tmp = now.tm_hour;
            if (tmp % 12 == 0) {
                tmp += 12;
            }
            if (tmp <= 12) {
                nvram[2] = HOURS_12 | to_bcd(tmp);
            } else {
                nvram[2] = HOURS_12 | HOURS_PM | to_bcd(tmp - 12);
            }
        } else {
            nvram[2] = to_bcd(now.tm_hour);
        }
        nvram[3] = (now.tm_wday + wday_offset) % 7 + 1;
        nvram[4] = to_bcd(now.tm_mday);
        nvram[5] = to_bcd(now.tm_mon + 1);
        nvram[6] = to_bcd(now.tm_year - 100);
    }

    void incRegptr()
    {
        /* The register pointer wraps around after 0x3F; wraparound
         * causes the current time/date to be retransferred into
         * the secondary registers.
         */
        ptr = (ptr + 1) & (NVRAM_SIZE - 1);
        if (!ptr) {
            captureCurrentTime();
        }
    }

    static int event(I2CSlave *i2c, enum i2c_event event)
    {
        DS1338State *s = DS1338(i2c);

        switch (event) {
        case I2C_START_RECV:
            /* In h/w, capture happens on any START condition, not just a
             * START_RECV, but there is no need to actually capture on
             * START_SEND, because the guest can't get at that data
             * without going through a START_RECV which would overwrite it.
             */
            s->captureCurrentTime();
            break;
        case I2C_START_SEND:
            s->addr_byte = true;
            break;
        default:
            break;
        }

        return 0;
    }

    static uint8_t recv(I2CSlave *i2c)
    {
        DS1338State *s = DS1338(i2c);
        uint8_t res;

        res  = s->nvram[s->ptr];

        trace_ds1338_recv(s->ptr, res);

        s->incRegptr();
        return res;
    }

    static int send(I2CSlave *i2c, uint8_t data)
    {
        DS1338State *s = DS1338(i2c);

        trace_ds1338_send(s->ptr, data);

        if (s->addr_byte) {
            s->ptr = data & (NVRAM_SIZE - 1);
            s->addr_byte = false;
            return 0;
        }
        if (s->ptr < 7) {
            /* Time register. */
            struct tm now;
            qemu_get_timedate(&now, s->offset);
            switch(s->ptr) {
            case 0:
                /* TODO: Implement CH (stop) bit.  */
                now.tm_sec = from_bcd(data & 0x7f);
                break;
            case 1:
                now.tm_min = from_bcd(data & 0x7f);
                break;
            case 2:
                if (data & HOURS_12) {
                    int tmp = from_bcd(data & (HOURS_PM - 1));
                    if (data & HOURS_PM) {
                        tmp += 12;
                    }
                    if (tmp % 12 == 0) {
                        tmp -= 12;
                    }
                    now.tm_hour = tmp;
                } else {
                    now.tm_hour = from_bcd(data & (HOURS_12 - 1));
                }
                break;
            case 3:
                {
                    /* The day field is supposed to contain a value in
                       the range 1-7. Otherwise behavior is undefined.
                     */
                    int user_wday = (data & 7) - 1;
                    s->wday_offset = (user_wday - now.tm_wday + 7) % 7;
                }
                break;
            case 4:
                now.tm_mday = from_bcd(data & 0x3f);
                break;
            case 5:
                now.tm_mon = from_bcd(data & 0x1f) - 1;
                break;
            case 6:
                now.tm_year = from_bcd(data) + 100;
                break;
            }
            s->offset = qemu_timedate_diff(&now);
        } else if (s->ptr == 7) {
            /* Control register. */

            /* Ensure bits 2, 3 and 6 will read back as zero. */
            data &= 0xB3;

            /* Attempting to write the OSF flag to logic 1 leaves the
               value unchanged. */
            data = (data & ~CTRL_OSF) | (data & s->nvram[s->ptr] & CTRL_OSF);

            s->nvram[s->ptr] = data;
        } else {
            s->nvram[s->ptr] = data;
        }
        s->incRegptr();
        return 0;
    }

    void reset()
    {
        /* The clock is running and synchronized with the host */
        offset = 0;
        wday_offset = 0;
        memset(nvram, 0, NVRAM_SIZE);
        ptr = 0;
        addr_byte = false;
    }

    static void resetWrapper(DeviceState *dev)
    {
        DS1338State *s = DS1338(dev);
        s->reset();
    }

    static void classInit(ObjectClass *klass, const void *data);
};

static const VMStateDescription vmstate_ds1338 = {
    .name = "ds1338",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, DS1338State),
        VMSTATE_INT64(offset, DS1338State),
        VMSTATE_UINT8_V(wday_offset, DS1338State, 2),
        VMSTATE_UINT8_ARRAY(nvram, DS1338State, NVRAM_SIZE),
        VMSTATE_INT32(ptr, DS1338State),
        VMSTATE_BOOL(addr_byte, DS1338State),
        VMSTATE_END_OF_LIST()
    }
};

/* Subclass struct for virtual method overrides */
struct DS1338Class : I2CSlaveClass {
    int event(I2CSlave *s, enum i2c_event ev) override;
    uint8_t recv(I2CSlave *s) override;
    int send(I2CSlave *s, uint8_t data) override;
};

int DS1338Class::event(I2CSlave *s, enum i2c_event ev) { return DS1338State::event(s, ev); }
uint8_t DS1338Class::recv(I2CSlave *s) { return DS1338State::recv(s); }
int DS1338Class::send(I2CSlave *s, uint8_t data) { return DS1338State::send(s, data); }

void DS1338State::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    qom_fixup_vtable<DS1338Class>(klass);

    device_class_set_legacy_reset(dc, resetWrapper);
    dc->vmsd = &vmstate_ds1338;
}

static const TypeInfo ds1338_types[] = {
    {
        .name          = TYPE_DS1338,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(DS1338State),
        .class_size    = sizeof(DS1338Class),
        .class_init    = DS1338State::classInit,
    },
};

DEFINE_TYPES(ds1338_types)
