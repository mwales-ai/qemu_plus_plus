/*
 * QEMU PS/2 keyboard/mouse emulation
 *
 * Copyright (C) 2003 Fabrice Bellard
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

#ifndef HW_PS2_H
#define HW_PS2_H

#include "hw/sysbus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_MOUSE_BUTTON_LEFT   0x01
#define PS2_MOUSE_BUTTON_RIGHT  0x02
#define PS2_MOUSE_BUTTON_MIDDLE 0x04
#define PS2_MOUSE_BUTTON_SIDE   0x08
#define PS2_MOUSE_BUTTON_EXTRA  0x10

struct PS2DeviceClass {
    SysBusDeviceClass parent_class;

    ResettablePhases parent_phases;
};

/*
 * PS/2 buffer size. Keep 256 bytes for compatibility with
 * older QEMU versions.
 */
#define PS2_BUFFER_SIZE     256

typedef struct {
    uint8_t data[PS2_BUFFER_SIZE];
    int rptr, wptr, cwptr, count;
} PS2Queue;

/* Output IRQ */
#define PS2_DEVICE_IRQ      0

#ifdef __cplusplus
} /* end extern "C" - structs with methods must be outside extern "C" */
#endif

struct PS2State {
    SysBusDevice parent_obj;

    PS2Queue queue;
    int32_t write_cmd;
    qemu_irq irq;

#ifdef __cplusplus
    void resetQueue();
    int queueEmpty();
    void queueNoirq(int b);
    void raiseIrq();
    void lowerIrq();
    void queueByte(int b);
    void queue2(int b1, int b2);
    void queue3(int b1, int b2, int b3);
    void queue4(int b1, int b2, int b3, int b4);
    void cqueueData(int b);
    void cqueue1(int b1);
    void cqueue2(int b1, int b2);
    void cqueue3(int b1, int b2, int b3);
    void cqueueReset();
    uint32_t readData();
    void commonPostLoad();
    void resetHold(ResetType type);
    void resetExit(ResetType type);
#endif
};

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE_PS2_DEVICE "ps2-device"
OBJECT_DECLARE_TYPE(PS2State, PS2DeviceClass, PS2_DEVICE)

#ifdef __cplusplus
} /* end extern "C" */
#endif

struct PS2KbdState {
    PS2State parent_obj;

    int scan_enabled;
    int translate;
    int scancode_set; /* 1=XT, 2=AT, 3=PS/2 */
    int ledstate;
    bool need_high_bit;
    unsigned int modifiers; /* bitmask of MOD_* constants above */

#ifdef __cplusplus
    void putKeycode(int keycode);
    void setLedstate(int new_ledstate);
    void resetKeyboard();
    void writeKeyboard(int val);
    void setTranslation(int mode);
    void kbdResetHold(ResetType type);

    static void keyboardEvent(DeviceState *dev, QemuConsole *src,
                              struct InputEvent *evt);
    static void kbdRealize(DeviceState *dev, Error **errp);
    static void classInit(DeviceClass *dc);
#endif
};

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE_PS2_KBD_DEVICE "ps2-kbd"
OBJECT_DECLARE_SIMPLE_TYPE(PS2KbdState, PS2_KBD_DEVICE)

#ifdef __cplusplus
} /* end extern "C" */
#endif

struct PS2MouseState {
    PS2State parent_obj;

    uint8_t mouse_status;
    uint8_t mouse_resolution;
    uint8_t mouse_sample_rate;
    uint8_t mouse_wrap;
    uint8_t mouse_type; /* 0 = PS2, 3 = IMPS/2, 4 = IMEX */
    uint8_t mouse_detect_state;
    int mouse_dx; /* current values, needed for 'poll' mode */
    int mouse_dy;
    int mouse_dz;
    int mouse_dw;
    uint8_t mouse_buttons;

#ifdef __cplusplus
    int sendPacket();
    void writeMouse(int val);
    void fakeEvent();
    void mouseResetHold(ResetType type);

    static void mouseEvent(DeviceState *dev, QemuConsole *src,
                           struct InputEvent *evt);
    static void mouseSync(DeviceState *dev);
    static void mouseRealize(DeviceState *dev, Error **errp);
    static void classInit(DeviceClass *dc);
#endif
};

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE_PS2_MOUSE_DEVICE "ps2-mouse"
OBJECT_DECLARE_SIMPLE_TYPE(PS2MouseState, PS2_MOUSE_DEVICE)

/* ps2.c */
void ps2_write_mouse(PS2MouseState *s, int val);
void ps2_write_keyboard(PS2KbdState *s, int val);
uint32_t ps2_read_data(PS2State *s);
void ps2_queue_noirq(PS2State *s, int b);
void ps2_queue(PS2State *s, int b);
void ps2_queue_2(PS2State *s, int b1, int b2);
void ps2_queue_3(PS2State *s, int b1, int b2, int b3);
void ps2_queue_4(PS2State *s, int b1, int b2, int b3, int b4);
void ps2_keyboard_set_translation(PS2KbdState *s, int mode);
void ps2_mouse_fake_event(PS2MouseState *s);
int ps2_queue_empty(PS2State *s);

#ifdef __cplusplus
}
#endif

#endif /* HW_PS2_H */
