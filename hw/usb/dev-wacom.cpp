/*
 * Wacom PenPartner USB tablet emulation.
 *
 * Copyright (c) 2006 Openedhand Ltd.
 * Author: Andrzej Zaborowski <balrog@zabor.org>
 *
 * Based on hw/usb-hid.c:
 * Copyright (c) 2005 Fabrice Bellard
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
#include "hw/usb.h"
#include "hw/usb/hid.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "desc.h"
#include "qom/object.h"

/* Interface requests */
#define WACOM_GET_REPORT    0x2101
#define WACOM_SET_REPORT    0x2109

enum WacomMode {
    WACOM_MODE_HID = 1,
    WACOM_MODE_WACOM = 2,
};

struct USBWacomState {
    USBDevice dev;
    USBEndpoint *intr;
    QEMUPutMouseEntry *eh_entry;
    int dx, dy, dz, buttons_state;
    int x, y;
    int mouse_grabbed;
    enum WacomMode mode;
    uint8_t idle;
    int changed;

    /* methods */
    int mousePoll(uint8_t *buf, int len);
    int wacomPoll(uint8_t *buf, int len);

    /* static callbacks */
    static void mouseEvent(void *opaque,
                           int dx1, int dy1, int dz1, int buttons_state);
    static void wacomEvent(void *opaque,
                           int x, int y, int dz, int buttons_state);
    static void handleReset(USBDevice *dev);
    static void handleControl(USBDevice *dev, USBPacket *p,
                              int request, int value, int index,
                              int length, uint8_t *data);
    static void handleData(USBDevice *dev, USBPacket *p);
    static void realize(USBDevice *dev, Error **errp);
    static void unrealize(USBDevice *dev);
    static void classInit(DeviceClass *dc);
};

#define TYPE_USB_WACOM "usb-wacom-tablet"
OBJECT_DECLARE_SIMPLE_TYPE(USBWacomState, USB_WACOM)

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

static const USBDescStrings desc_strings = {
    NULL,                   /* 0 */
    "QEMU",                /* STR_MANUFACTURER */
    "Wacom PenPartner",    /* STR_PRODUCT */
    "1",                   /* STR_SERIALNUMBER */
};

static const uint8_t qemu_wacom_hid_report_descriptor[] = {
    0x05, 0x01,      /* Usage Page (Desktop) */
    0x09, 0x02,      /* Usage (Mouse) */
    0xa1, 0x01,      /* Collection (Application) */
    0x85, 0x01,      /*    Report ID (1) */
    0x09, 0x01,      /*    Usage (Pointer) */
    0xa1, 0x00,      /*    Collection (Physical) */
    0x05, 0x09,      /*       Usage Page (Button) */
    0x19, 0x01,      /*       Usage Minimum (01h) */
    0x29, 0x03,      /*       Usage Maximum (03h) */
    0x15, 0x00,      /*       Logical Minimum (0) */
    0x25, 0x01,      /*       Logical Maximum (1) */
    0x95, 0x03,      /*       Report Count (3) */
    0x75, 0x01,      /*       Report Size (1) */
    0x81, 0x02,      /*       Input (Data, Variable, Absolute) */
    0x95, 0x01,      /*       Report Count (1) */
    0x75, 0x05,      /*       Report Size (5) */
    0x81, 0x01,      /*       Input (Constant) */
    0x05, 0x01,      /*       Usage Page (Desktop) */
    0x09, 0x30,      /*       Usage (X) */
    0x09, 0x31,      /*       Usage (Y) */
    0x09, 0x38,      /*       Usage (Wheel) */
    0x15, 0x81,      /*       Logical Minimum (-127) */
    0x25, 0x7f,      /*       Logical Maximum (127) */
    0x75, 0x08,      /*       Report Size (8) */
    0x95, 0x03,      /*       Report Count (3) */
    0x81, 0x06,      /*       Input (Data, Variable, Relative) */
    0x95, 0x03,      /*       Report Count (3) */
    0x81, 0x01,      /*       Input (Constant) */
    0xc0,            /*    End Collection */
    0xc0,            /* End Collection */
    0x05, 0x0d,      /* Usage Page (Digitizer) */
    0x09, 0x01,      /* Usage (Digitizer) */
    0xa1, 0x01,      /* Collection (Application) */
    0x85, 0x02,      /*    Report ID (2) */
    0xa1, 0x00,      /*    Collection (Physical) */
    0x06, 0x00, 0xff,/*       Usage Page (ff00h), vendor-defined */
    0x09, 0x01,      /*       Usage (01h) */
    0x15, 0x00,      /*       Logical Minimum (0) */
    0x26, 0xff, 0x00,/*       Logical Maximum (255) */
    0x75, 0x08,      /*       Report Size (8) */
    0x95, 0x07,      /*       Report Count (7) */
    0x81, 0x02,      /*       Input (Data, Variable, Absolute) */
    0xc0,            /*    End Collection */
    0x09, 0x01,      /*    Usage (01h) */
    0x85, 0x63,      /*    Report ID (99) */
    0x95, 0x07,      /*    Report Count (7) */
    0x81, 0x02,      /*    Input (Data, Variable, Absolute) */
    0x09, 0x01,      /*    Usage (01h) */
    0x85, 0x02,      /*    Report ID (2) */
    0x95, 0x01,      /*    Report Count (1) */
    0xb1, 0x02,      /*    Feature (Variable) */
    0x09, 0x01,      /*    Usage (01h) */
    0x85, 0x03,      /*    Report ID (3) */
    0x95, 0x01,      /*    Report Count (1) */
    0xb1, 0x02,      /*    Feature (Variable) */
    0xc0             /* End Collection */
};

