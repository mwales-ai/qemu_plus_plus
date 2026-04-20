/*
 * WM8750 audio CODEC.
 *
 * Copyright (c) 2006 Openedhand Ltd.
 * Written by Andrzej Zaborowski <balrog@zabor.org>
 *
 * This file is licensed under GNU GPL.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/audio/wm8750.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define IN_PORT_N	3
#define OUT_PORT_N	3

#define CODEC		"wm8750"

typedef struct {
    int adc;
    int adc_hz;
    int dac;
    int dac_hz;
} WMRate;

OBJECT_DECLARE_SIMPLE_TYPE(WM8750State, WM8750)

struct WM8750State {
    I2CSlave parent_obj;

    uint8_t i2c_data[2];
    int i2c_len;
    AudioBackend *audio_be;
    SWVoiceIn *adc_voice[IN_PORT_N];
    SWVoiceOut *dac_voice[OUT_PORT_N];
    int enable;
    void (*data_req)(void *, int, int);
    void *opaque;
    uint8_t data_in[4096];
    uint8_t data_out[4096];
    int idx_in, req_in;
    int idx_out, req_out;

    SWVoiceOut **out[2];
    uint8_t outvol[7], outmute[2];
    SWVoiceIn **in[2];
    uint8_t invol[4], inmute[2];

    uint8_t diff[2], pol, ds, monomix[2], alc, mute;
    uint8_t path[4], mpath[2], power, format;
    const WMRate *rate;
    uint8_t rate_vmstate;
    int adc_hz, dac_hz, ext_adc_hz, ext_dac_hz, master;

    /* methods */
    void volUpdate();
    void setFormat();
    void clkUpdate(int ext);
    void resetState();

    static void audioInCb(void *opaque, int avail_b);
    static void audioOutCb(void *opaque, int free_b);
    static int preSave(void *opaque);
    static int postLoad(void *opaque, int version_id);

    void doRealize(Error **errp);
    static void realizeWrapper(DeviceState *dev, Error **errp);

    static void classInit(DeviceClass *dc);
};

/* pow(10.0, -i / 20.0) * 255, i = 0..42 */
static const uint8_t wm8750_vol_db_table[] = {
    255, 227, 203, 181, 161, 143, 128, 114, 102, 90, 81, 72, 64, 57, 51, 45,
    40, 36, 32, 29, 26, 23, 20, 18, 16, 14, 13, 11, 10, 9, 8, 7, 6, 6, 5, 5,
    4, 4, 3, 3, 3, 2, 2
};

#define WM8750_OUTVOL_TRANSFORM(x)	wm8750_vol_db_table[(0x7f - x) / 3]
#define WM8750_INVOL_TRANSFORM(x)	(x << 2)

static inline void wm8750_in_load(WM8750State *s)
{
    if (s->idx_in + s->req_in <= sizeof(s->data_in))
        return;
    s->idx_in = MAX(0, (int) sizeof(s->data_in) - s->req_in);
    AUD_read(*s->in[0], s->data_in + s->idx_in,
             sizeof(s->data_in) - s->idx_in);
}

static inline void wm8750_out_flush(WM8750State *s)
{
    int sent = 0;
    while (sent < s->idx_out)
        sent += AUD_write(*s->out[0], s->data_out + sent, s->idx_out - sent)
                ?: s->idx_out;
    s->idx_out = 0;
}

void WM8750State::audioInCb(void *opaque, int avail_b)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);
    s->req_in = avail_b;
    s->data_req(s->opaque, s->req_out >> 2, avail_b >> 2);
}

void WM8750State::audioOutCb(void *opaque, int free_b)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);

    if (s->idx_out >= free_b) {
        s->idx_out = free_b;
        s->req_out = 0;
        wm8750_out_flush(s);
    } else
        s->req_out = free_b - s->idx_out;

    s->data_req(s->opaque, s->req_out >> 2, s->req_in >> 2);
}

