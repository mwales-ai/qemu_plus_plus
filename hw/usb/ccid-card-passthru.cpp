/*
 * CCID Passthru Card Device emulation
 *
 * Copyright (c) 2011 Red Hat.
 * Written by Alon Levy.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include <libcacard.h>
#include "chardev/char-fe.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "ccid.h"
#include "qapi/error.h"
#include "qom/object.h"

#define DPRINTF(card, lvl, fmt, ...)                    \
do {                                                    \
    if (lvl <= card->debug) {                           \
        printf("ccid-card-passthru: " fmt , ## __VA_ARGS__);     \
    }                                                   \
} while (0)

#define D_WARN 1
#define D_INFO 2
#define D_MORE_INFO 3
#define D_VERBOSE 4

/* TODO: do we still need this? */
static const uint8_t DEFAULT_ATR[] = {
/*
 * From some example somewhere
 * 0x3B, 0xB0, 0x18, 0x00, 0xD1, 0x81, 0x05, 0xB1, 0x40, 0x38, 0x1F, 0x03, 0x28
 */

/* From an Athena smart card */
 0x3B, 0xD5, 0x18, 0xFF, 0x80, 0x91, 0xFE, 0x1F, 0xC3, 0x80, 0x73, 0xC8, 0x21,
 0x13, 0x08
};

#define VSCARD_IN_SIZE      (64 * KiB)

/* maximum size of ATR - from 7816-3 */
#define MAX_ATR_SIZE        40

typedef struct PassthruState PassthruState;

struct PassthruState {
    CCIDCardState base;
    CharFrontend cs;
    uint8_t  vscard_in_data[VSCARD_IN_SIZE];
    uint32_t vscard_in_pos;
    uint32_t vscard_in_hdr;
    uint8_t  atr[MAX_ATR_SIZE];
    uint8_t  atr_length;
    uint8_t  debug;

    /* methods */
    void sendMsg(VSCMsgType type, uint32_t reader_id,
                 const uint8_t *payload, uint32_t length);
    void sendApdu(const uint8_t *apdu, uint32_t length);
    void sendError(uint32_t reader_id, VSCErrorCode code);
    void sendInit();
    void handleInit(VSCMsgHeader *hdr, VSCMsgInit *init);
    int checkAtr(uint8_t *data, int len);
    void handleMessage(VSCMsgHeader *scr_msg_header);
    void dropConnection();
    void doRealize(CCIDCardState *base, Error **errp);

    /* static callbacks */
    static int canRead(void *opaque);
    static void readData(void *opaque, const uint8_t *buf, int size);
    static void chrEvent(void *opaque, QEMUChrEvent event);
    static void apduFromGuest(CCIDCardState *base,
                              const uint8_t *apdu, uint32_t len);
    static const uint8_t *getAtr(CCIDCardState *base, uint32_t *len);
    static void classInit(ObjectClass *klass, const void *data);
};

#define TYPE_CCID_PASSTHRU "ccid-card-passthru"
DECLARE_INSTANCE_CHECKER(PassthruState, PASSTHRU_CCID_CARD,
                         TYPE_CCID_PASSTHRU)

/*
 * VSCard protocol over chardev
 * This code should not depend on the card type.
 */

void PassthruState::sendMsg(VSCMsgType type, uint32_t reader_id,
                            const uint8_t *payload, uint32_t length)
{
    VSCMsgHeader scr_msg_header;

    scr_msg_header.type = htonl(type);
    scr_msg_header.reader_id = htonl(reader_id);
    scr_msg_header.length = htonl(length);
    /* XXX this blocks entire thread. Rewrite to use
     * qemu_chr_fe_write and background I/O callbacks */
    qemu_chr_fe_write_all(&cs, (uint8_t *)&scr_msg_header,
                          sizeof(VSCMsgHeader));
    qemu_chr_fe_write_all(&cs, payload, length);
}