static const uint8_t desc_wacom_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x01, 0x10,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    sizeof(qemu_wacom_hid_report_descriptor), 0, /*  u16 len */
};

static USBDescOther desc_wacom_descs[] = {
    {
        /* HID descriptor */
        .data = desc_wacom_hid_data,
    },
};

static USBDescEndpoint desc_wacom_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 8,
        .bInterval             = 0x0a,
    },
};

static const USBDescIface desc_iface_wacom = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = desc_wacom_descs,
    .eps = desc_wacom_eps,
};

static const USBDescConfig desc_confs_wacom[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .bmAttributes          = USB_CFG_ATT_ONE,
        .bMaxPower             = 40,
        .nif = 1,
        .ifs = &desc_iface_wacom,
    },
};

static const USBDescDevice desc_device_wacom = {
    .bcdUSB                        = 0x0110,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = desc_confs_wacom,
};

static const USBDesc desc_wacom = {
    .id = {
        .idVendor          = 0x056a,
        .idProduct         = 0x0000,
        .bcdDevice         = 0x4210,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device_wacom,
    .str  = desc_strings,
};

void USBWacomState::mouseEvent(void *opaque,
                               int dx1, int dy1, int dz1, int buttons_state)
{
    USBWacomState *s = static_cast<USBWacomState *>(opaque);

    s->dx += dx1;
    s->dy += dy1;
    s->dz += dz1;
    s->buttons_state = buttons_state;
    s->changed = 1;
    usb_wakeup(s->intr, 0);
}

void USBWacomState::wacomEvent(void *opaque,
                               int x, int y, int dz, int buttons_state)
{
    USBWacomState *s = static_cast<USBWacomState *>(opaque);

    /* scale to Penpartner resolution */
    s->x = (x * 5040 / 0x7FFF);
    s->y = (y * 3780 / 0x7FFF);
    s->dz += dz;
    s->buttons_state = buttons_state;
    s->changed = 1;
    usb_wakeup(s->intr, 0);
}

static inline int int_clamp(int val, int vmin, int vmax)
{
    if (val < vmin)
        return vmin;
    else if (val > vmax)
        return vmax;
    else
        return val;
}

int USBWacomState::mousePoll(uint8_t *buf, int len)
{
    int dx_val, dy_val, dz_val, b, l;

    if (!mouse_grabbed) {
        eh_entry = qemu_add_mouse_event_handler(USBWacomState::mouseEvent,
                                                this, 0,
                                                "QEMU PenPartner tablet");
        qemu_activate_mouse_event_handler(eh_entry);
        mouse_grabbed = 1;
    }

    dx_val = int_clamp(dx, -128, 127);
    dy_val = int_clamp(dy, -128, 127);
    dz_val = int_clamp(dz, -128, 127);

    dx -= dx_val;
    dy -= dy_val;
    dz -= dz_val;

    b = 0;
    if (buttons_state & MOUSE_EVENT_LBUTTON)
        b |= 0x01;
    if (buttons_state & MOUSE_EVENT_RBUTTON)
        b |= 0x02;
    if (buttons_state & MOUSE_EVENT_MBUTTON)
        b |= 0x04;

    buf[0] = b;
    buf[1] = dx_val;
    buf[2] = dy_val;
    l = 3;
    if (len >= 4) {
        buf[3] = dz_val;
        l = 4;
    }
    return l;
}

int USBWacomState::wacomPoll(uint8_t *buf, int len)
{
    int b;

    if (!mouse_grabbed) {
        eh_entry = qemu_add_mouse_event_handler(USBWacomState::wacomEvent,
                                                this, 1,
                                                "QEMU PenPartner tablet");
        qemu_activate_mouse_event_handler(eh_entry);
        mouse_grabbed = 1;
    }

    b = 0;
    if (buttons_state & MOUSE_EVENT_LBUTTON)
        b |= 0x01;
    if (buttons_state & MOUSE_EVENT_RBUTTON)
        b |= 0x40;
    if (buttons_state & MOUSE_EVENT_MBUTTON)
        b |= 0x20; /* eraser */

    if (len < 7)
        return 0;

    buf[0] = mode;
    buf[5] = 0x00 | (b & 0xf0);
    buf[1] = x & 0xff;
    buf[2] = x >> 8;
    buf[3] = y & 0xff;
    buf[4] = y >> 8;
    if (b & 0x3f) {
        buf[6] = 0;
    } else {
        buf[6] = (unsigned char) -127;
    }

    return 7;
}

void USBWacomState::handleReset(USBDevice *dev)
{
    USBWacomState *s = reinterpret_cast<USBWacomState *>(dev);

    s->dx = 0;
    s->dy = 0;
    s->dz = 0;
    s->x = 0;
    s->y = 0;
    s->buttons_state = 0;
    s->mode = WACOM_MODE_HID;
}

void USBWacomState::handleControl(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBWacomState *s = reinterpret_cast<USBWacomState *>(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        switch (value >> 8) {
        case 0x22:
                memcpy(data, qemu_wacom_hid_report_descriptor,
                       sizeof(qemu_wacom_hid_report_descriptor));
                p->actual_length = sizeof(qemu_wacom_hid_report_descriptor);
            break;
        default:
            return;
        }
        break;
    case WACOM_SET_REPORT:
        if (s->mouse_grabbed) {
            qemu_remove_mouse_event_handler(s->eh_entry);
            s->mouse_grabbed = 0;
        }
        s->mode = static_cast<WacomMode>(data[0]);
        break;
    case WACOM_GET_REPORT:
        data[0] = 0;
        data[1] = s->mode;
        p->actual_length = 2;
        break;
    /* USB HID requests */
    case HID_GET_REPORT:
        if (s->mode == WACOM_MODE_HID)
            p->actual_length = s->mousePoll(data, length);
        else if (s->mode == WACOM_MODE_WACOM)
            p->actual_length = s->wacomPoll(data, length);
        break;
    case HID_GET_IDLE:
        data[0] = s->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        s->idle = (uint8_t) (value >> 8);
        break;
    default:
        p->status = USB_RET_STALL;
        break;
    }
}

void USBWacomState::handleData(USBDevice *dev, USBPacket *p)
{
    USBWacomState *s = reinterpret_cast<USBWacomState *>(dev);
    g_autofree uint8_t *buf = static_cast<uint8_t *>(g_malloc(p->iov.size));
    int len = 0;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            if (!(s->changed || s->idle)) {
                p->status = USB_RET_NAK;
                return;
            }
            s->changed = 0;
            if (s->mode == WACOM_MODE_HID)
                len = s->mousePoll(buf, p->iov.size);
            else if (s->mode == WACOM_MODE_WACOM)
                len = s->wacomPoll(buf, p->iov.size);
            usb_packet_copy(p, buf, len);
            break;
        }
        /* Fall through.  */
    case USB_TOKEN_OUT:
    default:
        p->status = USB_RET_STALL;
    }
}

