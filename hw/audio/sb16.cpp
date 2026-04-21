/*
 * QEMU Soundblaster 16 emulation
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
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
#include "hw/audio/model.h"
#include "qemu/audio.h"
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#define DEBUG 0
/* #define DEBUG_SB16_MOST */

#define ldebug(fmt, ...) do { \
        if (DEBUG) { \
            error_report("sb16: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

static const char e3[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

#define TYPE_SB16 "sb16"
OBJECT_DECLARE_SIMPLE_TYPE(SB16State, SB16)

static void SB_audio_callback (void *opaque, int free);

struct SB16State {
    ISADevice parent_obj;

    AudioBackend *audio_be;
    qemu_irq pic;
    uint32_t irq;
    uint32_t dma;
    uint32_t hdma;
    uint32_t port;
    uint32_t ver;
    IsaDma *isa_dma;
    IsaDma *isa_hdma;

    int in_index;
    int out_data_len;
    int fmt_stereo;
    int fmt_signed;
    int fmt_bits;
    AudioFormat fmt;
    int dma_auto;
    int block_size;
    int fifo;
    int freq;
    int time_const;
    int speaker;
    int needed_bytes;
    int cmd;
    int use_hdma;
    int highspeed;
    int can_write;

    int v2x6;

    uint8_t csp_param;
    uint8_t csp_value;
    uint8_t csp_mode;
    uint8_t csp_regs[256];
    uint8_t csp_index;
    uint8_t csp_reg83[4];
    int csp_reg83r;
    int csp_reg83w;

    uint8_t in2_data[10];
    uint8_t out_data[50];
    uint8_t test_reg;
    uint8_t last_read_byte;
    int nzero;

    int left_till_irq;

    int dma_running;
    int bytes_per_second;
    int align;
    int audio_free;
    SWVoiceOut *voice;

    QEMUTimer *aux_ts;
    /* mixer state */
    int mixer_nreg;
    uint8_t mixer_regs[256];
    PortioList portio_list;

    /* internal methods */
    void setSpeaker(int on);
    void setControl(int hold);
    void continueDma8();
    void dmaCmd8(int mask, int dma_len);
    void dmaCmd(uint8_t cmd, uint8_t d0, int dma_len);
    inline void dspOutData(uint8_t val);
    inline uint8_t dspGetData();
    void handleCommand(uint8_t cmd);
    uint16_t dspGetLohi();
    uint16_t dspGetHilo();
    void handleComplete();
    void legacyReset();
    void doReset();
    void resetMixer();
    int writeAudio(int nchan, int dma_pos, int dma_len, int len);
    void postLoad();

    /* QOM methods */
    void init();
    void realize(Error **errp);
    static void classInit(DeviceClass *dc);
};

#define SAMPLE_RATE_MIN 5000
#define SAMPLE_RATE_MAX 45000

static int magic_of_irq (int irq)
{
    switch (irq) {
    case 5:
        return 2;
    case 7:
        return 4;
    case 9:
        return 1;
    case 10:
        return 8;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq %d\n", irq);
        return 2;
    }
}

static int irq_of_magic (int magic)
{
    switch (magic) {
    case 1:
        return 9;
    case 2:
        return 5;
    case 4:
        return 7;
    case 8:
        return 10;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq magic %d\n", magic);
        return -1;
    }
}

#if 0
static void log_dsp (SB16State *dsp)
{
    ldebug("%s:%s:%d:%s:dmasize=%d:freq=%d:const=%d:speaker=%d",
            dsp->fmt_stereo ? "Stereo" : "Mono",
            dsp->fmt_signed ? "Signed" : "Unsigned",
            dsp->fmt_bits,
            dsp->dma_auto ? "Auto" : "Single",
            dsp->block_size,
            dsp->freq,
            dsp->time_const,
            dsp->speaker);
}
#endif

void SB16State::setSpeaker(int on)
{
    speaker = on;
    /* AUD_enable (voice, on); */
}

void SB16State::setControl(int hold)
{
    int dma_chan = use_hdma ? hdma : dma;
    IsaDma *isa_dma_chan = use_hdma ? isa_hdma : isa_dma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma_chan);
    dma_running = hold;

    ldebug("hold %d high %d dma %d", hold, use_hdma, dma_chan);

    if (hold) {
        k->hold_DREQ(isa_dma_chan, dma_chan);
        AUD_set_active_out (voice, 1);
    }
    else {
        k->release_DREQ(isa_dma_chan, dma_chan);
        AUD_set_active_out (voice, 0);
    }
}

static void aux_timer (void *opaque)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    s->can_write = 1;
    qemu_irq_raise (s->pic);
}

#define DMA8_AUTO 1
#define DMA8_HIGH 2

void SB16State::continueDma8()
{
    if (freq > 0) {
        struct audsettings as;

        audio_free = 0;

        as.freq = freq;
        as.nchannels = 1 << fmt_stereo;
        as.fmt = fmt;
        as.endianness = 0;

        voice = AUD_open_out (
            audio_be,
            voice,
            "sb16",
            this,
            SB_audio_callback,
            &as
            );
    }

    setControl(1);
}

static inline int restrict_sampling_rate(int freq)
{
    if (freq < SAMPLE_RATE_MIN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sampling range too low: %d, increasing to %u\n",
                      freq, SAMPLE_RATE_MIN);
        return SAMPLE_RATE_MIN;
    } else if (freq > SAMPLE_RATE_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sampling range too high: %d, decreasing to %u\n",
                      freq, SAMPLE_RATE_MAX);
        return SAMPLE_RATE_MAX;
    } else {
        return freq;
    }
}

