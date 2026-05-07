/*
 * QEMU USB HID devices
 *
 * Copyright (c) 2005 Fabrice Bellard
 * Copyright (c) 2007 OpenMoko, Inc.  (andrew@openedhand.com)
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
#include "migration/vmstate.h"
#include "desc.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/input/hid.h"
#include "hw/usb/hid.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"

struct USBHIDState {
    USBDevice dev;
    USBEndpoint *intr;
    HIDState hid;
    uint32_t usb_version;
    char *display;
    uint32_t head;

    /* Static callbacks */
    static void hidChanged(HIDState *hs);
    static void handleReset(USBDevice *dev);
    static void handleControl(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data);
    static void handleData(USBDevice *dev, USBPacket *p);
    static void unrealize(USBDevice *dev);
    static void tabletRealize(USBDevice *dev, Error **errp);
    static void mouseRealize(USBDevice *dev, Error **errp);
    static void keyboardRealize(USBDevice *dev, Error **errp);
    static int ptrPostLoad(void *opaque, int version_id);
    static void hidClassInit(ObjectClass *klass, const void *data);
    static void tabletClassInit(ObjectClass *klass, const void *data);
    static void mouseClassInit(ObjectClass *klass, const void *data);
    static void keyboardClassInit(ObjectClass *klass, const void *data);
    static void classInit(DeviceClass *dc);

private:
    static void initfn(USBDevice *dev, int kind,
                       const USBDesc *usb1, const USBDesc *usb2,
                       Error **errp);
};

#define TYPE_USB_HID "usb-hid"
OBJECT_DECLARE_SIMPLE_TYPE(USBHIDState, USB_HID)

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT_MOUSE,
    STR_PRODUCT_TABLET,
    STR_PRODUCT_KEYBOARD,
    STR_SERIAL_COMPAT,
    STR_CONFIG_MOUSE,
    STR_CONFIG_TABLET,
    STR_CONFIG_KEYBOARD,
    STR_SERIAL_MOUSE,
    STR_SERIAL_TABLET,
    STR_SERIAL_KEYBOARD,
};

static USBDescStrings desc_strings;

static void __attribute__((constructor)) usb_hid_init_strings(void)
{
    desc_strings[STR_MANUFACTURER]     = "QEMU";
    desc_strings[STR_PRODUCT_MOUSE]    = "QEMU USB Mouse";
    desc_strings[STR_PRODUCT_TABLET]   = "QEMU USB Tablet";
    desc_strings[STR_PRODUCT_KEYBOARD] = "QEMU USB Keyboard";
    desc_strings[STR_SERIAL_COMPAT]    = "42";
    desc_strings[STR_CONFIG_MOUSE]     = "HID Mouse";
    desc_strings[STR_CONFIG_TABLET]    = "HID Tablet";
    desc_strings[STR_CONFIG_KEYBOARD]  = "HID Keyboard";
    desc_strings[STR_SERIAL_MOUSE]     = "89126";
    desc_strings[STR_SERIAL_TABLET]    = "28754";
    desc_strings[STR_SERIAL_KEYBOARD]  = "68284";
}

/* Extracted compound literals for C++ compatibility */

/* Mouse HID descriptor data */
static uint8_t desc_mouse_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x01, 0x00,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    52, 0,         /*  u16 len */
};

static USBDescOther desc_mouse_hid_descs[] = {
    { .data = desc_mouse_hid_data, },
};

static USBDescEndpoint desc_mouse_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 4,
        .bInterval             = 0x0a,
    },
};

static const USBDescIface desc_iface_mouse = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = desc_mouse_hid_descs,
    .eps = desc_mouse_eps,
};

/* Mouse2 HID descriptor data (USB 2.0) */
static uint8_t desc_mouse2_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x01, 0x00,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    52, 0,         /*  u16 len */
};

static USBDescOther desc_mouse2_hid_descs[] = {
    { .data = desc_mouse2_hid_data, },
};

static USBDescEndpoint desc_mouse2_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 4,
        .bInterval             = 7, /* 2 ^ (8-1) * 125 usecs = 8 ms */
    },
};

static const USBDescIface desc_iface_mouse2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = desc_mouse2_hid_descs,
    .eps = desc_mouse2_eps,
};

/* Tablet HID descriptor data */
static uint8_t desc_tablet_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x01, 0x00,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    74, 0,         /*  u16 len */
};

static USBDescOther desc_tablet_hid_descs[] = {
    { .data = desc_tablet_hid_data, },
};

static USBDescEndpoint desc_tablet_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 8,
        .bInterval             = 0x0a,
    },
};

