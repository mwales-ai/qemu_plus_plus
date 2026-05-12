/*
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/iov.h"
#include "qemu/module.h"
}

#include "hw/virtio/virtio.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-input.h"

#include "ui/console.h"

extern "C" {
#include "standard-headers/linux/input.h"
}

#define VIRTIO_ID_NAME_KEYBOARD     "QEMU Virtio Keyboard"
#define VIRTIO_ID_NAME_MOUSE        "QEMU Virtio Mouse"
#define VIRTIO_ID_NAME_TABLET       "QEMU Virtio Tablet"
#define VIRTIO_ID_NAME_MULTITOUCH   "QEMU Virtio MultiTouch"

/* ----------------------------------------------------------------- */

static unsigned short keymap_button[INPUT_BUTTON__MAX];
static unsigned short axismap_rel[INPUT_AXIS__MAX];
static unsigned short axismap_abs[INPUT_AXIS__MAX];
static unsigned short axismap_tch[INPUT_AXIS__MAX];

static void __attribute__((constructor)) init_input_maps(void)
{
    memset(keymap_button, 0, sizeof(keymap_button));
    keymap_button[INPUT_BUTTON_LEFT]       = BTN_LEFT;
    keymap_button[INPUT_BUTTON_RIGHT]      = BTN_RIGHT;
    keymap_button[INPUT_BUTTON_MIDDLE]     = BTN_MIDDLE;
    keymap_button[INPUT_BUTTON_WHEEL_UP]   = BTN_GEAR_UP;
    keymap_button[INPUT_BUTTON_WHEEL_DOWN] = BTN_GEAR_DOWN;
    keymap_button[INPUT_BUTTON_SIDE]       = BTN_SIDE;
    keymap_button[INPUT_BUTTON_EXTRA]      = BTN_EXTRA;
    keymap_button[INPUT_BUTTON_TOUCH]      = BTN_TOUCH;

    memset(axismap_rel, 0, sizeof(axismap_rel));
    axismap_rel[INPUT_AXIS_X] = REL_X;
    axismap_rel[INPUT_AXIS_Y] = REL_Y;

    memset(axismap_abs, 0, sizeof(axismap_abs));
    axismap_abs[INPUT_AXIS_X] = ABS_X;
    axismap_abs[INPUT_AXIS_Y] = ABS_Y;

    memset(axismap_tch, 0, sizeof(axismap_tch));
    axismap_tch[INPUT_AXIS_X] = ABS_MT_POSITION_X;
    axismap_tch[INPUT_AXIS_Y] = ABS_MT_POSITION_Y;
}

/* ----------------------------------------------------------------- */

static void virtio_input_extend_config(VirtIOInput *vinput,
                                       const unsigned short *map,
                                       size_t mapsize,
                                       uint8_t select, uint8_t subsel)
{
    virtio_input_config ext;
    int i, bit, byte, bmax = 0;

    memset(&ext, 0, sizeof(ext));
    for (i = 0; i < mapsize; i++) {
        bit = map[i];
        if (!bit) {
            continue;
        }
        byte = bit / 8;
        bit  = bit % 8;
        ext.u.bitmap[byte] |= (1 << bit);
        if (bmax < byte+1) {
            bmax = byte+1;
        }
    }
    ext.select = select;
    ext.subsel = subsel;
    ext.size   = bmax;
    virtio_input_add_config(vinput, &ext);
}