void SB16State::dmaCmd8(int mask, int dma_len)
{
    fmt = AUDIO_FORMAT_U8;
    use_hdma = 0;
    fmt_bits = 8;
    fmt_signed = 0;
    fmt_stereo = (mixer_regs[0x0e] & 2) != 0;
    if (-1 == time_const) {
        if (freq <= 0)
            freq = 11025;
    }
    else {
        int tmp = (256 - time_const);
        freq = (1000000 + (tmp / 2)) / tmp;
    }
    freq = restrict_sampling_rate(freq);

    if (dma_len != -1) {
        block_size = dma_len << fmt_stereo;
    }
    else {
        /* This is apparently the only way to make both Act1/PL
           and SecondReality/FC work

           Act1 sets block size via command 0x48 and it's an odd number
           SR does the same with even number
           Both use stereo, and Creatives own documentation states that
           0x48 sets block size in bytes less one.. go figure */
        block_size &= ~fmt_stereo;
    }

    freq >>= fmt_stereo;
    left_till_irq = block_size;
    bytes_per_second = (freq << fmt_stereo);
    /* highspeed = (mask & DMA8_HIGH) != 0; */
    dma_auto = (mask & DMA8_AUTO) != 0;
    align = (1 << fmt_stereo) - 1;

    if (block_size & align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", block_size, align + 1);
    }

    ldebug("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d",
            freq, fmt_stereo, fmt_signed, fmt_bits,
            block_size, dma_auto, fifo, highspeed);

    continueDma8();
    setSpeaker(1);
}

void SB16State::dmaCmd(uint8_t cmd_byte, uint8_t d0, int dma_len)
{
    use_hdma = cmd_byte < 0xc0;
    fifo = (cmd_byte >> 1) & 1;
    dma_auto = (cmd_byte >> 2) & 1;
    fmt_signed = (d0 >> 4) & 1;
    fmt_stereo = (d0 >> 5) & 1;

    switch (cmd_byte >> 4) {
    case 11:
        fmt_bits = 16;
        break;

    case 12:
        fmt_bits = 8;
        break;
    }

    if (-1 != time_const) {
#if 1
        int tmp = 256 - time_const;
        freq = (1000000 + (tmp / 2)) / tmp;
#else
        /* freq = 1000000 / ((255 - time_const) << fmt_stereo); */
        freq = 1000000 / ((255 - time_const));
#endif
        time_const = -1;
    }

    block_size = dma_len + 1;
    block_size <<= (fmt_bits == 16);
    if (!dma_auto) {
        /* It is clear that for DOOM and auto-init this value
           shouldn't take stereo into account, while Miles Sound Systems
           setsound.exe with single transfer mode wouldn't work without it
           wonders of SB16 yet again */
        block_size <<= fmt_stereo;
    }

    ldebug("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d",
            freq, fmt_stereo, fmt_signed, fmt_bits,
            block_size, dma_auto, fifo, highspeed);

    if (16 == fmt_bits) {
        if (fmt_signed) {
            fmt = AUDIO_FORMAT_S16;
        }
        else {
            fmt = AUDIO_FORMAT_U16;
        }
    }
    else {
        if (fmt_signed) {
            fmt = AUDIO_FORMAT_S8;
        }
        else {
            fmt = AUDIO_FORMAT_U8;
        }
    }

    left_till_irq = block_size;

    bytes_per_second = (freq << fmt_stereo) << (fmt_bits == 16);
    highspeed = 0;
    align = (1 << (fmt_stereo + (fmt_bits == 16))) - 1;
    if (block_size & align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", block_size, align + 1);
    }

    if (freq) {
        struct audsettings as;

        audio_free = 0;

        as.freq = freq;
        as.nchannels = 1 << fmt_stereo;
        as.fmt = fmt;
        as.endianness = 0;

        voice = AUD_open_out (
            audio_be,
            voice,
            "sb16",
            this,
            SB_audio_callback,
            &as
            );
    }

    setControl(1);
    setSpeaker(1);
}