static const USBDescIface desc_iface_tablet = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = desc_tablet_hid_descs,
    .eps = desc_tablet_eps,
};

/* Tablet2 HID descriptor data (USB 2.0) */
static uint8_t desc_tablet2_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x01, 0x00,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    74, 0,         /*  u16 len */
};

static USBDescOther desc_tablet2_hid_descs[] = {
    { .data = desc_tablet2_hid_data, },
};

static USBDescEndpoint desc_tablet2_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 8,
        .bInterval             = 4, /* 2 ^ (4-1) * 125 usecs = 1 ms */
    },
};

static const USBDescIface desc_iface_tablet2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = desc_tablet2_hid_descs,
    .eps = desc_tablet2_eps,
};

/* Keyboard HID descriptor data */
static uint8_t desc_keyboard_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x11, 0x01,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    0x3f, 0,       /*  u16 len */
};

static USBDescOther desc_keyboard_hid_descs[] = {
    { .data = desc_keyboard_hid_data, },
};

static USBDescEndpoint desc_keyboard_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 8,
        .bInterval             = 0x0a,
    },
};

static const USBDescIface desc_iface_keyboard = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x01, /* keyboard */
    .ndesc                         = 1,
    .descs = desc_keyboard_hid_descs,
    .eps = desc_keyboard_eps,
};

/* Keyboard2 HID descriptor data (USB 2.0) */
static uint8_t desc_keyboard2_hid_data[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x11, 0x01,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    0x3f, 0,       /*  u16 len */
};

static USBDescOther desc_keyboard2_hid_descs[] = {
    { .data = desc_keyboard2_hid_data, },
};

static USBDescEndpoint desc_keyboard2_eps[] = {
    {
        .bEndpointAddress      = USB_DIR_IN | 0x01,
        .bmAttributes          = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize        = 8,
        .bInterval             = 7, /* 2 ^ (8-1) * 125 usecs = 8 ms */
    },
};

static const USBDescIface desc_iface_keyboard2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x01, /* keyboard */
    .ndesc                         = 1,
    .descs = desc_keyboard2_hid_descs,
    .eps = desc_keyboard2_eps,
};

/* USB config arrays for device descriptors */
static const USBDescConfig desc_device_mouse_confs[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .iConfiguration        = STR_CONFIG_MOUSE,
        .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower             = 50,
        .nif = 1,
        .ifs = &desc_iface_mouse,
    },
};

static const USBDescDevice desc_device_mouse = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = desc_device_mouse_confs,
};

static const USBDescConfig desc_device_mouse2_confs[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .iConfiguration        = STR_CONFIG_MOUSE,
        .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower             = 50,
        .nif = 1,
        .ifs = &desc_iface_mouse2,
    },
};

static const USBDescDevice desc_device_mouse2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = desc_device_mouse2_confs,
};

static const USBDescConfig desc_device_tablet_confs[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .iConfiguration        = STR_CONFIG_TABLET,
        .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower             = 50,
        .nif = 1,
        .ifs = &desc_iface_tablet,
    },
};

static const USBDescDevice desc_device_tablet = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = desc_device_tablet_confs,
};

static const USBDescConfig desc_device_tablet2_confs[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .iConfiguration        = STR_CONFIG_TABLET,
        .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower             = 50,
        .nif = 1,
        .ifs = &desc_iface_tablet2,
    },
};

static const USBDescDevice desc_device_tablet2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = desc_device_tablet2_confs,
};

static const USBDescConfig desc_device_keyboard_confs[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .iConfiguration        = STR_CONFIG_KEYBOARD,
        .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower             = 50,
        .nif = 1,
        .ifs = &desc_iface_keyboard,
    },
};

static const USBDescDevice desc_device_keyboard = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = desc_device_keyboard_confs,
};

static const USBDescConfig desc_device_keyboard2_confs[] = {
    {
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .iConfiguration        = STR_CONFIG_KEYBOARD,
        .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower             = 50,
        .nif = 1,
        .ifs = &desc_iface_keyboard2,
    },
};

static const USBDescDevice desc_device_keyboard2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = desc_device_keyboard2_confs,
};

static const USBDescMSOS desc_msos_suspend = {
    .SelectiveSuspendEnabled = true,
};