void PassthruState::sendApdu(const uint8_t *apdu, uint32_t length)
{
    sendMsg(VSC_APDU, VSCARD_MINIMAL_READER_ID, apdu, length);
}

void PassthruState::sendError(uint32_t reader_id, VSCErrorCode code)
{
    VSCMsgError msg = {.code = htonl(code)};

    sendMsg(VSC_Error, reader_id, (uint8_t *)&msg, sizeof(msg));
}

void PassthruState::sendInit()
{
    VSCMsgInit msg = {
        .magic = VSCARD_MAGIC,
        .version = htonl(VSCARD_VERSION),
        .capabilities = {0}
    };

    sendMsg(VSC_Init, VSCARD_UNDEFINED_READER_ID,
            (uint8_t *)&msg, sizeof(msg));
}

int PassthruState::canRead(void *opaque)
{
    PassthruState *card = static_cast<PassthruState *>(opaque);

    return VSCARD_IN_SIZE >= card->vscard_in_pos ?
           VSCARD_IN_SIZE - card->vscard_in_pos : 0;
}

void PassthruState::handleInit(VSCMsgHeader *hdr, VSCMsgInit *init)
{
    uint32_t *capabilities;
    int num_capabilities;
    int i;

    capabilities = init->capabilities;
    num_capabilities =
        1 + ((hdr->length - sizeof(VSCMsgInit)) / sizeof(uint32_t));
    init->version = ntohl(init->version);
    for (i = 0 ; i < num_capabilities; ++i) {
        capabilities[i] = ntohl(capabilities[i]);
    }
    if (init->magic != VSCARD_MAGIC) {
        error_report("wrong magic");
        /* we can't disconnect the chardev */
    }
    if (init->version != VSCARD_VERSION) {
        DPRINTF(this, D_WARN,
            "got version %d, have %d", init->version, VSCARD_VERSION);
    }
    /* future handling of capabilities, none exist atm */
    sendInit();
}

int PassthruState::checkAtr(uint8_t *data, int len)
{
    int historical_length, opt_bytes;
    int td_count = 0;
    int td;

    if (len < 2) {
        return 0;
    }
    historical_length = data[1] & 0xf;
    opt_bytes = 0;
    if (data[0] != 0x3b && data[0] != 0x3f) {
        DPRINTF(this, D_WARN, "atr's T0 is 0x%X, not in {0x3b, 0x3f}\n",
                data[0]);
        return 0;
    }
    td_count = 0;
    td = data[1] >> 4;
    while (td && td_count < 2 && opt_bytes + historical_length + 2 < len) {
        td_count++;
        if (td & 0x1) {
            opt_bytes++;
        }
        if (td & 0x2) {
            opt_bytes++;
        }
        if (td & 0x4) {
            opt_bytes++;
        }
        if (td & 0x8) {
            opt_bytes++;
            td = data[opt_bytes + 2] >> 4;
        }
    }
    if (len < 2 + historical_length + opt_bytes) {
        DPRINTF(this, D_WARN,
            "atr too short: len %d, but historical_len %d, T1 0x%X\n",
            len, historical_length, data[1]);
        return 0;
    }
    if (len > 2 + historical_length + opt_bytes) {
        DPRINTF(this, D_WARN,
            "atr too long: len %d, but hist/opt %d/%d, T1 0x%X\n",
            len, historical_length, opt_bytes, data[1]);
        /* let it through */
    }
    DPRINTF(this, D_VERBOSE,
            "atr passes check: %d total length, %d historical, %d optional\n",
            len, historical_length, opt_bytes);

    return 1;
}