static const WMRate wm_rate_table[] = {
    {  256, 48000,  256, 48000 },	/* SR: 00000 */
    {  384, 48000,  384, 48000 },	/* SR: 00001 */
    {  256, 48000, 1536,  8000 },	/* SR: 00010 */
    {  384, 48000, 2304,  8000 },	/* SR: 00011 */
    { 1536,  8000,  256, 48000 },	/* SR: 00100 */
    { 2304,  8000,  384, 48000 },	/* SR: 00101 */
    { 1536,  8000, 1536,  8000 },	/* SR: 00110 */
    { 2304,  8000, 2304,  8000 },	/* SR: 00111 */
    { 1024, 12000, 1024, 12000 },	/* SR: 01000 */
    { 1526, 12000, 1536, 12000 },	/* SR: 01001 */
    {  768, 16000,  768, 16000 },	/* SR: 01010 */
    { 1152, 16000, 1152, 16000 },	/* SR: 01011 */
    {  384, 32000,  384, 32000 },	/* SR: 01100 */
    {  576, 32000,  576, 32000 },	/* SR: 01101 */
    {  128, 96000,  128, 96000 },	/* SR: 01110 */
    {  192, 96000,  192, 96000 },	/* SR: 01111 */
    {  256, 44100,  256, 44100 },	/* SR: 10000 */
    {  384, 44100,  384, 44100 },	/* SR: 10001 */
    {  256, 44100, 1408,  8018 },	/* SR: 10010 */
    {  384, 44100, 2112,  8018 },	/* SR: 10011 */
    { 1408,  8018,  256, 44100 },	/* SR: 10100 */
    { 2112,  8018,  384, 44100 },	/* SR: 10101 */
    { 1408,  8018, 1408,  8018 },	/* SR: 10110 */
    { 2112,  8018, 2112,  8018 },	/* SR: 10111 */
    { 1024, 11025, 1024, 11025 },	/* SR: 11000 */
    { 1536, 11025, 1536, 11025 },	/* SR: 11001 */
    {  512, 22050,  512, 22050 },	/* SR: 11010 */
    {  768, 22050,  768, 22050 },	/* SR: 11011 */
    {  512, 24000,  512, 24000 },	/* SR: 11100 */
    {  768, 24000,  768, 24000 },	/* SR: 11101 */
    {  128, 88200,  128, 88200 },	/* SR: 11110 */
    {  192, 88200,  192, 88200 },	/* SR: 11111 */
};

void WM8750State::volUpdate()
{
    /* FIXME: multiply all volumes by invol[2], invol[3] */

    AUD_set_volume_in_lr(adc_voice[0], mute,
                    inmute[0] ? 0 : WM8750_INVOL_TRANSFORM(invol[0]),
                    inmute[1] ? 0 : WM8750_INVOL_TRANSFORM(invol[1]));
    AUD_set_volume_in_lr(adc_voice[1], mute,
                    inmute[0] ? 0 : WM8750_INVOL_TRANSFORM(invol[0]),
                    inmute[1] ? 0 : WM8750_INVOL_TRANSFORM(invol[1]));
    AUD_set_volume_in_lr(adc_voice[2], mute,
                    inmute[0] ? 0 : WM8750_INVOL_TRANSFORM(invol[0]),
                    inmute[1] ? 0 : WM8750_INVOL_TRANSFORM(invol[1]));

    /* FIXME: multiply all volumes by outvol[0], outvol[1] */

    /* Speaker: LOUT2VOL ROUT2VOL */
    AUD_set_volume_out_lr(dac_voice[0], mute,
                    outmute[0] ? 0 : WM8750_OUTVOL_TRANSFORM(outvol[4]),
                    outmute[1] ? 0 : WM8750_OUTVOL_TRANSFORM(outvol[5]));

    /* Headphone: LOUT1VOL ROUT1VOL */
    AUD_set_volume_out_lr(dac_voice[1], mute,
                    outmute[0] ? 0 : WM8750_OUTVOL_TRANSFORM(outvol[2]),
                    outmute[1] ? 0 : WM8750_OUTVOL_TRANSFORM(outvol[3]));

    /* MONOOUT: MONOVOL MONOVOL */
    AUD_set_volume_out_lr(dac_voice[2], mute,
                    outmute[0] ? 0 : WM8750_OUTVOL_TRANSFORM(outvol[6]),
                    outmute[1] ? 0 : WM8750_OUTVOL_TRANSFORM(outvol[6]));
}

