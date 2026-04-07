/*
 * U2F USB Passthru device.
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

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "migration/vmstate.h"

#include "u2f.h"

#ifdef CONFIG_LIBUDEV
#include <libudev.h>
#endif
#include <linux/hidraw.h>
#include <sys/ioctl.h>

#define NONCE_SIZE 8
#define BROADCAST_CID 0xFFFFFFFF
#define TRANSACTION_TIMEOUT 120000

struct transaction {
    uint32_t cid;
    uint16_t resp_bcnt;
    uint16_t resp_size;

    /* Nonce for broadcast isolation */
    uint8_t nonce[NONCE_SIZE];
};

typedef struct U2FPassthruState U2FPassthruState;

#define CURRENT_TRANSACTIONS_NUM 4

struct U2FPassthruState {
    U2FKeyState base;

    /* Host device */
    char *hidraw;
    int hidraw_fd;

    /* Current Transactions */
    struct transaction current_transactions[CURRENT_TRANSACTIONS_NUM];
    uint8_t current_transactions_start;
    uint8_t current_transactions_end;
    uint8_t current_transactions_num;

    /* Transaction time checking */
    int64_t last_transaction_time;
    QEMUTimer timer;

    /* methods */
    void resetState();
    int transactionGetIndex(uint32_t cid);
    struct transaction *transactionGet(uint32_t cid);
    struct transaction *transactionGetFromNonce(const uint8_t nonce[NONCE_SIZE]);
    void transactionClose(uint32_t cid);
    void transactionAdd(uint32_t cid, const uint8_t nonce[NONCE_SIZE]);
    void transactionStart(const struct packet_init *packet_init);
    void recvFromHost(const uint8_t packet[U2FHID_PACKET_SIZE]);
    void doRealize(U2FKeyState *base, Error **errp);
    void doUnrealize(U2FKeyState *base);

    /* static callbacks */
    static void timeoutCheck(void *opaque);
    static void readFromHost(void *opaque);
    static void recvFromGuest(U2FKeyState *base,
                              const uint8_t packet[U2FHID_PACKET_SIZE]);
    static bool isU2fDevice(int fd);
    static int postLoad(void *opaque, int version_id);
    static void classInit(ObjectClass *klass, const void *data);
};

#define TYPE_U2F_PASSTHRU "u2f-passthru"
#define PASSTHRU_U2F_KEY(obj) \
    OBJECT_CHECK(U2FPassthruState, (obj), TYPE_U2F_PASSTHRU)

/* Init packet sizes */
#define PACKET_INIT_HEADER_SIZE 7
#define PACKET_INIT_DATA_SIZE (U2FHID_PACKET_SIZE - PACKET_INIT_HEADER_SIZE)

/* Cont packet sizes */
#define PACKET_CONT_HEADER_SIZE 5
#define PACKET_CONT_DATA_SIZE (U2FHID_PACKET_SIZE - PACKET_CONT_HEADER_SIZE)

struct packet_init {
    uint32_t cid;
    uint8_t cmd;
    uint8_t bcnth;
    uint8_t bcntl;
    uint8_t data[PACKET_INIT_DATA_SIZE];
} QEMU_PACKED;

static inline uint32_t packet_get_cid(const void *packet)
{
    return *((uint32_t *)packet);
}

static inline bool packet_is_init(const void *packet)
{
    return ((uint8_t *)packet)[4] & (1 << 7);
}

static inline uint16_t packet_init_get_bcnt(
        const struct packet_init *packet_init)
{
    uint16_t bcnt = 0;
    bcnt |= packet_init->bcnth << 8;
    bcnt |= packet_init->bcntl;

    return bcnt;
}

void U2FPassthruState::resetState()
{
    timer_del(&timer);
    qemu_set_fd_handler(hidraw_fd, NULL, NULL, this);
    last_transaction_time = 0;
    current_transactions_start = 0;
    current_transactions_end = 0;
    current_transactions_num = 0;
}

void U2FPassthruState::timeoutCheck(void *opaque)
{
    U2FPassthruState *key = static_cast<U2FPassthruState *>(opaque);
    int64_t time = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (time > key->last_transaction_time + TRANSACTION_TIMEOUT) {
        key->resetState();
    } else {
        timer_mod(&key->timer, time + TRANSACTION_TIMEOUT / 4);
    }
}

