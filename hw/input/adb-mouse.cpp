/*
 * QEMU ADB mouse support
 *
 * Copyright (c) 2004 Fabrice Bellard
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
#include "ui/console.h"
#include "hw/input/adb.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "adb-internal.h"
#include "trace.h"
#include "qom/object.h"

OBJECT_DECLARE_TYPE(MouseState, ADBMouseClass, ADB_MOUSE)

struct MouseState {
    /*< public >*/
    ADBDevice parent_obj;
    /*< private >*/

    QemuInputHandlerState *hs;
    int buttons_state, last_buttons_state;
    int dx, dy, dz;

    /* Static callbacks */
    static void handleEvent(DeviceState *dev, QemuConsole *src,
                            InputEvent *evt);

    /* Instance methods */
    int poll(ADBDevice *d, uint8_t *obuf);
    int request(ADBDevice *d, uint8_t *obuf, const uint8_t *buf, int len);
    bool hasData(ADBDevice *d);
    void reset(DeviceState *dev);
    void realize(DeviceState *dev, Error **errp);
    void initfn(Object *obj);

    /* Class init */
    static void classInit(ObjectClass *oc, const void *data);
};


struct ADBMouseClass {
    /*< public >*/
    ADBDeviceClass parent_class;
    /*< private >*/

    DeviceRealize parent_realize;
};

#define ADB_MOUSE_BUTTON_LEFT   0x01
#define ADB_MOUSE_BUTTON_RIGHT  0x02

void MouseState::handleEvent(DeviceState *dev, QemuConsole *src,
                              InputEvent *evt)
{
    MouseState *s = (MouseState *)dev;
    InputMoveEvent *move;
    InputBtnEvent *btn;
    static int bmap[INPUT_BUTTON__MAX] = {};
    static bool bmap_init = false;
    if (!bmap_init) {
        bmap[INPUT_BUTTON_LEFT]   = ADB_MOUSE_BUTTON_LEFT;
        bmap[INPUT_BUTTON_RIGHT]  = ADB_MOUSE_BUTTON_RIGHT;
        bmap_init = true;
    }

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        move = evt->u.rel.data;
        if (move->axis == INPUT_AXIS_X) {
            s->dx += move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->dy += move->value;
        }
        break;

    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (bmap[btn->button]) {
            if (btn->down) {
                s->buttons_state |= bmap[btn->button];
            } else {
                s->buttons_state &= ~bmap[btn->button];
            }
        }
        break;

    default:
        /* keep gcc happy */
        break;
    }
}

static const QemuInputHandler adb_mouse_handler = {
    .name  = "QEMU ADB Mouse",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = MouseState::handleEvent,
    /*
     * We do not need the .sync handler because unlike e.g. PS/2 where async
     * mouse events are sent over the serial port, an ADB mouse is constantly
     * polled by the host via the adb_mouse_poll() callback.
     */
};

int MouseState::poll(ADBDevice *d, uint8_t *obuf)
{
    MouseState *s = ADB_MOUSE(d);
    int dx, dy;

    if (s->last_buttons_state == s->buttons_state &&
        s->dx == 0 && s->dy == 0) {
        return 0;
    }

    dx = s->dx;
    if (dx < -63) {
        dx = -63;
    } else if (dx > 63) {
        dx = 63;
    }

    dy = s->dy;
    if (dy < -63) {
        dy = -63;
    } else if (dy > 63) {
        dy = 63;
    }

    s->dx -= dx;
    s->dy -= dy;
    s->last_buttons_state = s->buttons_state;

    dx &= 0x7f;
    dy &= 0x7f;

    if (!(s->buttons_state & ADB_MOUSE_BUTTON_LEFT)) {
        dy |= 0x80;
    }
    if (!(s->buttons_state & ADB_MOUSE_BUTTON_RIGHT)) {
        dx |= 0x80;
    }

    obuf[0] = dy;
    obuf[1] = dx;
    return 2;
}

static int adb_mouse_poll(ADBDevice *d, uint8_t *obuf)
{
    MouseState *s = ADB_MOUSE(d);
    return s->poll(d, obuf);
}