inline void SB16State::dspOutData(uint8_t val)
{
    ldebug("outdata 0x%x", val);
    if ((size_t) out_data_len < sizeof (out_data)) {
        out_data[out_data_len++] = val;
    }
}

inline uint8_t SB16State::dspGetData()
{
    if (in_index) {
        return in2_data[--in_index];
    }
    else {
        warn_report("sb16: buffer underflow");
        return 0;
    }
}

void SB16State::handleCommand(uint8_t cmd_byte)
{
    ldebug("command 0x%x", cmd_byte);

    if (cmd_byte > 0xaf && cmd_byte < 0xd0) {
        if (cmd_byte & 8) {
            qemu_log_mask(LOG_UNIMP, "ADC not yet supported (command 0x%x)\n",
                          cmd_byte);
        }

        switch (cmd_byte >> 4) {
        case 11:
        case 12:
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "0x%x wrong bits\n", cmd_byte);
        }
        needed_bytes = 3;
    }
    else {
        needed_bytes = 0;

        switch (cmd_byte) {
        case 0x03:
            dspOutData(0x10); /* csp_param); */
            goto warn;

        case 0x04:
            needed_bytes = 1;
            goto warn;

        case 0x05:
            needed_bytes = 2;
            goto warn;

        case 0x08:
            /* __asm__ ("int3"); */
            goto warn;

        case 0x0e:
            needed_bytes = 2;
            goto warn;

        case 0x09:
            dspOutData(0xf8);
            goto warn;

        case 0x0f:
            needed_bytes = 1;
            goto warn;

        case 0x10:
            needed_bytes = 1;
            goto warn;

        case 0x14:
            needed_bytes = 2;
            block_size = 0;
            break;

        case 0x1c:              /* Auto-Initialize DMA DAC, 8-bit */
            dmaCmd8(DMA8_AUTO, -1);
            break;

        case 0x20:              /* Direct ADC, Juice/PL */
            dspOutData(0xff);
            goto warn;

        case 0x35:
            qemu_log_mask(LOG_UNIMP, "0x35 - MIDI command not implemented\n");
            break;

        case 0x40:
            freq = -1;
            time_const = -1;
            needed_bytes = 1;
            break;

        case 0x41:
            freq = -1;
            time_const = -1;
            needed_bytes = 2;
            break;

        case 0x42:
            freq = -1;
            time_const = -1;
            needed_bytes = 2;
            goto warn;

        case 0x45:
            dspOutData(0xaa);
            goto warn;

        case 0x47:                /* Continue Auto-Initialize DMA 16bit */
            break;

        case 0x48:
            needed_bytes = 2;
            break;

        case 0x74:
            needed_bytes = 2; /* DMA DAC, 4-bit ADPCM */
            qemu_log_mask(LOG_UNIMP, "0x75 - DMA DAC, 4-bit ADPCM not"
                          " implemented\n");
            break;

        case 0x75:              /* DMA DAC, 4-bit ADPCM Reference */
            needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 4-bit ADPCM Reference not"
                          " implemented\n");
            break;

        case 0x76:              /* DMA DAC, 2.6-bit ADPCM */
            needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 2.6-bit ADPCM not"
                          " implemented\n");
            break;

        case 0x77:              /* DMA DAC, 2.6-bit ADPCM Reference */
            needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 2.6-bit ADPCM Reference"
                          " not implemented\n");
            break;

        case 0x7d:
            qemu_log_mask(LOG_UNIMP, "0x7d - Autio-Initialize DMA DAC, 4-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x7f:
            qemu_log_mask(LOG_UNIMP, "0x7d - Autio-Initialize DMA DAC, 2.6-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x80:
            needed_bytes = 2;
            break;

        case 0x90:
        case 0x91:
            dmaCmd8(((cmd_byte & 1) == 0) | DMA8_HIGH, -1);
            break;

        case 0xd0:              /* halt DMA operation. 8bit */
            setControl(0);
            break;

        case 0xd1:              /* speaker on */
            setSpeaker(1);
            break;

        case 0xd3:              /* speaker off */
            setSpeaker(0);
            break;

        case 0xd4:              /* continue DMA operation. 8bit */
            /* KQ6 (or maybe Sierras audblst.drv in general) resets
               the frequency between halt/continue */
            continueDma8();
            break;

        case 0xd5:              /* halt DMA operation. 16bit */
            setControl(0);
            break;

        case 0xd6:              /* continue DMA operation. 16bit */
            setControl(1);
            break;

        case 0xd9:              /* exit auto-init DMA after this block. 16bit */
            dma_auto = 0;
            break;

        case 0xda:              /* exit auto-init DMA after this block. 8bit */
            dma_auto = 0;
            break;

        case 0xe0:              /* DSP identification */
            needed_bytes = 1;
            break;

        case 0xe1:
            dspOutData(ver & 0xff);
            dspOutData(ver >> 8);
            break;

        case 0xe2:
            needed_bytes = 1;
            goto warn;

        case 0xe3:
            {
                int i;
                for (i = sizeof (e3) - 1; i >= 0; --i)
                    dspOutData(e3[i]);
            }
            break;

        case 0xe4:              /* write test reg */
            needed_bytes = 1;
            break;

        case 0xe7:
            qemu_log_mask(LOG_UNIMP, "Attempt to probe for ESS (0xe7)?\n");
            break;

        case 0xe8:              /* read test reg */
            dspOutData(test_reg);
            break;

        case 0xf2:
        case 0xf3:
            dspOutData(0xaa);
            mixer_regs[0x82] |= (cmd_byte == 0xf2) ? 1 : 2;
            qemu_irq_raise (pic);
            break;

        case 0xf9:
            needed_bytes = 1;
            goto warn;

        case 0xfa:
            dspOutData(0);
            goto warn;

        case 0xfc:              /* FIXME */
            dspOutData(0);
            goto warn;

        default:
            qemu_log_mask(LOG_UNIMP, "Unrecognized command 0x%x\n", cmd_byte);
            break;
        }
    }

    if (!needed_bytes) {
        ldebug("!needed_bytes");
    }

 exit:
    if (!needed_bytes) {
        cmd = -1;
    }
    else {
        cmd = cmd_byte;
    }
    return;

 warn:
    qemu_log_mask(LOG_UNIMP, "warning: command 0x%x,%d is not truly understood"
                  " yet\n", cmd_byte, needed_bytes);
    goto exit;

}