int U2FPassthruState::transactionGetIndex(uint32_t cid)
{
    for (int i = 0; i < current_transactions_num; ++i) {
        int index = (current_transactions_start + i)
            % CURRENT_TRANSACTIONS_NUM;
        if (cid == current_transactions[index].cid) {
            return index;
        }
    }
    return -1;
}

struct transaction *U2FPassthruState::transactionGet(uint32_t cid)
{
    int index = transactionGetIndex(cid);
    if (index < 0) {
        return NULL;
    }
    return &current_transactions[index];
}

struct transaction *U2FPassthruState::transactionGetFromNonce(
                                const uint8_t nonce[NONCE_SIZE])
{
    for (int i = 0; i < current_transactions_num; ++i) {
        int index = (current_transactions_start + i)
            % CURRENT_TRANSACTIONS_NUM;
        if (current_transactions[index].cid == BROADCAST_CID
            && memcmp(nonce, current_transactions[index].nonce,
                      NONCE_SIZE) == 0) {
            return &current_transactions[index];
        }
    }
    return NULL;
}

void U2FPassthruState::transactionClose(uint32_t cid)
{
    int index, next_index;
    index = transactionGetIndex(cid);
    if (index < 0) {
        return;
    }
    next_index = (index + 1) % CURRENT_TRANSACTIONS_NUM;

    /* Rearrange to ensure the oldest is at the start position */
    while (next_index != current_transactions_end) {
        memcpy(&current_transactions[index],
               &current_transactions[next_index],
               sizeof(struct transaction));

        index = next_index;
        next_index = (index + 1) % CURRENT_TRANSACTIONS_NUM;
    }

    current_transactions_end = index;
    --current_transactions_num;

    if (current_transactions_num == 0) {
        resetState();
    }
}

void U2FPassthruState::transactionAdd(uint32_t cid,
                                      const uint8_t nonce[NONCE_SIZE])
{
    uint8_t index;
    struct transaction *t;

    if (current_transactions_num >= CURRENT_TRANSACTIONS_NUM) {
        /* Close the oldest transaction */
        index = current_transactions_start;
        t = &current_transactions[index];
        transactionClose(t->cid);
    }

    /* Index */
    index = current_transactions_end;
    current_transactions_end = (index + 1) % CURRENT_TRANSACTIONS_NUM;
    ++current_transactions_num;

    /* Transaction */
    t = &current_transactions[index];
    t->cid = cid;
    t->resp_bcnt = 0;
    t->resp_size = 0;

    /* Nonce */
    if (nonce != NULL) {
        memcpy(t->nonce, nonce, NONCE_SIZE);
    }
}

void U2FPassthruState::transactionStart(
                                  const struct packet_init *pkt_init)
{
    int64_t time;

    /* Transaction */
    if (pkt_init->cid == BROADCAST_CID) {
        transactionAdd(pkt_init->cid, pkt_init->data);
    } else {
        transactionAdd(pkt_init->cid, NULL);
    }

    /* Time */
    time = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (last_transaction_time == 0) {
        qemu_set_fd_handler(hidraw_fd, U2FPassthruState::readFromHost,
                            NULL, this);
        timer_init_ms(&timer, QEMU_CLOCK_VIRTUAL,
                      U2FPassthruState::timeoutCheck, this);
        timer_mod(&timer, time + TRANSACTION_TIMEOUT / 4);
    }
    last_transaction_time = time;
}