void WM8750State::setFormat()
{
    int i;
    struct audsettings in_fmt;
    struct audsettings out_fmt;

    wm8750_out_flush(this);

    if (in[0] && *in[0])
        AUD_set_active_in(*in[0], 0);
    if (out[0] && *out[0])
        AUD_set_active_out(*out[0], 0);

    for (i = 0; i < IN_PORT_N; i ++)
        if (adc_voice[i]) {
            AUD_close_in(audio_be, adc_voice[i]);
            adc_voice[i] = NULL;
        }
    for (i = 0; i < OUT_PORT_N; i ++)
        if (dac_voice[i]) {
            AUD_close_out(audio_be, dac_voice[i]);
            dac_voice[i] = NULL;
        }

    if (!enable)
        return;

    /* Setup input */
    in_fmt.endianness = 0;
    in_fmt.nchannels = 2;
    in_fmt.freq = adc_hz;
    in_fmt.fmt = AUDIO_FORMAT_S16;

    adc_voice[0] = AUD_open_in(audio_be, adc_voice[0],
                    CODEC ".input1", this, WM8750State::audioInCb, &in_fmt);
    adc_voice[1] = AUD_open_in(audio_be, adc_voice[1],
                    CODEC ".input2", this, WM8750State::audioInCb, &in_fmt);
    adc_voice[2] = AUD_open_in(audio_be, adc_voice[2],
                    CODEC ".input3", this, WM8750State::audioInCb, &in_fmt);

    /* Setup output */
    out_fmt.endianness = 0;
    out_fmt.nchannels = 2;
    out_fmt.freq = dac_hz;
    out_fmt.fmt = AUDIO_FORMAT_S16;

    dac_voice[0] = AUD_open_out(audio_be, dac_voice[0],
                    CODEC ".speaker", this, WM8750State::audioOutCb, &out_fmt);
    dac_voice[1] = AUD_open_out(audio_be, dac_voice[1],
                    CODEC ".headphone", this, WM8750State::audioOutCb, &out_fmt);
    /* MONOMIX is also in stereo for simplicity */
    dac_voice[2] = AUD_open_out(audio_be, dac_voice[2],
                    CODEC ".monomix", this, WM8750State::audioOutCb, &out_fmt);
    /* no sense emulating OUT3 which is a mix of other outputs */

    volUpdate();

    /* We should connect the left and right channels to their
     * respective inputs/outputs but we have completely no need
     * for mixing or combining paths to different ports, so we
     * connect both channels to where the left channel is routed.  */
    if (in[0] && *in[0])
        AUD_set_active_in(*in[0], 1);
    if (out[0] && *out[0])
        AUD_set_active_out(*out[0], 1);
}

void WM8750State::clkUpdate(int ext)
{
    if (master || !ext_dac_hz)
        dac_hz = rate->dac_hz;
    else
        dac_hz = ext_dac_hz;

    if (master || !ext_adc_hz)
        adc_hz = rate->adc_hz;
    else
        adc_hz = ext_adc_hz;

    if (master || (!ext_dac_hz && !ext_adc_hz)) {
        if (!ext)
            setFormat();
    } else {
        if (ext)
            setFormat();
    }
}

void WM8750State::resetState()
{
    rate = &wm_rate_table[0];
    enable = 0;
    clkUpdate(1);
    diff[0] = 0;
    diff[1] = 0;
    ds = 0;
    alc = 0;
    in[0] = &adc_voice[0];
    invol[0] = 0x17;
    invol[1] = 0x17;
    invol[2] = 0xc3;
    invol[3] = 0xc3;
    out[0] = &dac_voice[0];
    outvol[0] = 0xff;
    outvol[1] = 0xff;
    outvol[2] = 0x79;
    outvol[3] = 0x79;
    outvol[4] = 0x79;
    outvol[5] = 0x79;
    outvol[6] = 0x79;
    inmute[0] = 0;
    inmute[1] = 0;
    outmute[0] = 0;
    outmute[1] = 0;
    mute = 1;
    path[0] = 0;
    path[1] = 0;
    path[2] = 0;
    path[3] = 0;
    mpath[0] = 0;
    mpath[1] = 0;
    format = 0x0a;
    idx_in = sizeof(data_in);
    req_in = 0;
    idx_out = 0;
    req_out = 0;
    volUpdate();
    i2c_len = 0;
}

static int wm8750_event(I2CSlave *i2c, enum i2c_event event)
{
    WM8750State *s = WM8750(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->i2c_len = 0;
        break;
    case I2C_FINISH:
#ifdef VERBOSE
        if (s->i2c_len < 2)
            printf("%s: message too short (%i bytes)\n",
                            __func__, s->i2c_len);
#endif
        break;
    default:
        break;
    }

    return 0;
}