uint16_t SB16State::dspGetLohi()
{
    uint8_t hi = dspGetData();
    uint8_t lo = dspGetData();
    return (hi << 8) | lo;
}

uint16_t SB16State::dspGetHilo()
{
    uint8_t lo = dspGetData();
    uint8_t hi = dspGetData();
    return (hi << 8) | lo;
}

void SB16State::handleComplete()
{
    int d0, d1, d2;
    ldebug("complete command 0x%x, in_index %d, needed_bytes %d",
            cmd, in_index, needed_bytes);

    if (cmd > 0xaf && cmd < 0xd0) {
        d2 = dspGetData();
        d1 = dspGetData();
        d0 = dspGetData();

        if (cmd & 8) {
            warn_report("sb16: ADC params cmd = 0x%x d0 = %d, d1 = %d, d2 = %d",
                   cmd, d0, d1, d2);
        }
        else {
            ldebug("cmd = 0x%x d0 = %d, d1 = %d, d2 = %d",
                    cmd, d0, d1, d2);
            dmaCmd(cmd, d0, d1 + (d2 << 8));
        }
    }
    else {
        switch (cmd) {
        case 0x04:
            csp_mode = dspGetData();
            csp_reg83r = 0;
            csp_reg83w = 0;
            ldebug("CSP command 0x04: mode=0x%x", csp_mode);
            break;

        case 0x05:
            csp_param = dspGetData();
            csp_value = dspGetData();
            ldebug("CSP command 0x05: param=0x%x value=0x%x",
                    csp_param,
                    csp_value);
            break;

        case 0x0e:
            d0 = dspGetData();
            d1 = dspGetData();
            ldebug("write CSP register %d <- 0x%x", d1, d0);
            if (d1 == 0x83) {
                ldebug("0x83[%d] <- 0x%x", csp_reg83r, d0);
                csp_reg83[csp_reg83r % 4] = d0;
                csp_reg83r += 1;
            }
            else {
                csp_regs[d1] = d0;
            }
            break;

        case 0x0f:
            d0 = dspGetData();
            ldebug("read CSP register 0x%x -> 0x%x, mode=0x%x",
                    d0, csp_regs[d0], csp_mode);
            if (d0 == 0x83) {
                ldebug("0x83[%d] -> 0x%x",
                        csp_reg83w,
                        csp_reg83[csp_reg83w % 4]);
                dspOutData(csp_reg83[csp_reg83w % 4]);
                csp_reg83w += 1;
            }
            else {
                dspOutData(csp_regs[d0]);
            }
            break;

        case 0x10:
            d0 = dspGetData();
            warn_report("sb16: cmd 0x10 d0=0x%x", d0);
            break;

        case 0x14:
            dmaCmd8(0, dspGetLohi() + 1);
            break;

        case 0x40:
            time_const = dspGetData();
            ldebug("set time const %d", time_const);
            break;

        case 0x41:
        case 0x42:
            /*
             * 0x41 is documented as setting the output sample rate,
             * and 0x42 the input sample rate, but in fact SB16 hardware
             * seems to have only a single sample rate under the hood,
             * and FT2 sets output freq with this (go figure).  Compare:
             * http://homepages.cae.wisc.edu/~brodskye/sb16doc/sb16doc.html#SamplingRate
             */
            freq = restrict_sampling_rate(dspGetHilo());
            ldebug("set freq %d", freq);
            break;

        case 0x48:
            block_size = dspGetLohi() + 1;
            ldebug("set dma block len %d", block_size);
            break;

        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
            /* ADPCM stuff, ignore */
            break;

        case 0x80:
            {
                int f, samples, bytes;
                int64_t ticks;

                f = freq > 0 ? freq : 11025;
                samples = dspGetLohi() + 1;
                bytes = samples << fmt_stereo << (fmt_bits == 16);
                ticks = muldiv64(bytes, NANOSECONDS_PER_SECOND, f);
                if (ticks < NANOSECONDS_PER_SECOND / 1024) {
                    qemu_irq_raise (pic);
                }
                else {
                    if (aux_ts) {
                        timer_mod (
                            aux_ts,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ticks
                            );
                    }
                }
                ldebug("mix silence %d %d %" PRId64, samples, bytes, ticks);
            }
            break;

        case 0xe0:
            d0 = dspGetData();
            out_data_len = 0;
            ldebug("E0 data = 0x%x", d0);
            dspOutData(~d0);
            break;

        case 0xe2:
#if DEBUG
            d0 = dspGetData();
            warn_report("sb16: E2 = 0x%x", d0);
#endif
            break;

        case 0xe4:
            test_reg = dspGetData();
            break;

        case 0xf9:
            d0 = dspGetData();
            ldebug("command 0xf9 with 0x%x", d0);
            switch (d0) {
            case 0x0e:
                dspOutData(0xff);
                break;

            case 0x0f:
                dspOutData(0x07);
                break;

            case 0x37:
                dspOutData(0x38);
                break;

            default:
                dspOutData(0x00);
                break;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "complete: unrecognized command 0x%x\n",
                          cmd);
            return;
        }
    }

    ldebug("");
    cmd = -1;
}