void U2FPassthruState::recvFromHost(
                                    const uint8_t packet[U2FHID_PACKET_SIZE])
{
    struct transaction *t;
    uint32_t cid;

    /* Retrieve transaction */
    cid = packet_get_cid(packet);
    if (cid == BROADCAST_CID) {
        struct packet_init *pkt_init;
        if (!packet_is_init(packet)) {
            return;
        }
        pkt_init = (struct packet_init *)packet;
        t = transactionGetFromNonce(pkt_init->data);
    } else {
        t = transactionGet(cid);
    }

    /* Ignore no started transaction */
    if (t == NULL) {
        return;
    }

    if (packet_is_init(packet)) {
        struct packet_init *pkt_init = (struct packet_init *)packet;
        t->resp_bcnt = packet_init_get_bcnt(pkt_init);
        t->resp_size = PACKET_INIT_DATA_SIZE;

        if (pkt_init->cid == BROADCAST_CID) {
            /* Nonce checking for legitimate response */
            if (memcmp(t->nonce, pkt_init->data, NONCE_SIZE) != 0) {
                return;
            }
        }
    } else {
        t->resp_size += PACKET_CONT_DATA_SIZE;
    }

    /* Transaction end check */
    if (t->resp_size >= t->resp_bcnt) {
        transactionClose(cid);
    }
    u2f_send_to_guest(&base, packet);
}

void U2FPassthruState::readFromHost(void *opaque)
{
    U2FPassthruState *key = static_cast<U2FPassthruState *>(opaque);
    U2FKeyState *base = &key->base;
    uint8_t packet[2 * U2FHID_PACKET_SIZE];
    int ret;

    /* Full size base queue check */
    if (base->pending_in_num >= U2FHID_PENDING_IN_NUM) {
        return;
    }

    ret = read(key->hidraw_fd, packet, sizeof(packet));
    if (ret < 0) {
        /* Detach */
        if (base->dev.attached) {
            usb_device_detach(&base->dev);
            key->resetState();
        }
        return;
    }
    if (ret != U2FHID_PACKET_SIZE) {
        return;
    }
    key->recvFromHost(packet);
}

void U2FPassthruState::recvFromGuest(U2FKeyState *base,
                                    const uint8_t packet[U2FHID_PACKET_SIZE])
{
    U2FPassthruState *key = reinterpret_cast<U2FPassthruState *>(base);
    uint8_t host_packet[U2FHID_PACKET_SIZE + 1];
    ssize_t written;

    if (packet_is_init(packet)) {
        key->transactionStart((struct packet_init *)packet);
    }

    host_packet[0] = 0;
    memcpy(host_packet + 1, packet, U2FHID_PACKET_SIZE);

    written = write(key->hidraw_fd, host_packet, sizeof(host_packet));
    if (written != sizeof(host_packet)) {
        error_report("%s: Bad written size (req 0x%zu, val 0x%zd)",
                     TYPE_U2F_PASSTHRU, sizeof(host_packet), written);
    }
}

bool U2FPassthruState::isU2fDevice(int fd)
{
    int ret, rdesc_size;
    struct hidraw_report_descriptor rdesc;
    const uint8_t u2f_hid_report_desc_header[] = {
        0x06, 0xd0, 0xf1, /* Usage Page (FIDO) */
        0x09, 0x01,       /* Usage (FIDO) */
    };

    /* Get report descriptor size */
    ret = ioctl(fd, HIDIOCGRDESCSIZE, &rdesc_size);
    if (ret < 0 || rdesc_size < sizeof(u2f_hid_report_desc_header)) {
        return false;
    }

    /* Get report descriptor */
    memset(&rdesc, 0x0, sizeof(rdesc));
    rdesc.size = rdesc_size;
    ret = ioctl(fd, HIDIOCGRDESC, &rdesc);
    if (ret < 0) {
        return false;
    }

    /* Header bytes cover specific U2F rdesc values */
    return memcmp(u2f_hid_report_desc_header, rdesc.value,
                  sizeof(u2f_hid_report_desc_header)) == 0;
}

#ifdef CONFIG_LIBUDEV
static int u2f_passthru_open_from_device(struct udev_device *device)
{
    const char *devnode = udev_device_get_devnode(device);

    int fd = qemu_open_old(devnode, O_RDWR);
    if (fd < 0) {
        return -1;
    } else if (!U2FPassthruState::isU2fDevice(fd)) {
        qemu_close(fd);
        return -1;
    }
    return fd;
}

static int u2f_passthru_open_from_enumerate(struct udev *udev,
                                            struct udev_enumerate *enumerate)
{
    struct udev_list_entry *devices, *entry;
    int ret, fd;