void PassthruState::handleMessage(VSCMsgHeader *scr_msg_header)
{
    uint8_t *data = (uint8_t *)&scr_msg_header[1];

    switch (scr_msg_header->type) {
    case VSC_ATR:
        DPRINTF(this, D_INFO, "VSC_ATR %d\n", scr_msg_header->length);
        if (scr_msg_header->length > MAX_ATR_SIZE) {
            error_report("ATR size exceeds spec, ignoring");
            sendError(scr_msg_header->reader_id, VSC_GENERAL_ERROR);
            break;
        }
        if (!checkAtr(data, scr_msg_header->length)) {
            error_report("ATR is inconsistent, ignoring");
            sendError(scr_msg_header->reader_id, VSC_GENERAL_ERROR);
            break;
        }
        memcpy(atr, data, scr_msg_header->length);
        atr_length = scr_msg_header->length;
        ccid_card_card_inserted(&base);
        sendError(scr_msg_header->reader_id, VSC_SUCCESS);
        break;
    case VSC_APDU:
        ccid_card_send_apdu_to_guest(&base, data, scr_msg_header->length);
        break;
    case VSC_CardRemove:
        DPRINTF(this, D_INFO, "VSC_CardRemove\n");
        ccid_card_card_removed(&base);
        sendError(scr_msg_header->reader_id, VSC_SUCCESS);
        break;
    case VSC_Init:
        handleInit(scr_msg_header, (VSCMsgInit *)data);
        break;
    case VSC_Error:
        ccid_card_card_error(&base, *(uint32_t *)data);
        break;
    case VSC_ReaderAdd:
        if (ccid_card_ccid_attach(&base) < 0) {
            sendError(VSCARD_UNDEFINED_READER_ID,
                      VSC_CANNOT_ADD_MORE_READERS);
        } else {
            sendError(VSCARD_MINIMAL_READER_ID, VSC_SUCCESS);
        }
        break;
    case VSC_ReaderRemove:
        ccid_card_ccid_detach(&base);
        sendError(scr_msg_header->reader_id, VSC_SUCCESS);
        break;
    default:
        printf("usb-ccid: chardev: unexpected message of type %X\n",
               scr_msg_header->type);
        sendError(scr_msg_header->reader_id, VSC_GENERAL_ERROR);
    }
}

void PassthruState::dropConnection()
{
    qemu_chr_fe_deinit(&cs, true);
    vscard_in_pos = vscard_in_hdr = 0;
}

void PassthruState::readData(void *opaque, const uint8_t *buf, int size)
{
    PassthruState *card = static_cast<PassthruState *>(opaque);
    VSCMsgHeader *hdr;

    if (card->vscard_in_pos + size > VSCARD_IN_SIZE) {
        error_report("no room for data: pos %u +  size %d > %" PRId64 "."
                     " dropping connection.",
                     card->vscard_in_pos, size, VSCARD_IN_SIZE);
        card->dropConnection();
        return;
    }
    assert(card->vscard_in_pos < VSCARD_IN_SIZE);
    assert(card->vscard_in_hdr < VSCARD_IN_SIZE);
    memcpy(card->vscard_in_data + card->vscard_in_pos, buf, size);
    card->vscard_in_pos += size;
    hdr = (VSCMsgHeader *)(card->vscard_in_data + card->vscard_in_hdr);

    while ((card->vscard_in_pos - card->vscard_in_hdr >= sizeof(VSCMsgHeader))
         &&(card->vscard_in_pos - card->vscard_in_hdr >=
                                  sizeof(VSCMsgHeader) + ntohl(hdr->length))) {
        hdr->reader_id = ntohl(hdr->reader_id);
        hdr->length = ntohl(hdr->length);
        hdr->type = ntohl(hdr->type);
        card->handleMessage(hdr);
        card->vscard_in_hdr += hdr->length + sizeof(VSCMsgHeader);
        hdr = (VSCMsgHeader *)(card->vscard_in_data + card->vscard_in_hdr);
    }
    if (card->vscard_in_hdr == card->vscard_in_pos) {
        card->vscard_in_pos = card->vscard_in_hdr = 0;
    }
}