void SB16State::legacyReset()
{
    struct audsettings as;

    freq = 11025;
    fmt_signed = 0;
    fmt_bits = 8;
    fmt_stereo = 0;

    as.freq = freq;
    as.nchannels = 1;
    as.fmt = AUDIO_FORMAT_U8;
    as.endianness = 0;

    voice = AUD_open_out (
        audio_be,
        voice,
        "sb16",
        this,
        SB_audio_callback,
        &as
        );

    /* Not sure about that... */
    /* AUD_set_active_out (voice, 1); */
}

void SB16State::doReset()
{
    qemu_irq_lower (pic);
    if (dma_auto) {
        qemu_irq_raise (pic);
        qemu_irq_lower (pic);
    }

    mixer_regs[0x82] = 0;
    dma_auto = 0;
    in_index = 0;
    out_data_len = 0;
    left_till_irq = 0;
    needed_bytes = 0;
    block_size = -1;
    nzero = 0;
    highspeed = 0;
    v2x6 = 0;
    cmd = -1;

    dspOutData(0xaa);
    setSpeaker(0);
    setControl(0);
    legacyReset();
}

static void dsp_write(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    int iport;

    iport = nport - s->port;

    ldebug("write 0x%x <- 0x%x", nport, val);
    switch (iport) {
    case 0x06:
        switch (val) {
        case 0x00:
            if (s->v2x6 == 1) {
                s->doReset();
            }
            s->v2x6 = 0;
            break;

        case 0x01:
        case 0x03:              /* FreeBSD kludge */
            s->v2x6 = 1;
            break;

        case 0xc6:
            s->v2x6 = 0;        /* Prince of Persia, csp.sys, diagnose.exe */
            break;

        case 0xb8:              /* Panic */
            s->doReset();
            break;

        case 0x39:
            s->dspOutData(0x38);
            s->doReset();
            s->v2x6 = 0x39;
            break;

        default:
            s->v2x6 = val;
            break;
        }
        break;

    case 0x0c:                  /* write data or command | write status */
/*         if (s->highspeed) */
/*             break; */

        if (s->needed_bytes == 0) {
            s->handleCommand(val);
#if 0
            if (0 == s->needed_bytes) {
                log_dsp (s);
            }
#endif
        }
        else {
            if (s->in_index == sizeof (s->in2_data)) {
                warn_report("sb16: in data overrun");
            }
            else {
                s->in2_data[s->in_index++] = val;
                if (s->in_index == s->needed_bytes) {
                    s->needed_bytes = 0;
                    s->handleComplete();
#if 0
                    log_dsp (s);
#endif
                }
            }
        }
        break;

    default:
        ldebug("(nport=0x%x, val=0x%x)", nport, val);
        break;
    }
}