static const USBDesc desc_mouse = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_MOUSE,
        .iSerialNumber     = STR_SERIAL_MOUSE,
    },
    .full = &desc_device_mouse,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_mouse2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_MOUSE,
        .iSerialNumber     = STR_SERIAL_MOUSE,
    },
    .full = &desc_device_mouse,
    .high = &desc_device_mouse2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_tablet = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_tablet,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_tablet2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_tablet,
    .high = &desc_device_tablet2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_keyboard = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_KEYBOARD,
        .iSerialNumber     = STR_SERIAL_KEYBOARD,
    },
    .full = &desc_device_keyboard,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_keyboard2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_KEYBOARD,
        .iSerialNumber     = STR_SERIAL_KEYBOARD,
    },
    .full = &desc_device_keyboard,
    .high = &desc_device_keyboard2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const uint8_t qemu_mouse_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x02,		/* Usage (Mouse) */
    0xa1, 0x01,		/* Collection (Application) */
    0x09, 0x01,		/*   Usage (Pointer) */
    0xa1, 0x00,		/*   Collection (Physical) */
    0x05, 0x09,		/*     Usage Page (Button) */
    0x19, 0x01,		/*     Usage Minimum (1) */
    0x29, 0x05,		/*     Usage Maximum (5) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x25, 0x01,		/*     Logical Maximum (1) */
    0x95, 0x05,		/*     Report Count (5) */
    0x75, 0x01,		/*     Report Size (1) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*     Report Count (1) */
    0x75, 0x03,		/*     Report Size (3) */
    0x81, 0x01,		/*     Input (Constant) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x30,		/*     Usage (X) */
    0x09, 0x31,		/*     Usage (Y) */
    0x09, 0x38,		/*     Usage (Wheel) */
    0x15, 0x81,		/*     Logical Minimum (-0x7f) */
    0x25, 0x7f,		/*     Logical Maximum (0x7f) */
    0x75, 0x08,		/*     Report Size (8) */
    0x95, 0x03,		/*     Report Count (3) */
    0x81, 0x06,		/*     Input (Data, Variable, Relative) */
    0xc0,		/*   End Collection */
    0xc0,		/* End Collection */
};

static const uint8_t qemu_tablet_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x02,		/* Usage (Mouse) */
    0xa1, 0x01,		/* Collection (Application) */
    0x09, 0x01,		/*   Usage (Pointer) */
    0xa1, 0x00,		/*   Collection (Physical) */
    0x05, 0x09,		/*     Usage Page (Button) */
    0x19, 0x01,		/*     Usage Minimum (1) */
    0x29, 0x05,		/*     Usage Maximum (5) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x25, 0x01,		/*     Logical Maximum (1) */
    0x95, 0x05,		/*     Report Count (5) */
    0x75, 0x01,		/*     Report Size (1) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*     Report Count (1) */
    0x75, 0x03,		/*     Report Size (3) */
    0x81, 0x01,		/*     Input (Constant) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x30,		/*     Usage (X) */
    0x09, 0x31,		/*     Usage (Y) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,	/*     Logical Maximum (0x7fff) */
    0x35, 0x00,		/*     Physical Minimum (0) */
    0x46, 0xff, 0x7f,	/*     Physical Maximum (0x7fff) */
    0x75, 0x10,		/*     Report Size (16) */
    0x95, 0x02,		/*     Report Count (2) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x38,		/*     Usage (Wheel) */
    0x15, 0x81,		/*     Logical Minimum (-0x7f) */
    0x25, 0x7f,		/*     Logical Maximum (0x7f) */
    0x35, 0x00,		/*     Physical Minimum (same as logical) */
    0x45, 0x00,		/*     Physical Maximum (same as logical) */
    0x75, 0x08,		/*     Report Size (8) */
    0x95, 0x01,		/*     Report Count (1) */
    0x81, 0x06,		/*     Input (Data, Variable, Relative) */
    0xc0,		/*   End Collection */
    0xc0,		/* End Collection */
};

static const uint8_t qemu_keyboard_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x06,		/* Usage (Keyboard) */
    0xa1, 0x01,		/* Collection (Application) */
    0x75, 0x01,		/*   Report Size (1) */
    0x95, 0x08,		/*   Report Count (8) */
    0x05, 0x07,		/*   Usage Page (Key Codes) */
    0x19, 0xe0,		/*   Usage Minimum (224) */
    0x29, 0xe7,		/*   Usage Maximum (231) */
    0x15, 0x00,		/*   Logical Minimum (0) */
    0x25, 0x01,		/*   Logical Maximum (1) */
    0x81, 0x02,		/*   Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*   Report Count (1) */
    0x75, 0x08,		/*   Report Size (8) */
    0x81, 0x01,		/*   Input (Constant) */
    0x95, 0x05,		/*   Report Count (5) */
    0x75, 0x01,		/*   Report Size (1) */
    0x05, 0x08,		/*   Usage Page (LEDs) */
    0x19, 0x01,		/*   Usage Minimum (1) */
    0x29, 0x05,		/*   Usage Maximum (5) */
    0x91, 0x02,		/*   Output (Data, Variable, Absolute) */
    0x95, 0x01,		/*   Report Count (1) */
    0x75, 0x03,		/*   Report Size (3) */
    0x91, 0x01,		/*   Output (Constant) */
    0x95, 0x06,		/*   Report Count (6) */
    0x75, 0x08,		/*   Report Size (8) */
    0x15, 0x00,		/*   Logical Minimum (0) */
    0x25, 0xff,		/*   Logical Maximum (255) */
    0x05, 0x07,		/*   Usage Page (Key Codes) */
    0x19, 0x00,		/*   Usage Minimum (0) */
    0x29, 0xff,		/*   Usage Maximum (255) */
    0x81, 0x00,		/*   Input (Data, Array) */
    0xc0,		/* End Collection */
};