#define WM8750_LINVOL	0x00
#define WM8750_RINVOL	0x01
#define WM8750_LOUT1V	0x02
#define WM8750_ROUT1V	0x03
#define WM8750_ADCDAC	0x05
#define WM8750_IFACE	0x07
#define WM8750_SRATE	0x08
#define WM8750_LDAC	0x0a
#define WM8750_RDAC	0x0b
#define WM8750_BASS	0x0c
#define WM8750_TREBLE	0x0d
#define WM8750_RESET	0x0f
#define WM8750_3D	0x10
#define WM8750_ALC1	0x11
#define WM8750_ALC2	0x12
#define WM8750_ALC3	0x13
#define WM8750_NGATE	0x14
#define WM8750_LADC	0x15
#define WM8750_RADC	0x16
#define WM8750_ADCTL1	0x17
#define WM8750_ADCTL2	0x18
#define WM8750_PWR1	0x19
#define WM8750_PWR2	0x1a
#define WM8750_ADCTL3	0x1b
#define WM8750_ADCIN	0x1f
#define WM8750_LADCIN	0x20
#define WM8750_RADCIN	0x21
#define WM8750_LOUTM1	0x22
#define WM8750_LOUTM2	0x23
#define WM8750_ROUTM1	0x24
#define WM8750_ROUTM2	0x25
#define WM8750_MOUTM1	0x26
#define WM8750_MOUTM2	0x27
#define WM8750_LOUT2V	0x28
#define WM8750_ROUT2V	0x29
#define WM8750_MOUTV	0x2a