static uint32_t dsp_read(void *opaque, uint32_t nport)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    int iport, retval, ack = 0;

    iport = nport - s->port;

    switch (iport) {
    case 0x06:                  /* reset */
        retval = 0xff;
        break;

    case 0x0a:                  /* read data */
        if (s->out_data_len) {
            retval = s->out_data[--s->out_data_len];
            s->last_read_byte = retval;
        }
        else {
            if (s->cmd != -1) {
                warn_report("sb16: empty output buffer for command 0x%x",
                       s->cmd);
            }
            retval = s->last_read_byte;
            /* goto error; */
        }
        break;

    case 0x0c:                  /* 0 can write */
        retval = s->can_write ? 0 : 0x80;
        break;

    case 0x0d:                  /* timer interrupt clear */
        /* warn_report("sb16: timer interrupt clear"); */
        retval = 0;
        break;

    case 0x0e:                  /* data available status | irq 8 ack */
        retval = (!s->out_data_len || s->highspeed) ? 0 : 0x80;
        if (s->mixer_regs[0x82] & 1) {
            ack = 1;
            s->mixer_regs[0x82] &= ~1;
            qemu_irq_lower (s->pic);
        }
        break;

    case 0x0f:                  /* irq 16 ack */
        retval = 0xff;
        if (s->mixer_regs[0x82] & 2) {
            ack = 1;
            s->mixer_regs[0x82] &= ~2;
            qemu_irq_lower (s->pic);
        }
        break;

    default:
        goto error;
    }

    if (!ack) {
        ldebug("read 0x%x -> 0x%x", nport, retval);
    }

    return retval;

 error:
    warn_report("sb16: dsp_read 0x%x error", nport);
    return 0xff;
}

void SB16State::resetMixer()
{
    int i;

    memset (mixer_regs, 0xff, 0x7f);
    memset (mixer_regs + 0x83, 0xff, sizeof (mixer_regs) - 0x83);

    mixer_regs[0x02] = 4;    /* master volume 3bits */
    mixer_regs[0x06] = 4;    /* MIDI volume 3bits */
    mixer_regs[0x08] = 0;    /* CD volume 3bits */
    mixer_regs[0x0a] = 0;    /* voice volume 2bits */

    /* d5=input filt, d3=lowpass filt, d1,d2=input source */
    mixer_regs[0x0c] = 0;

    /* d5=output filt, d1=stereo switch */
    mixer_regs[0x0e] = 0;

    /* voice volume L d5,d7, R d1,d3 */
    mixer_regs[0x04] = (4 << 5) | (4 << 1);
    /* master ... */
    mixer_regs[0x22] = (4 << 5) | (4 << 1);
    /* MIDI ... */
    mixer_regs[0x26] = (4 << 5) | (4 << 1);

    for (i = 0x30; i < 0x48; i++) {
        mixer_regs[i] = 0x20;
    }
}

static void mixer_write_indexb(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    (void) nport;
    s->mixer_nreg = val;
}

static void mixer_write_datab(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = static_cast<SB16State *>(opaque);

    (void) nport;
    ldebug("mixer_write [0x%x] <- 0x%x", s->mixer_nreg, val);

    switch (s->mixer_nreg) {
    case 0x00:
        s->resetMixer();
        break;

    case 0x80:
        {
            int irq = irq_of_magic (val);
            ldebug("setting irq to %d (val=0x%x)", irq, val);
            if (irq > 0) {
                s->irq = irq;
            }
        }
        break;

    case 0x81:
        {
            int dma, hdma;

            dma = ctz32 (val & 0xf);
            hdma = ctz32 (val & 0xf0);
            if (dma != s->dma || hdma != s->hdma) {
                qemu_log_mask(LOG_GUEST_ERROR, "attempt to change DMA 8bit"
                              " %d(%d), 16bit %d(%d) (val=0x%x)\n", dma, s->dma,
                              hdma, s->hdma, val);
            }
#if 0
            s->dma = dma;
            s->hdma = hdma;
#endif
        }
        break;

    case 0x82:
        qemu_log_mask(LOG_GUEST_ERROR, "attempt to write into IRQ status"
                      " register (val=0x%x)\n", val);
        return;

    default:
        if (s->mixer_nreg >= 0x80) {
            ldebug("attempt to write mixer[0x%x] <- 0x%x", s->mixer_nreg, val);
        }
        break;
    }

    s->mixer_regs[s->mixer_nreg] = val;
}