void USBWacomState::unrealize(USBDevice *dev)
{
    USBWacomState *s = reinterpret_cast<USBWacomState *>(dev);

    if (s->mouse_grabbed) {
        qemu_remove_mouse_event_handler(s->eh_entry);
        s->mouse_grabbed = 0;
    }
}

void USBWacomState::realize(USBDevice *dev, Error **errp)
{
    USBWacomState *s = reinterpret_cast<USBWacomState *>(dev);
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->changed = 1;
}

static const VMStateDescription vmstate_usb_wacom = {
    .name = "usb-wacom",
    .unmigratable = 1,
};

void USBWacomState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    USBDeviceClass *uc = reinterpret_cast<USBDeviceClass *>(klass);

    uc->product_desc   = "QEMU PenPartner Tablet";
    uc->usb_desc       = &desc_wacom;
    uc->realize        = USBWacomState::realize;
    uc->handle_reset   = USBWacomState::handleReset;
    uc->handle_control = USBWacomState::handleControl;
    uc->handle_data    = USBWacomState::handleData;
    uc->unrealize      = USBWacomState::unrealize;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "QEMU PenPartner Tablet";
    dc->vmsd = &vmstate_usb_wacom;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(USBWacomState, TYPE_USB_WACOM, TYPE_USB_DEVICE)

static void __attribute__((constructor)) usb_wacom_legacy_init(void)
{
    usb_legacy_register(TYPE_USB_WACOM, "wacom-tablet", NULL);
}