static void virtio_input_handle_event(DeviceState *dev, QemuConsole *src,
                                      InputEvent *evt)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(dev);
    VirtIOInput *vinput = VIRTIO_INPUT(dev);
    virtio_input_event event;
    int qcode;
    InputKeyEvent *key;
    InputMoveEvent *move;
    InputBtnEvent *btn;
    InputMultiTouchEvent *mtt;

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY:
        key = evt->u.key.data;
        qcode = qemu_input_key_value_to_qcode(key->key);
        if (qcode < qemu_input_map_qcode_to_linux_len &&
            qemu_input_map_qcode_to_linux[qcode]) {
            event.type  = cpu_to_le16(EV_KEY);
            event.code  = cpu_to_le16(qemu_input_map_qcode_to_linux[qcode]);
            event.value = cpu_to_le32(key->down ? 1 : 0);
            virtio_input_send(vinput, &event);
        } else {
            if (key->down) {
                fprintf(stderr, "%s: unmapped key: %d [%s]\n", __func__,
                        qcode, QKeyCode_str(qcode));
            }
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (vhid->wheel_axis &&
            (btn->button == INPUT_BUTTON_WHEEL_UP ||
             btn->button == INPUT_BUTTON_WHEEL_DOWN) &&
            btn->down) {
            event.type  = cpu_to_le16(EV_REL);
            event.code  = cpu_to_le16(REL_WHEEL);
            event.value = cpu_to_le32(btn->button == INPUT_BUTTON_WHEEL_UP
                                      ? 1 : -1);
            virtio_input_send(vinput, &event);
        } else if (keymap_button[btn->button]) {
            event.type  = cpu_to_le16(EV_KEY);
            event.code  = cpu_to_le16(keymap_button[btn->button]);
            event.value = cpu_to_le32(btn->down ? 1 : 0);
            virtio_input_send(vinput, &event);
        } else {
            if (btn->down) {
                fprintf(stderr, "%s: unmapped button: %d [%s]\n", __func__,
                        btn->button,
                        InputButton_str(btn->button));
            }
        }
        break;
    case INPUT_EVENT_KIND_REL:
        move = evt->u.rel.data;
        event.type  = cpu_to_le16(EV_REL);
        event.code  = cpu_to_le16(axismap_rel[move->axis]);
        event.value = cpu_to_le32(move->value);
        virtio_input_send(vinput, &event);
        break;
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        event.type  = cpu_to_le16(EV_ABS);
        event.code  = cpu_to_le16(axismap_abs[move->axis]);
        event.value = cpu_to_le32(move->value);
        virtio_input_send(vinput, &event);
        break;
    case INPUT_EVENT_KIND_MTT:
        mtt = evt->u.mtt.data;
        if (mtt->type == INPUT_MULTI_TOUCH_TYPE_DATA) {
            event.type  = cpu_to_le16(EV_ABS);
            event.code  = cpu_to_le16(axismap_tch[mtt->axis]);
            event.value = cpu_to_le32(mtt->value);
            virtio_input_send(vinput, &event);
        } else {
            event.type  = cpu_to_le16(EV_ABS);
            event.code  = cpu_to_le16(ABS_MT_SLOT);
            event.value = cpu_to_le32(mtt->slot);
            virtio_input_send(vinput, &event);
            event.type  = cpu_to_le16(EV_ABS);
            event.code  = cpu_to_le16(ABS_MT_TRACKING_ID);
            event.value = cpu_to_le32(mtt->tracking_id);
            virtio_input_send(vinput, &event);
        }
        break;
    default:
        /* keep gcc happy */
        break;
    }
}

static void virtio_input_handle_sync(DeviceState *dev)
{
    VirtIOInput *vinput = VIRTIO_INPUT(dev);
    virtio_input_event event = {
        .type  = cpu_to_le16(EV_SYN),
        .code  = cpu_to_le16(SYN_REPORT),
        .value = 0,
    };

    virtio_input_send(vinput, &event);
}

static void virtio_input_hid_realize(DeviceState *dev, Error **errp)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(dev);

    vhid->hs = qemu_input_handler_register(dev, vhid->handler);
    if (vhid->display && vhid->hs) {
        qemu_input_handler_bind(vhid->hs, vhid->display, vhid->head, NULL);
    }
}

static void virtio_input_hid_unrealize(DeviceState *dev)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(dev);
    qemu_input_handler_unregister(vhid->hs);
}

static void virtio_input_hid_change_active(VirtIOInput *vinput)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(vinput);

    if (vinput->active) {
        qemu_input_handler_activate(vhid->hs);
    } else {
        qemu_input_handler_deactivate(vhid->hs);
    }
}

static void virtio_input_hid_handle_status(VirtIOInput *vinput,
                                           virtio_input_event *event)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(vinput);
    int ledbit = 0;

    switch (le16_to_cpu(event->type)) {
    case EV_LED:
        if (event->code == LED_NUML) {
            ledbit = QEMU_NUM_LOCK_LED;
        } else if (event->code == LED_CAPSL) {
            ledbit = QEMU_CAPS_LOCK_LED;
        } else if (event->code == LED_SCROLLL) {
            ledbit = QEMU_SCROLL_LOCK_LED;
        }
        if (event->value) {
            vhid->ledstate |= ledbit;
        } else {
            vhid->ledstate &= ~ledbit;
        }
        kbd_put_ledstate(vhid->ledstate);
        break;
    default:
        fprintf(stderr, "%s: unknown type %d\n", __func__,
                le16_to_cpu(event->type));
        break;
    }
}