static uint32_t mixer_read(void *opaque, uint32_t nport)
{
    SB16State *s = static_cast<SB16State *>(opaque);

    (void) nport;
#ifndef DEBUG_SB16_MOST
    if (s->mixer_nreg != 0x82) {
        ldebug("mixer_read[0x%x] -> 0x%x",
                s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
    }
#else
    ldebug("mixer_read[0x%x] -> 0x%x",
            s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
#endif
    return s->mixer_regs[s->mixer_nreg];
}

int SB16State::writeAudio(int nchan, int dma_pos,
                           int dma_len, int len)
{
    IsaDma *isa_dma_chan = nchan == dma ? isa_dma : isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma_chan);
    int temp, net;
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];

    temp = len;
    net = 0;

    while (temp) {
        int left = dma_len - dma_pos;
        int copied;
        size_t to_copy;

        to_copy = MIN (temp, left);
        if (to_copy > sizeof (tmpbuf)) {
            to_copy = sizeof (tmpbuf);
        }

        copied = k->read_memory(isa_dma_chan, nchan, tmpbuf, dma_pos, to_copy);
        copied = AUD_write (voice, tmpbuf, copied);

        temp -= copied;
        dma_pos = (dma_pos + copied) % dma_len;
        net += copied;

        if (!copied) {
            break;
        }
    }

    return net;
}

static int SB_read_DMA (void *opaque, int nchan, int dma_pos, int dma_len)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    int till, copy, written, free;

    if (s->block_size <= 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "invalid block size=%d nchan=%d"
                      " dma_pos=%d dma_len=%d\n", s->block_size, nchan,
                      dma_pos, dma_len);
        return dma_pos;
    }

    if (s->left_till_irq < 0) {
        s->left_till_irq = s->block_size;
    }

    if (s->voice) {
        free = s->audio_free & ~s->align;
        if ((free <= 0) || !dma_len) {
            return dma_pos;
        }
    }
    else {
        free = dma_len;
    }

    copy = free;
    till = s->left_till_irq;

#ifdef DEBUG_SB16_MOST
    warn_report("sb16: pos:%06d %d till:%d len:%d",
           dma_pos, free, till, dma_len);
#endif

    if (till <= copy) {
        if (s->dma_auto == 0) {
            copy = till;
        }
    }

    written = s->writeAudio(nchan, dma_pos, dma_len, copy);
    dma_pos = (dma_pos + written) % dma_len;
    s->left_till_irq -= written;

    if (s->left_till_irq <= 0) {
        s->mixer_regs[0x82] |= (nchan & 4) ? 2 : 1;
        qemu_irq_raise (s->pic);
        if (s->dma_auto == 0) {
            s->setControl(0);
            s->setSpeaker(0);
        }
    }

#ifdef DEBUG_SB16_MOST
    ldebug("pos %5d free %5d size %5d till % 5d copy %5d written %5d size %5d",
            dma_pos, free, dma_len, s->left_till_irq, copy, written,
            s->block_size);
#endif

    while (s->left_till_irq <= 0) {
        s->left_till_irq = s->block_size + s->left_till_irq;
    }

    return dma_pos;
}

static void SB_audio_callback (void *opaque, int free)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    s->audio_free = free;
}

void SB16State::postLoad()
{
    if (voice) {
        AUD_close_out(audio_be, voice);
        voice = NULL;
    }

    if (dma_running) {
        if (freq) {
            struct audsettings as;

            audio_free = 0;

            as.freq = freq;
            as.nchannels = 1 << fmt_stereo;
            as.fmt = fmt;
            as.endianness = 0;

            voice = AUD_open_out (
                audio_be,
                voice,
                "sb16",
                this,
                SB_audio_callback,
                &as
                );
        }

        setControl(1);
        setSpeaker(speaker);
    }
}

static int sb16_post_load (void *opaque, int version_id)
{
    SB16State *s = static_cast<SB16State *>(opaque);
    s->postLoad();
    return 0;
}