    ret = udev_enumerate_scan_devices(enumerate);
    if (ret < 0) {
        return -1;
    }

    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(entry, devices) {
        struct udev_device *device;
        const char *syspath = udev_list_entry_get_name(entry);

        if (syspath == NULL) {
            continue;
        }

        device = udev_device_new_from_syspath(udev, syspath);
        if (device == NULL) {
            continue;
        }

        fd = u2f_passthru_open_from_device(device);
        udev_device_unref(device);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

static int u2f_passthru_open_from_scan(void)
{
    struct udev *udev;
    struct udev_enumerate *enumerate;
    int ret, fd = -1;

    udev = udev_new();
    if (udev == NULL) {
        return -1;
    }

    enumerate = udev_enumerate_new(udev);
    if (enumerate == NULL) {
        udev_unref(udev);
        return -1;
    }

    ret = udev_enumerate_add_match_subsystem(enumerate, "hidraw");
    if (ret >= 0) {
        fd = u2f_passthru_open_from_enumerate(udev, enumerate);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    return fd;
}
#endif

void U2FPassthruState::doUnrealize(U2FKeyState *base)
{
    U2FPassthruState *key = reinterpret_cast<U2FPassthruState *>(base);

    key->resetState();
    qemu_close(key->hidraw_fd);
}

static void u2f_passthru_unrealize(U2FKeyState *base)
{
    U2FPassthruState *key = reinterpret_cast<U2FPassthruState *>(base);
    key->doUnrealize(base);
}

void U2FPassthruState::doRealize(U2FKeyState *base, Error **errp)
{
    U2FPassthruState *key = reinterpret_cast<U2FPassthruState *>(base);
    int fd;

    if (key->hidraw == NULL) {
#ifdef CONFIG_LIBUDEV
        fd = u2f_passthru_open_from_scan();
        if (fd < 0) {
            error_setg(errp, "%s: Failed to find a U2F USB device",
                       TYPE_U2F_PASSTHRU);
            return;
        }
#else
        error_setg(errp, "%s: Missing hidraw", TYPE_U2F_PASSTHRU);
        return;
#endif
    } else {
        fd = qemu_open(key->hidraw, O_RDWR, errp);
        if (fd < 0) {
            return;
        }

        if (!isU2fDevice(fd)) {
            qemu_close(fd);
            error_setg(errp, "%s: Passed hidraw does not represent "
                       "a U2F HID device", TYPE_U2F_PASSTHRU);
            return;
        }
    }
    key->hidraw_fd = fd;
    key->resetState();
}

static void u2f_passthru_realize(U2FKeyState *base, Error **errp)
{
    U2FPassthruState *key = reinterpret_cast<U2FPassthruState *>(base);
    key->doRealize(base, errp);
}

int U2FPassthruState::postLoad(void *opaque, int version_id)
{
    U2FPassthruState *key = static_cast<U2FPassthruState *>(opaque);
    key->resetState();
    return 0;
}

static const VMStateField vmstate_u2f_passthru_fields[] = {
    VMSTATE_U2F_KEY(base, U2FPassthruState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription u2f_passthru_vmstate = {
    .name = "u2f-key-passthru",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = U2FPassthruState::postLoad,
    .fields = vmstate_u2f_passthru_fields
};

static const Property u2f_passthru_properties[] = {
    DEFINE_PROP_STRING("hidraw", U2FPassthruState, hidraw),
};

void U2FPassthruState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    U2FKeyClass *kc = reinterpret_cast<U2FKeyClass *>(klass);

    kc->realize = u2f_passthru_realize;
    kc->unrealize = u2f_passthru_unrealize;
    kc->recv_from_guest = U2FPassthruState::recvFromGuest;
    dc->desc = "QEMU U2F passthrough key";
    dc->vmsd = &u2f_passthru_vmstate;
    device_class_set_props(dc, u2f_passthru_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo u2f_key_passthru_info = {
    .name = TYPE_U2F_PASSTHRU,
    .parent = TYPE_U2F_KEY,
    .instance_size = sizeof(U2FPassthruState),
    .class_init = U2FPassthruState::classInit
};

static void u2f_key_passthru_register_types(void)
{
    type_register_static(&u2f_key_passthru_info);
}

type_init(u2f_key_passthru_register_types)