static const Property virtio_input_hid_properties[] = {
    DEFINE_PROP_STRING("display", VirtIOInputHID, display),
    DEFINE_PROP_UINT32("head", VirtIOInputHID, head, 0),
};

static void virtio_input_hid_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtIOInputClass *vic = VIRTIO_INPUT_CLASS(klass);

    device_class_set_props(dc, virtio_input_hid_properties);
    vic->realize       = virtio_input_hid_realize;
    vic->unrealize     = virtio_input_hid_unrealize;
    vic->change_active = virtio_input_hid_change_active;
    vic->handle_status = virtio_input_hid_handle_status;
}


/* ----------------------------------------------------------------- */

static const QemuInputHandler virtio_keyboard_handler = {
    .name  = VIRTIO_ID_NAME_KEYBOARD,
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = virtio_input_handle_event,
    .sync  = virtio_input_handle_sync,
};

static struct virtio_input_config virtio_keyboard_config[5];

static void virtio_keyboard_init(Object *obj)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(obj);
    VirtIOInput *vinput = VIRTIO_INPUT(obj);

    vhid->handler = &virtio_keyboard_handler;
    virtio_input_init_config(vinput, virtio_keyboard_config);
    virtio_input_extend_config(vinput, qemu_input_map_qcode_to_linux,
                               qemu_input_map_qcode_to_linux_len,
                               VIRTIO_INPUT_CFG_EV_BITS, EV_KEY);
}


/* ----------------------------------------------------------------- */

static const QemuInputHandler virtio_mouse_handler = {
    .name  = VIRTIO_ID_NAME_MOUSE,
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = virtio_input_handle_event,
    .sync  = virtio_input_handle_sync,
};

static struct virtio_input_config virtio_mouse_config_v1[4];
static struct virtio_input_config virtio_mouse_config_v2[4];

static const Property virtio_mouse_properties[] = {
    DEFINE_PROP_BOOL("wheel-axis", VirtIOInputHID, wheel_axis, true),
};

static void virtio_mouse_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_mouse_properties);
}

static void virtio_mouse_init(Object *obj)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(obj);
    VirtIOInput *vinput = VIRTIO_INPUT(obj);

    vhid->handler = &virtio_mouse_handler;
    virtio_input_init_config(vinput, vhid->wheel_axis
                             ? virtio_mouse_config_v2
                             : virtio_mouse_config_v1);
    virtio_input_extend_config(vinput, keymap_button,
                               ARRAY_SIZE(keymap_button),
                               VIRTIO_INPUT_CFG_EV_BITS, EV_KEY);
}


/* ----------------------------------------------------------------- */

static const QemuInputHandler virtio_tablet_handler = {
    .name  = VIRTIO_ID_NAME_TABLET,
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = virtio_input_handle_event,
    .sync  = virtio_input_handle_sync,
};

static struct virtio_input_config virtio_tablet_config_v1[6];
static struct virtio_input_config virtio_tablet_config_v2[7];

static const Property virtio_tablet_properties[] = {
    DEFINE_PROP_BOOL("wheel-axis", VirtIOInputHID, wheel_axis, true),
};

static void virtio_tablet_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_tablet_properties);
}

static void virtio_tablet_init(Object *obj)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(obj);
    VirtIOInput *vinput = VIRTIO_INPUT(obj);

    vhid->handler = &virtio_tablet_handler;
    virtio_input_init_config(vinput, vhid->wheel_axis
                             ? virtio_tablet_config_v2
                             : virtio_tablet_config_v1);
    virtio_input_extend_config(vinput, keymap_button,
                               ARRAY_SIZE(keymap_button),
                               VIRTIO_INPUT_CFG_EV_BITS, EV_KEY);
}


/* ----------------------------------------------------------------- */

static const QemuInputHandler virtio_multitouch_handler = {
    .name  = VIRTIO_ID_NAME_MULTITOUCH,
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_MTT,
    .event = virtio_input_handle_event,
    .sync  = virtio_input_handle_sync,
};

static struct virtio_input_config virtio_multitouch_config[7];