static const VMStateField vmstate_sb16_fields[] = {
    VMSTATE_UNUSED(  4 /* irq */
                   + 4 /* dma */
                   + 4 /* hdma */
                   + 4 /* port */
                   + 4 /* ver */),
    VMSTATE_INT32 (in_index, SB16State),
    VMSTATE_INT32 (out_data_len, SB16State),
    VMSTATE_INT32 (fmt_stereo, SB16State),
    VMSTATE_INT32 (fmt_signed, SB16State),
    VMSTATE_INT32 (fmt_bits, SB16State),
    VMSTATE_UINT32 (fmt, SB16State),
    VMSTATE_INT32 (dma_auto, SB16State),
    VMSTATE_INT32 (block_size, SB16State),
    VMSTATE_INT32 (fifo, SB16State),
    VMSTATE_INT32 (freq, SB16State),
    VMSTATE_INT32 (time_const, SB16State),
    VMSTATE_INT32 (speaker, SB16State),
    VMSTATE_INT32 (needed_bytes, SB16State),
    VMSTATE_INT32 (cmd, SB16State),
    VMSTATE_INT32 (use_hdma, SB16State),
    VMSTATE_INT32 (highspeed, SB16State),
    VMSTATE_INT32 (can_write, SB16State),
    VMSTATE_INT32 (v2x6, SB16State),

    VMSTATE_UINT8 (csp_param, SB16State),
    VMSTATE_UINT8 (csp_value, SB16State),
    VMSTATE_UINT8 (csp_mode, SB16State),
    VMSTATE_UINT8 (csp_param, SB16State),
    VMSTATE_BUFFER (csp_regs, SB16State),
    VMSTATE_UINT8 (csp_index, SB16State),
    VMSTATE_BUFFER (csp_reg83, SB16State),
    VMSTATE_INT32 (csp_reg83r, SB16State),
    VMSTATE_INT32 (csp_reg83w, SB16State),

    VMSTATE_BUFFER (in2_data, SB16State),
    VMSTATE_BUFFER (out_data, SB16State),
    VMSTATE_UINT8 (test_reg, SB16State),
    VMSTATE_UINT8 (last_read_byte, SB16State),

    VMSTATE_INT32 (nzero, SB16State),
    VMSTATE_INT32 (left_till_irq, SB16State),
    VMSTATE_INT32 (dma_running, SB16State),
    VMSTATE_INT32 (bytes_per_second, SB16State),
    VMSTATE_INT32 (align, SB16State),

    VMSTATE_INT32 (mixer_nreg, SB16State),
    VMSTATE_BUFFER (mixer_regs, SB16State),

    VMSTATE_END_OF_LIST ()
};

static const VMStateDescription vmstate_sb16 = {
    .name = "sb16",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sb16_post_load,
    .fields = vmstate_sb16_fields,
};

static const MemoryRegionPortio sb16_ioport_list[] = {
    {  4, 1, 1, NULL,     mixer_write_indexb },
    {  5, 1, 1, mixer_read, mixer_write_datab },
    {  6, 1, 1, dsp_read, dsp_write },
    { 10, 1, 1, dsp_read, NULL },
    { 12, 1, 1, NULL,     dsp_write },
    { 12, 4, 1, dsp_read, NULL },
    PORTIO_END_OF_LIST (),
};


void SB16State::init()
{
    cmd = -1;
}

void SB16State::realize(Error **errp)
{
    DeviceState *dev = reinterpret_cast<DeviceState *>(this);
    ISADevice *isadev = reinterpret_cast<ISADevice *>(dev);
    ISABus *bus = isa_bus_from_device(isadev);
    IsaDmaClass *k;

    if (!AUD_backend_check(&audio_be, errp)) {
        return;
    }

    isa_hdma = isa_bus_get_dma(bus, hdma);
    isa_dma = isa_bus_get_dma(bus, dma);
    if (!isa_dma || !isa_hdma) {
        error_setg(errp, "ISA controller does not support DMA");
        return;
    }

    pic = isa_bus_get_irq(bus, irq);

    mixer_regs[0x80] = magic_of_irq (irq);
    mixer_regs[0x81] = (1 << dma) | (1 << hdma);
    mixer_regs[0x82] = 2 << 5;

    csp_regs[5] = 1;
    csp_regs[9] = 0xf8;

    resetMixer();
    aux_ts = timer_new_ns(QEMU_CLOCK_VIRTUAL, aux_timer, this);
    if (!aux_ts) {
        error_setg(errp, "warning: Could not create auxiliary timer");
    }

    isa_register_portio_list(isadev, &portio_list, port,
                             sb16_ioport_list, this, "sb16");

    k = ISADMA_GET_CLASS(isa_hdma);
    k->register_channel(isa_hdma, hdma, SB_read_DMA, this);

    k = ISADMA_GET_CLASS(isa_dma);
    k->register_channel(isa_dma, dma, SB_read_DMA, this);

    can_write = 1;
}

static const Property sb16_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SB16State, audio_be),
    DEFINE_PROP_UINT32 ("version", SB16State, ver,  0x0405), /* 4.5 */
    DEFINE_PROP_UINT32 ("iobase",  SB16State, port, 0x220),
    DEFINE_PROP_UINT32 ("irq",     SB16State, irq,  5),
    DEFINE_PROP_UINT32 ("dma",     SB16State, dma,  1),
    DEFINE_PROP_UINT32 ("dma16",   SB16State, hdma, 5),
};

void SB16State::classInit(DeviceClass *dc)
{
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Creative Sound Blaster 16";
    dc->vmsd = &vmstate_sb16;
    device_class_set_props(dc, sb16_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(SB16State, TYPE_SB16, TYPE_ISA_DEVICE)

static void __attribute__((constructor)) sb16_audio_init(void)
{
    audio_register_model("sb16", "Creative Sound Blaster 16", TYPE_SB16);
}