static int wm8750_tx(I2CSlave *i2c, uint8_t data)
{
    WM8750State *s = WM8750(i2c);
    uint8_t cmd;
    uint16_t value;

    if (s->i2c_len >= 2) {
#ifdef VERBOSE
        printf("%s: long message (%i bytes)\n", __func__, s->i2c_len);
#endif
        return 1;
    }
    s->i2c_data[s->i2c_len ++] = data;
    if (s->i2c_len != 2)
        return 0;

    cmd = s->i2c_data[0] >> 1;
    value = ((s->i2c_data[0] << 8) | s->i2c_data[1]) & 0x1ff;

    switch (cmd) {
    case WM8750_LADCIN:	/* ADC Signal Path Control (Left) */
        s->diff[0] = (((value >> 6) & 3) == 3);	/* LINSEL */
        if (s->diff[0])
            s->in[0] = &s->adc_voice[0 + s->ds * 1];
        else
            s->in[0] = &s->adc_voice[((value >> 6) & 3) * 1 + 0];
        break;

    case WM8750_RADCIN:	/* ADC Signal Path Control (Right) */
        s->diff[1] = (((value >> 6) & 3) == 3);	/* RINSEL */
        if (s->diff[1])
            s->in[1] = &s->adc_voice[0 + s->ds * 1];
        else
            s->in[1] = &s->adc_voice[((value >> 6) & 3) * 1 + 0];
        break;

    case WM8750_ADCIN:	/* ADC Input Mode */
        s->ds = (value >> 8) & 1;	/* DS */
        if (s->diff[0])
            s->in[0] = &s->adc_voice[0 + s->ds * 1];
        if (s->diff[1])
            s->in[1] = &s->adc_voice[0 + s->ds * 1];
        s->monomix[0] = (value >> 6) & 3;	/* MONOMIX */
        break;

    case WM8750_ADCTL1:	/* Additional Control (1) */
        s->monomix[1] = (value >> 1) & 1;	/* DMONOMIX */
        break;

    case WM8750_PWR1:	/* Power Management (1) */
        s->enable = ((value >> 6) & 7) == 3;	/* VMIDSEL, VREF */
        s->setFormat();
        break;

    case WM8750_LINVOL:	/* Left Channel PGA */
        s->invol[0] = value & 0x3f;		/* LINVOL */
        s->inmute[0] = (value >> 7) & 1;	/* LINMUTE */
        s->volUpdate();
        break;

    case WM8750_RINVOL:	/* Right Channel PGA */
        s->invol[1] = value & 0x3f;		/* RINVOL */
        s->inmute[1] = (value >> 7) & 1;	/* RINMUTE */
        s->volUpdate();
        break;

    case WM8750_ADCDAC:	/* ADC and DAC Control */
        s->pol = (value >> 5) & 3;		/* ADCPOL */
        s->mute = (value >> 3) & 1;		/* DACMU */
        s->volUpdate();
        break;

    case WM8750_ADCTL3:	/* Additional Control (3) */
        break;

    case WM8750_LADC:	/* Left ADC Digital Volume */
        s->invol[2] = value & 0xff;		/* LADCVOL */
        s->volUpdate();
        break;

    case WM8750_RADC:	/* Right ADC Digital Volume */
        s->invol[3] = value & 0xff;		/* RADCVOL */
        s->volUpdate();
        break;

    case WM8750_ALC1:	/* ALC Control (1) */
        s->alc = (value >> 7) & 3;		/* ALCSEL */
        break;

    case WM8750_NGATE:	/* Noise Gate Control */
    case WM8750_3D:	/* 3D enhance */
        break;

    case WM8750_LDAC:	/* Left Channel Digital Volume */
        s->outvol[0] = value & 0xff;		/* LDACVOL */
        s->volUpdate();
        break;

    case WM8750_RDAC:	/* Right Channel Digital Volume */
        s->outvol[1] = value & 0xff;		/* RDACVOL */
        s->volUpdate();
        break;

    case WM8750_BASS:	/* Bass Control */
        break;

    case WM8750_LOUTM1:	/* Left Mixer Control (1) */
        s->path[0] = (value >> 8) & 1;		/* LD2LO */
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_LOUTM2:	/* Left Mixer Control (2) */
        s->path[1] = (value >> 8) & 1;		/* RD2LO */
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_ROUTM1:	/* Right Mixer Control (1) */
        s->path[2] = (value >> 8) & 1;		/* LD2RO */
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_ROUTM2:	/* Right Mixer Control (2) */
        s->path[3] = (value >> 8) & 1;		/* RD2RO */
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_MOUTM1:	/* Mono Mixer Control (1) */
        s->mpath[0] = (value >> 8) & 1;		/* LD2MO */
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_MOUTM2:	/* Mono Mixer Control (2) */
        s->mpath[1] = (value >> 8) & 1;		/* RD2MO */
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_LOUT1V:	/* LOUT1 Volume */
        s->outvol[2] = value & 0x7f;		/* LOUT1VOL */
        s->volUpdate();
        break;

    case WM8750_LOUT2V:	/* LOUT2 Volume */
        s->outvol[4] = value & 0x7f;		/* LOUT2VOL */
        s->volUpdate();
        break;

    case WM8750_ROUT1V:	/* ROUT1 Volume */
        s->outvol[3] = value & 0x7f;		/* ROUT1VOL */
        s->volUpdate();
        break;

    case WM8750_ROUT2V:	/* ROUT2 Volume */
        s->outvol[5] = value & 0x7f;		/* ROUT2VOL */
        s->volUpdate();
        break;

    case WM8750_MOUTV:	/* MONOOUT Volume */
        s->outvol[6] = value & 0x7f;		/* MONOOUTVOL */
        s->volUpdate();
        break;

    case WM8750_ADCTL2:	/* Additional Control (2) */
        break;

    case WM8750_PWR2:	/* Power Management (2) */
        s->power = value & 0x7e;
        /* TODO: mute/unmute respective paths */
        s->volUpdate();
        break;

    case WM8750_IFACE:	/* Digital Audio Interface Format */
        s->format = value;
        s->master = (value >> 6) & 1;			/* MS */
        s->clkUpdate(s->master);
        break;

    case WM8750_SRATE:	/* Clocking and Sample Rate Control */
        s->rate = &wm_rate_table[(value >> 1) & 0x1f];
        s->clkUpdate(0);
        break;

    case WM8750_RESET:	/* Reset */
        s->resetState();
        break;

#ifdef VERBOSE
    default:
        printf("%s: unknown register %02x\n", __func__, cmd);
#endif
    }

    return 0;
}

static uint8_t wm8750_rx(I2CSlave *i2c)
{
    return 0x00;
}

int WM8750State::preSave(void *opaque)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);

    s->rate_vmstate = s->rate - wm_rate_table;

    return 0;
}

int __attribute__((used)) WM8750State::postLoad(void *opaque, int version_id)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);

    s->rate = &wm_rate_table[s->rate_vmstate & 0x1f];
    return 0;
}