void USBHIDState::hidChanged(HIDState *hs)
{
    USBHIDState *us = container_of(hs, USBHIDState, hid);

    usb_wakeup(us->intr, 0);
}

void USBHIDState::handleReset(USBDevice *dev)
{
    USBHIDState *us = reinterpret_cast<USBHIDState *>(dev);

    hid_reset(&us->hid);
}

void USBHIDState::handleControl(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBHIDState *us = reinterpret_cast<USBHIDState *>(dev);
    HIDState *hs = &us->hid;
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
        /* hid specific requests */
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        switch (value >> 8) {
        case 0x22:
            if (hs->kind == HID_MOUSE) {
                memcpy(data, qemu_mouse_hid_report_descriptor,
                       sizeof(qemu_mouse_hid_report_descriptor));
                p->actual_length = sizeof(qemu_mouse_hid_report_descriptor);
            } else if (hs->kind == HID_TABLET) {
                memcpy(data, qemu_tablet_hid_report_descriptor,
                       sizeof(qemu_tablet_hid_report_descriptor));
                p->actual_length = sizeof(qemu_tablet_hid_report_descriptor);
            } else if (hs->kind == HID_KEYBOARD) {
                memcpy(data, qemu_keyboard_hid_report_descriptor,
                       sizeof(qemu_keyboard_hid_report_descriptor));
                p->actual_length = sizeof(qemu_keyboard_hid_report_descriptor);
            }
            break;
        default:
            goto fail;
        }
        break;
    case HID_GET_REPORT:
        if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
            p->actual_length = hid_pointer_poll(hs, data, length);
        } else if (hs->kind == HID_KEYBOARD) {
            p->actual_length = hid_keyboard_poll(hs, data, length);
        }
        break;
    case HID_SET_REPORT:
        if (hs->kind == HID_KEYBOARD) {
            p->actual_length = hid_keyboard_write(hs, data, length);
        } else {
            goto fail;
        }
        break;
    case HID_GET_PROTOCOL:
        if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE) {
            goto fail;
        }
        data[0] = hs->protocol;
        p->actual_length = 1;
        break;
    case HID_SET_PROTOCOL:
        if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE) {
            goto fail;
        }
        hs->protocol = value;
        break;
    case HID_GET_IDLE:
        data[0] = hs->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        hs->idle = (uint8_t) (value >> 8);
        hid_set_next_idle(hs);
        if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
            hid_pointer_activate(hs);
        }
        break;
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

void USBHIDState::handleData(USBDevice *dev, USBPacket *p)
{
    USBHIDState *us = reinterpret_cast<USBHIDState *>(dev);
    HIDState *hs = &us->hid;
    g_autofree uint8_t *buf = static_cast<uint8_t *>(g_malloc(p->iov.size));
    int len = 0;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
                hid_pointer_activate(hs);
            }
            if (!hid_has_events(hs)) {
                p->status = USB_RET_NAK;
                return;
            }
            hid_set_next_idle(hs);
            if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
                len = hid_pointer_poll(hs, buf, p->iov.size);
            } else if (hs->kind == HID_KEYBOARD) {
                len = hid_keyboard_poll(hs, buf, p->iov.size);
            }
            usb_packet_copy(p, buf, len);
        } else {
            goto fail;
        }
        break;
    case USB_TOKEN_OUT:
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

void USBHIDState::unrealize(USBDevice *dev)
{
    USBHIDState *us = reinterpret_cast<USBHIDState *>(dev);

    hid_free(&us->hid);
}