static void virtio_multitouch_init(Object *obj)
{
    VirtIOInputHID *vhid = VIRTIO_INPUT_HID(obj);
    VirtIOInput *vinput = VIRTIO_INPUT(obj);
    unsigned short abs_props[] = {
        INPUT_PROP_DIRECT,
    };
    unsigned short abs_bits[] = {
        ABS_MT_SLOT,
        ABS_MT_TRACKING_ID,
        ABS_MT_POSITION_X,
        ABS_MT_POSITION_Y,
    };

    vhid->handler = &virtio_multitouch_handler;
    virtio_input_init_config(vinput, virtio_multitouch_config);
    virtio_input_extend_config(vinput, keymap_button,
                               ARRAY_SIZE(keymap_button),
                               VIRTIO_INPUT_CFG_EV_BITS, EV_KEY);
    virtio_input_extend_config(vinput, abs_props,
                               ARRAY_SIZE(abs_props),
                               VIRTIO_INPUT_CFG_PROP_BITS, 0);
    virtio_input_extend_config(vinput, abs_bits,
                               ARRAY_SIZE(abs_bits),
                               VIRTIO_INPUT_CFG_EV_BITS, EV_ABS);
}


/* ----------------------------------------------------------------- */

/* Helper to set up a name config entry */
static void init_name_config(struct virtio_input_config *c, const char *name)
{
    memset(c, 0, sizeof(*c));
    c->select = VIRTIO_INPUT_CFG_ID_NAME;
    c->size = strlen(name) + 1;
    memcpy(c->u.string, name, c->size);
}

/* Helper to set up a devids config entry */
static void init_devids_config(struct virtio_input_config *c,
                               uint16_t product, uint16_t version)
{
    memset(c, 0, sizeof(*c));
    c->select = VIRTIO_INPUT_CFG_ID_DEVIDS;
    c->size = sizeof(struct virtio_input_devids);
    c->u.ids.bustype = const_le16(BUS_VIRTUAL);
    c->u.ids.vendor = const_le16(0x0627);
    c->u.ids.product = const_le16(product);
    c->u.ids.version = const_le16(version);
}

/* Helper to set up an abs_info config entry */
static void init_absinfo_config(struct virtio_input_config *c,
                                uint8_t subsel, uint32_t min_val, uint32_t max_val)
{
    memset(c, 0, sizeof(*c));
    c->select = VIRTIO_INPUT_CFG_ABS_INFO;
    c->subsel = subsel;
    c->size = sizeof(virtio_input_absinfo);
    c->u.abs.min = const_le32(min_val);
    c->u.abs.max = const_le32(max_val);
}