int MouseState::request(ADBDevice *d, uint8_t *obuf,
                         const uint8_t *buf, int len)
{
    MouseState *s = ADB_MOUSE(d);
    int cmd, reg, olen;

    if ((buf[0] & 0x0f) == ADB_FLUSH) {
        /* flush mouse fifo */
        s->buttons_state = s->last_buttons_state;
        s->dx = 0;
        s->dy = 0;
        s->dz = 0;
        trace_adb_device_mouse_flush();
        return 0;
    }

    cmd = buf[0] & 0xc;
    reg = buf[0] & 0x3;
    olen = 0;
    switch (cmd) {
    case ADB_WRITEREG:
        trace_adb_device_mouse_writereg(reg, buf[1]);
        switch (reg) {
        case 2:
            break;
        case 3:
            /*
             * MacOS 9 has a bug in its ADB driver whereby after configuring
             * the ADB bus devices it sends another write of invalid length
             * to reg 3. Make sure we ignore it to prevent an address clash
             * with the previous device.
             */
            if (len != 3) {
                return 0;
            }

            switch (buf[2]) {
            case ADB_CMD_SELF_TEST:
                break;
            case ADB_CMD_CHANGE_ID:
            case ADB_CMD_CHANGE_ID_AND_ACT:
            case ADB_CMD_CHANGE_ID_AND_ENABLE:
                d->devaddr = buf[1] & 0xf;
                trace_adb_device_mouse_request_change_addr(d->devaddr);
                break;
            default:
                d->devaddr = buf[1] & 0xf;
                /*
                 * we support handlers:
                 * 0x01: Classic Apple Mouse Protocol / 100 cpi operations
                 * 0x02: Classic Apple Mouse Protocol / 200 cpi operations
                 * we don't support handlers (at least):
                 * 0x03: Mouse systems A3 trackball
                 * 0x04: Extended Apple Mouse Protocol
                 * 0x2f: Microspeed mouse
                 * 0x42: Macally
                 * 0x5f: Microspeed mouse
                 * 0x66: Microspeed mouse
                 */
                if (buf[2] == 1 || buf[2] == 2) {
                    d->handler = buf[2];
                }

                trace_adb_device_mouse_request_change_addr_and_handler(
                    d->devaddr, d->handler);
                break;
            }
        }
        break;
    case ADB_READREG:
        switch (reg) {
        case 0:
            olen = adb_mouse_poll(d, obuf);
            break;
        case 1:
            break;
        case 3:
            obuf[0] = d->devaddr;
            obuf[1] = d->handler;
            olen = 2;
            break;
        }
        trace_adb_device_mouse_readreg(reg, obuf[0], obuf[1]);
        break;
    }
    return olen;
}

static int adb_mouse_request(ADBDevice *d, uint8_t *obuf,
                             const uint8_t *buf, int len)
{
    MouseState *s = ADB_MOUSE(d);
    return s->request(d, obuf, buf, len);
}

bool MouseState::hasData(ADBDevice *d)
{
    MouseState *s = ADB_MOUSE(d);

    return !(s->last_buttons_state == s->buttons_state &&
             s->dx == 0 && s->dy == 0);
}

static bool adb_mouse_has_data(ADBDevice *d)
{
    MouseState *s = ADB_MOUSE(d);
    return s->hasData(d);
}

void MouseState::reset(DeviceState *dev)
{
    ADBDevice *d = ADB_DEVICE(dev);
    MouseState *s = ADB_MOUSE(dev);

    d->handler = 2;
    d->devaddr = ADB_DEVID_MOUSE;
    s->last_buttons_state = s->buttons_state = 0;
    s->dx = s->dy = s->dz = 0;
}

static void adb_mouse_reset(DeviceState *dev)
{
    MouseState *s = ADB_MOUSE(dev);
    s->reset(dev);
}

static const VMStateDescription vmstate_adb_mouse = {
    .name = "adb_mouse",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, MouseState, 0, vmstate_adb_device,
                       ADBDevice),
        VMSTATE_INT32(buttons_state, MouseState),
        VMSTATE_INT32(last_buttons_state, MouseState),
        VMSTATE_INT32(dx, MouseState),
        VMSTATE_INT32(dy, MouseState),
        VMSTATE_INT32(dz, MouseState),
        VMSTATE_END_OF_LIST()
    }
};

void MouseState::realize(DeviceState *dev, Error **errp)
{
    MouseState *s = ADB_MOUSE(dev);
    ADBMouseClass *amc = ADB_MOUSE_GET_CLASS(dev);

    amc->parent_realize(dev, errp);

    s->hs = qemu_input_handler_register(dev, &adb_mouse_handler);
}

static void adb_mouse_realizefn(DeviceState *dev, Error **errp)
{
    MouseState *s = ADB_MOUSE(dev);
    s->realize(dev, errp);
}

void MouseState::initfn(Object *obj)
{
    ADBDevice *d = ADB_DEVICE(obj);

    d->devaddr = ADB_DEVID_MOUSE;
}

static void adb_mouse_initfn(Object *obj)
{
    MouseState *s = ADB_MOUSE(obj);
    s->initfn(obj);
}

void MouseState::classInit(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ADBDeviceClass *adc = ADB_DEVICE_CLASS(oc);
    ADBMouseClass *amc = ADB_MOUSE_CLASS(oc);

    device_class_set_parent_realize(dc, adb_mouse_realizefn,
                                    &amc->parent_realize);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    adc->devreq = adb_mouse_request;
    adc->devhasdata = adb_mouse_has_data;
    device_class_set_legacy_reset(dc, adb_mouse_reset);
    dc->vmsd = &vmstate_adb_mouse;
}

static const TypeInfo adb_mouse_type_info = {
    .name = TYPE_ADB_MOUSE,
    .parent = TYPE_ADB_DEVICE,
    .instance_size = sizeof(MouseState),
    .instance_init = adb_mouse_initfn,
    .class_size = sizeof(ADBMouseClass),
    .class_init = MouseState::classInit,
};

static void adb_mouse_register_types(void)
{
    type_register_static(&adb_mouse_type_info);
}

type_init(adb_mouse_register_types)