void USBHIDState::initfn(USBDevice *dev, int kind,
                           const USBDesc *usb1, const USBDesc *usb2,
                           Error **errp)
{
    USBHIDState *us = reinterpret_cast<USBHIDState *>(dev);
    switch (us->usb_version) {
    case 1:
        dev->usb_desc = usb1;
        break;
    case 2:
        dev->usb_desc = usb2;
        break;
    default:
        dev->usb_desc = NULL;
    }
    if (!dev->usb_desc) {
        error_setg(errp, "Invalid usb version %d for usb hid device",
                   us->usb_version);
        return;
    }

    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    us->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    hid_init(&us->hid, kind, USBHIDState::hidChanged);
    if (us->display && us->hid.s) {
        qemu_input_handler_bind(us->hid.s, us->display, us->head, NULL);
    }
}

void USBHIDState::tabletRealize(USBDevice *dev, Error **errp)
{

    initfn(dev, HID_TABLET, &desc_tablet, &desc_tablet2, errp);
}

void USBHIDState::mouseRealize(USBDevice *dev, Error **errp)
{
    initfn(dev, HID_MOUSE, &desc_mouse, &desc_mouse2, errp);
}

void USBHIDState::keyboardRealize(USBDevice *dev, Error **errp)
{
    initfn(dev, HID_KEYBOARD, &desc_keyboard, &desc_keyboard2, errp);
}

int USBHIDState::ptrPostLoad(void *opaque, int version_id)
{
    USBHIDState *s = static_cast<USBHIDState *>(opaque);

    if (s->dev.remote_wakeup) {
        hid_pointer_activate(&s->hid);
    }
    return 0;
}

static const VMStateField vmstate_usb_ptr_fields[] = {
    VMSTATE_USB_DEVICE(dev, USBHIDState),
    VMSTATE_HID_POINTER_DEVICE(hid, USBHIDState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_usb_ptr = {
    .name = "usb-ptr",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = USBHIDState::ptrPostLoad,
    .fields = vmstate_usb_ptr_fields,
};

static const VMStateField vmstate_usb_kbd_fields[] = {
    VMSTATE_USB_DEVICE(dev, USBHIDState),
    VMSTATE_HID_KEYBOARD_DEVICE(hid, USBHIDState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_usb_kbd = {
    .name = "usb-kbd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_usb_kbd_fields,
};

void USBHIDState::hidClassInit(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = reinterpret_cast<USBDeviceClass *>(klass);

    uc->handle_reset   = USBHIDState::handleReset;
    uc->handle_control = USBHIDState::handleControl;
    uc->handle_data    = USBHIDState::handleData;
    uc->unrealize      = USBHIDState::unrealize;
    uc->handle_attach  = usb_desc_attach;
}

void USBHIDState::classInit(DeviceClass *dc)
{
    hidClassInit(reinterpret_cast<ObjectClass *>(dc), nullptr);
}

static const Property usb_tablet_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
        DEFINE_PROP_UINT32("head", USBHIDState, head, 0),
};

void USBHIDState::tabletClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    USBDeviceClass *uc = reinterpret_cast<USBDeviceClass *>(klass);

    uc->realize        = USBHIDState::tabletRealize;
    uc->product_desc   = "QEMU USB Tablet";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_tablet_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const Property usb_mouse_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
};

void USBHIDState::mouseClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    USBDeviceClass *uc = reinterpret_cast<USBDeviceClass *>(klass);

    uc->realize        = USBHIDState::mouseRealize;
    uc->product_desc   = "QEMU USB Mouse";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_mouse_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const Property usb_keyboard_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
};

void USBHIDState::keyboardClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    USBDeviceClass *uc = reinterpret_cast<USBDeviceClass *>(klass);

    uc->realize        = USBHIDState::keyboardRealize;
    uc->product_desc   = "QEMU USB Keyboard";
    dc->vmsd = &vmstate_usb_kbd;
    device_class_set_props(dc, usb_keyboard_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static void usb_hid_legacy_register(void)
{
    usb_legacy_register("usb-tablet", "tablet", NULL);
    usb_legacy_register("usb-mouse", "mouse", NULL);
    usb_legacy_register("usb-kbd", "keyboard", NULL);
}

type_init(usb_hid_legacy_register)

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS(USBHIDState, TYPE_USB_HID, TYPE_USB_DEVICE)

REGISTER_QEMU_OBJECT_CLASS_ONLY(usb_tablet, "usb-tablet", TYPE_USB_HID,
                                 USBHIDState::tabletClassInit)

REGISTER_QEMU_OBJECT_CLASS_ONLY(usb_mouse, "usb-mouse", TYPE_USB_HID,
                                 USBHIDState::mouseClassInit)

REGISTER_QEMU_OBJECT_CLASS_ONLY(usb_keyboard, "usb-kbd", TYPE_USB_HID,
                                 USBHIDState::keyboardClassInit)