static const VMStateField vmstate_wm8750_fields[] = {
    VMSTATE_UINT8_ARRAY(i2c_data, WM8750State, 2),
    VMSTATE_INT32(i2c_len, WM8750State),
    VMSTATE_INT32(enable, WM8750State),
    VMSTATE_INT32(idx_in, WM8750State),
    VMSTATE_INT32(req_in, WM8750State),
    VMSTATE_INT32(idx_out, WM8750State),
    VMSTATE_INT32(req_out, WM8750State),
    VMSTATE_UINT8_ARRAY(outvol, WM8750State, 7),
    VMSTATE_UINT8_ARRAY(outmute, WM8750State, 2),
    VMSTATE_UINT8_ARRAY(invol, WM8750State, 4),
    VMSTATE_UINT8_ARRAY(inmute, WM8750State, 2),
    VMSTATE_UINT8_ARRAY(diff, WM8750State, 2),
    VMSTATE_UINT8(pol, WM8750State),
    VMSTATE_UINT8(ds, WM8750State),
    VMSTATE_UINT8_ARRAY(monomix, WM8750State, 2),
    VMSTATE_UINT8(alc, WM8750State),
    VMSTATE_UINT8(mute, WM8750State),
    VMSTATE_UINT8_ARRAY(path, WM8750State, 4),
    VMSTATE_UINT8_ARRAY(mpath, WM8750State, 2),
    VMSTATE_UINT8(format, WM8750State),
    VMSTATE_UINT8(power, WM8750State),
    VMSTATE_UINT8(rate_vmstate, WM8750State),
    VMSTATE_I2C_SLAVE(parent_obj, WM8750State),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_wm8750 = {
    .name = CODEC,
    .version_id = 0,
    .minimum_version_id = 0,
    .post_load = WM8750State::postLoad,
    .pre_save = WM8750State::preSave,
    .fields = vmstate_wm8750_fields,
};

void WM8750State::realizeWrapper(DeviceState *dev, Error **errp)
{
    WM8750State *s = WM8750(dev);
    s->doRealize(errp);
}

void WM8750State::doRealize(Error **errp)
{
    if (!AUD_backend_check(&audio_be, errp)) {
        return;
    }

    resetState();
}

#if 0
static void wm8750_fini(I2CSlave *i2c)
{
    WM8750State *s = WM8750(i2c);

    s->resetState();
    g_free(s);
}
#endif

void wm8750_data_req_set(DeviceState *dev, data_req_cb *data_req, void *opaque)
{
    WM8750State *s = WM8750(dev);

    s->data_req = data_req;
    s->opaque = opaque;
}

void wm8750_dac_dat(void *opaque, uint32_t sample)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);

    *(uint32_t *) &s->data_out[s->idx_out] = sample;
    s->req_out -= 4;
    s->idx_out += 4;
    if (s->idx_out >= sizeof(s->data_out) || s->req_out <= 0)
        wm8750_out_flush(s);
}

void *wm8750_dac_buffer(void *opaque, int samples)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);
    /* XXX: Should check if there are <i>samples</i> free samples available */
    void *ret = s->data_out + s->idx_out;

    s->idx_out += samples << 2;
    s->req_out -= samples << 2;
    return ret;
}

void wm8750_dac_commit(void *opaque)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);

    wm8750_out_flush(s);
}

uint32_t wm8750_adc_dat(void *opaque)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);
    uint32_t *data;

    if (s->idx_in >= sizeof(s->data_in)) {
        wm8750_in_load(s);
        if (s->idx_in >= sizeof(s->data_in)) {
            return 0x80008000; /* silence in AUDIO_FORMAT_S16 sample format */
        }
    }

    data = (uint32_t *) &s->data_in[s->idx_in];
    s->req_in -= 4;
    s->idx_in += 4;
    return *data;
}

void wm8750_set_bclk_in(void *opaque, int new_hz)
{
    WM8750State *s = static_cast<WM8750State *>(opaque);

    s->ext_adc_hz = new_hz;
    s->ext_dac_hz = new_hz;
    s->clkUpdate(1);
}

static const Property wm8750_properties[] = {
    DEFINE_AUDIO_PROPERTIES(WM8750State, audio_be),
};

void WM8750State::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    I2CSlaveClass *sc = reinterpret_cast<I2CSlaveClass *>(klass);

    dc->realize = WM8750State::realizeWrapper;
    sc->event = wm8750_event;
    sc->recv = wm8750_rx;
    sc->send = wm8750_tx;
    dc->vmsd = &vmstate_wm8750;
    device_class_set_props(dc, wm8750_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(WM8750State, TYPE_WM8750, TYPE_I2C_SLAVE)
