/*
 * M41T80 serial rtc emulation
 *
 * Copyright (c) 2018 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bcd.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "system/rtc.h"
#include "qom/cpp/object.h"

#define TYPE_M41T80 "m41t80"
OBJECT_DECLARE_SIMPLE_TYPE(M41t80State, M41T80)

struct M41t80State {
    I2CSlave parent_obj;
    int8_t addr;

    void realize(Error **errp)
    {
        addr = -1;
    }

    int send(uint8_t data)
    {
        if (addr < 0) {
            addr = data;
        } else {
            addr++;
        }
        return 0;
    }

    uint8_t recv()
    {
        struct tm now;
        int64_t rt;

        if (addr < 0) {
            addr = 0;
        }
        if (addr >= 1 && addr <= 7) {
            qemu_get_timedate(&now, -1);
        }
        switch (addr++) {
        case 0:
            rt = g_get_real_time();
            return to_bcd((rt % G_USEC_PER_SEC) / 10000);
        case 1:
            return to_bcd(now.tm_sec);
        case 2:
            return to_bcd(now.tm_min);
        case 3:
            return to_bcd(now.tm_hour);
        case 4:
            return to_bcd(now.tm_wday);
        case 5:
            return to_bcd(now.tm_mday);
        case 6:
            return to_bcd(now.tm_mon + 1);
        case 7:
            return to_bcd(now.tm_year % 100);
        case 8 ... 19:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented register: %d\n",
                          __func__, addr - 1);
            return 0;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid register: %d\n",
                          __func__, addr - 1);
            return 0;
        }
    }

    int event(enum i2c_event event)
    {
        if (event == I2C_START_SEND) {
            addr = -1;
        }
        return 0;
    }

    static int i2cSend(I2CSlave *i2c, uint8_t data)
    {
        M41t80State *s = M41T80(i2c);
        return s->send(data);
    }

    static uint8_t i2cRecv(I2CSlave *i2c)
    {
        M41t80State *s = M41T80(i2c);
        return s->recv();
    }

    static int i2cEvent(I2CSlave *i2c, enum i2c_event event)
    {
        M41t80State *s = M41T80(i2c);
        return s->event(event);
    }

    static void classInit(DeviceClass *dc)
    {
        I2CSlaveClass *sc = I2C_SLAVE_CLASS(dc);

        sc->send = i2cSend;
        sc->recv = i2cRecv;
        sc->event = i2cEvent;
    }
};

REGISTER_QEMU_DEVICE(M41t80State, TYPE_M41T80, TYPE_I2C_SLAVE)