static void __attribute__((constructor)) init_virtio_input_hid_configs(void)
{
    /* virtio_keyboard_config */
    memset(virtio_keyboard_config, 0, sizeof(virtio_keyboard_config));
    init_name_config(&virtio_keyboard_config[0], VIRTIO_ID_NAME_KEYBOARD);
    init_devids_config(&virtio_keyboard_config[1], 0x0001, 0x0001);
    virtio_keyboard_config[2].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_keyboard_config[2].subsel = EV_REP;
    virtio_keyboard_config[2].size = 1;
    virtio_keyboard_config[3].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_keyboard_config[3].subsel = EV_LED;
    virtio_keyboard_config[3].size = 1;
    virtio_keyboard_config[3].u.bitmap[0] =
        (1 << LED_NUML) | (1 << LED_CAPSL) | (1 << LED_SCROLLL);
    /* [4] is zero-initialized end of list */

    /* virtio_mouse_config_v1 */
    memset(virtio_mouse_config_v1, 0, sizeof(virtio_mouse_config_v1));
    init_name_config(&virtio_mouse_config_v1[0], VIRTIO_ID_NAME_MOUSE);
    init_devids_config(&virtio_mouse_config_v1[1], 0x0002, 0x0001);
    virtio_mouse_config_v1[2].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_mouse_config_v1[2].subsel = EV_REL;
    virtio_mouse_config_v1[2].size = 1;
    virtio_mouse_config_v1[2].u.bitmap[0] = (1 << REL_X) | (1 << REL_Y);
    /* [3] is zero-initialized end of list */

    /* virtio_mouse_config_v2 */
    memset(virtio_mouse_config_v2, 0, sizeof(virtio_mouse_config_v2));
    init_name_config(&virtio_mouse_config_v2[0], VIRTIO_ID_NAME_MOUSE);
    init_devids_config(&virtio_mouse_config_v2[1], 0x0002, 0x0002);
    virtio_mouse_config_v2[2].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_mouse_config_v2[2].subsel = EV_REL;
    virtio_mouse_config_v2[2].size = 2;
    virtio_mouse_config_v2[2].u.bitmap[0] = (1 << REL_X) | (1 << REL_Y);
    virtio_mouse_config_v2[2].u.bitmap[1] = (1 << (REL_WHEEL - 8));
    /* [3] is zero-initialized end of list */

    /* virtio_tablet_config_v1 */
    memset(virtio_tablet_config_v1, 0, sizeof(virtio_tablet_config_v1));
    init_name_config(&virtio_tablet_config_v1[0], VIRTIO_ID_NAME_TABLET);
    init_devids_config(&virtio_tablet_config_v1[1], 0x0003, 0x0001);
    virtio_tablet_config_v1[2].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_tablet_config_v1[2].subsel = EV_ABS;
    virtio_tablet_config_v1[2].size = 1;
    virtio_tablet_config_v1[2].u.bitmap[0] = (1 << ABS_X) | (1 << ABS_Y);
    init_absinfo_config(&virtio_tablet_config_v1[3], ABS_X,
                        INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    init_absinfo_config(&virtio_tablet_config_v1[4], ABS_Y,
                        INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    /* [5] is zero-initialized end of list */

    /* virtio_tablet_config_v2 */
    memset(virtio_tablet_config_v2, 0, sizeof(virtio_tablet_config_v2));
    init_name_config(&virtio_tablet_config_v2[0], VIRTIO_ID_NAME_TABLET);
    init_devids_config(&virtio_tablet_config_v2[1], 0x0003, 0x0002);
    virtio_tablet_config_v2[2].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_tablet_config_v2[2].subsel = EV_ABS;
    virtio_tablet_config_v2[2].size = 1;
    virtio_tablet_config_v2[2].u.bitmap[0] = (1 << ABS_X) | (1 << ABS_Y);
    virtio_tablet_config_v2[3].select = VIRTIO_INPUT_CFG_EV_BITS;
    virtio_tablet_config_v2[3].subsel = EV_REL;
    virtio_tablet_config_v2[3].size = 2;
    virtio_tablet_config_v2[3].u.bitmap[0] = 0;
    virtio_tablet_config_v2[3].u.bitmap[1] = (1 << (REL_WHEEL - 8));
    init_absinfo_config(&virtio_tablet_config_v2[4], ABS_X,
                        INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    init_absinfo_config(&virtio_tablet_config_v2[5], ABS_Y,
                        INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    /* [6] is zero-initialized end of list */

    /* virtio_multitouch_config */
    memset(virtio_multitouch_config, 0, sizeof(virtio_multitouch_config));
    init_name_config(&virtio_multitouch_config[0], VIRTIO_ID_NAME_MULTITOUCH);
    init_devids_config(&virtio_multitouch_config[1], 0x0003, 0x0001);
    init_absinfo_config(&virtio_multitouch_config[2], ABS_MT_SLOT,
                        INPUT_EVENT_SLOTS_MIN, INPUT_EVENT_SLOTS_MAX);
    init_absinfo_config(&virtio_multitouch_config[3], ABS_MT_TRACKING_ID,
                        INPUT_EVENT_SLOTS_MIN, INPUT_EVENT_SLOTS_MAX);
    init_absinfo_config(&virtio_multitouch_config[4], ABS_MT_POSITION_X,
                        INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    init_absinfo_config(&virtio_multitouch_config[5], ABS_MT_POSITION_Y,
                        INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    /* [6] is zero-initialized end of list */
}

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS(VirtIOInputHID,
                                               TYPE_VIRTIO_INPUT_HID,
                                               TYPE_VIRTIO_INPUT,
                                               virtio_input_hid_class_init)

REGISTER_QEMU_OBJECT_INIT_ONLY(virtio_keyboard, TYPE_VIRTIO_KEYBOARD,
                                TYPE_VIRTIO_INPUT_HID,
                                virtio_keyboard_init)

REGISTER_QEMU_OBJECT_INIT_CLASS(virtio_mouse, TYPE_VIRTIO_MOUSE,
                                 TYPE_VIRTIO_INPUT_HID,
                                 virtio_mouse_init, virtio_mouse_class_init)

REGISTER_QEMU_OBJECT_INIT_CLASS(virtio_tablet, TYPE_VIRTIO_TABLET,
                                 TYPE_VIRTIO_INPUT_HID,
                                 virtio_tablet_init, virtio_tablet_class_init)

REGISTER_QEMU_OBJECT_INIT_ONLY(virtio_multitouch, TYPE_VIRTIO_MULTITOUCH,
                                TYPE_VIRTIO_INPUT_HID,
                                virtio_multitouch_init)