void PassthruState::chrEvent(void *opaque, QEMUChrEvent event)
{
    PassthruState *card = static_cast<PassthruState *>(opaque);

    switch (event) {
    case CHR_EVENT_BREAK:
        card->vscard_in_pos = card->vscard_in_hdr = 0;
        break;
    case CHR_EVENT_OPENED:
        DPRINTF(card, D_INFO, "%s: CHR_EVENT_OPENED\n", __func__);
        break;
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
    case CHR_EVENT_CLOSED:
        /* Ignore */
        break;
    }
}

/* End VSCard handling */

void PassthruState::apduFromGuest(CCIDCardState *base,
                                  const uint8_t *apdu, uint32_t len)
{
    PassthruState *card = PASSTHRU_CCID_CARD(base);

    if (!qemu_chr_fe_backend_connected(&card->cs)) {
        printf("ccid-passthru: no chardev, discarding apdu length %u\n", len);
        return;
    }
    card->sendApdu(apdu, len);
}

const uint8_t *PassthruState::getAtr(CCIDCardState *base, uint32_t *len)
{
    PassthruState *card = PASSTHRU_CCID_CARD(base);

    *len = card->atr_length;
    return card->atr;
}

void PassthruState::doRealize(CCIDCardState *base, Error **errp)
{
    PassthruState *card = PASSTHRU_CCID_CARD(base);

    card->vscard_in_pos = 0;
    card->vscard_in_hdr = 0;
    if (qemu_chr_fe_backend_connected(&card->cs)) {
        DPRINTF(card, D_INFO, "ccid-card-passthru: initing chardev");
        qemu_chr_fe_set_handlers(&card->cs,
            PassthruState::canRead,
            PassthruState::readData,
            PassthruState::chrEvent, NULL, card, NULL, true);
        card->sendInit();
    } else {
        error_setg(errp, "missing chardev");
        return;
    }
    card->debug = parse_debug_env("QEMU_CCID_PASSTHRU_DEBUG", D_VERBOSE,
                                  card->debug);
    assert(sizeof(DEFAULT_ATR) <= MAX_ATR_SIZE);
    memcpy(card->atr, DEFAULT_ATR, sizeof(DEFAULT_ATR));
    card->atr_length = sizeof(DEFAULT_ATR);
}

static void passthru_realize(CCIDCardState *base, Error **errp)
{
    PassthruState *card = PASSTHRU_CCID_CARD(base);
    card->doRealize(base, errp);
}

static const VMStateField vmstate_passthru_fields[] = {
    VMSTATE_BUFFER(vscard_in_data, PassthruState),
    VMSTATE_UINT32(vscard_in_pos, PassthruState),
    VMSTATE_UINT32(vscard_in_hdr, PassthruState),
    VMSTATE_BUFFER(atr, PassthruState),
    VMSTATE_UINT8(atr_length, PassthruState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription passthru_vmstate = {
    .name = "ccid-card-passthru",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_passthru_fields
};

static const Property passthru_card_properties[] = {
    DEFINE_PROP_CHR("chardev", PassthruState, cs),
    DEFINE_PROP_UINT8("debug", PassthruState, debug, 0),
};

void PassthruState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CCIDCardClass *cc = CCID_CARD_CLASS(klass);

    cc->realize = passthru_realize;
    cc->get_atr = PassthruState::getAtr;
    cc->apdu_from_guest = PassthruState::apduFromGuest;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "passthrough smartcard";
    dc->vmsd = &passthru_vmstate;
    device_class_set_props(dc, passthru_card_properties);
}

static const TypeInfo passthru_card_info = {
    .name          = TYPE_CCID_PASSTHRU,
    .parent        = TYPE_CCID_CARD,
    .instance_size = sizeof(PassthruState),
    .class_init    = PassthruState::classInit,
};
module_obj(TYPE_CCID_PASSTHRU);
module_kconfig(USB);

static void ccid_card_passthru_register_types(void)
{
    type_register_static(&passthru_card_info);
}

type_init(ccid_card_passthru_register_types)
