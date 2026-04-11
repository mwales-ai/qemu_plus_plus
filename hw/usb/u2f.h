/*
 * U2F USB device.
 *
 * Copyright (c) 2020 César Belley <cesar.belley@lse.epita.fr>
 * Written by César Belley <cesar.belley@lse.epita.fr>
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

#ifndef U2F_H
#define U2F_H

#include "hw/qdev-core.h"

#define U2FHID_PACKET_SIZE 64
#define U2FHID_PENDING_IN_NUM 32

typedef struct U2FKeyInfo U2FKeyInfo;

#define TYPE_U2F_KEY "u2f-key"
OBJECT_DECLARE_TYPE(U2FKeyState, U2FKeyClass, U2F_KEY)

/*
 * Callbacks to be used by the U2F key base device (i.e. hw/u2f.c)
 * to interact with its variants (i.e. hw/u2f-*.c)
 */
#ifdef __cplusplus
struct U2FKeyClass : USBDeviceClass {
    void realize(USBDevice *dev, Error **errp) override;
    void unrealize(USBDevice *dev) override;
    void handle_reset(USBDevice *dev) override;
    void handle_control(USBDevice *dev, USBPacket *p, int request,
                        int value, int index, int length,
                        uint8_t *data) override;
    void handle_data(USBDevice *dev, USBPacket *p) override;
    void handle_attach(USBDevice *dev) override;
#else
struct U2FKeyClass {
    USBDeviceClass parent_class;
#endif

    /*< public >*/
    void (*recv_from_guest)(U2FKeyState *key,
                            const uint8_t packet[U2FHID_PACKET_SIZE]);
    void (*u2f_realize)(U2FKeyState *key, Error **errp);
    void (*u2f_unrealize)(U2FKeyState *key);
};

/*
 * State of the U2F key base device (i.e. hw/u2f.c)
 */
struct U2FKeyState {
    USBDevice dev;
    USBEndpoint *ep;
    uint8_t idle;

    /* Pending packets to be send to the guest */
    uint8_t pending_in[U2FHID_PENDING_IN_NUM][U2FHID_PACKET_SIZE];
    uint8_t pending_in_start;
    uint8_t pending_in_end;
    uint8_t pending_in_num;
};

/*
 * API to be used by the U2F key device variants (i.e. hw/u2f-*.c)
 * to interact with the U2F key base device (i.e. hw/u2f.c)
 */
#ifdef __cplusplus
extern "C" {
#endif

void u2f_send_to_guest(U2FKeyState *key,
                       const uint8_t packet[U2FHID_PACKET_SIZE]);

extern const VMStateDescription vmstate_u2f_key;

#ifdef __cplusplus
}
#endif

#define VMSTATE_U2F_KEY(_field, _state) {                            \
    .name       = (stringify(_field)),                               \
    .offset     = vmstate_offset_value(_state, _field, U2FKeyState), \
    .size       = sizeof(U2FKeyState),                               \
    .flags      = VMS_STRUCT,                                        \
    .vmsd       = &vmstate_u2f_key,                                  \
}

#endif /* U2F_H */
