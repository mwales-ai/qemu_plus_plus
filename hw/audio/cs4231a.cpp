/*
 * QEMU Crystal CS4231 audio chip emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
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
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qom/object.h"

/*
  Missing features:
  ADC
  Loopback
  Timer
  ADPCM
  More...
*/

#define DEBUG 0
/* #define DEBUG_XLAW */

static struct {
    int aci_counter;
} conf = {1};

#define dolog(fmt, ...) do { \
        if (DEBUG) { \
            error_report("cs4231a: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define lwarn(fmt, ...) warn_report("cs4231a: " fmt, ##__VA_ARGS__)
#define lerr(fmt, ...) error_report("cs4231a: " fmt, ##__VA_ARGS__)

#define CS_REGS 16
#define CS_DREGS 32

#define TYPE_CS4231A "cs4231a"
typedef struct CSState CSState;
DECLARE_INSTANCE_CHECKER(CSState, CS4231A,
                         TYPE_CS4231A)

struct CSState {
    ISADevice dev;
    AudioBackend *audio_be;
    MemoryRegion ioports;
    qemu_irq pic;
    uint32_t regs[CS_REGS];
    uint8_t dregs[CS_DREGS];
    uint32_t irq;
    uint32_t dma;
    uint32_t port;
    IsaDma *isa_dma;
    int shift;
    int dma_running;
    int audio_free;
    int transferred;
    int aci_counter;
    SWVoiceOut *voice;
    const int16_t *tab;

    /* methods */
    void resetState();
    void resetVoices(uint32_t val);
    int writeAudio(int nchan, int dma_pos, int dma_len, int len);

    static void audioCallback(void *opaque, int free);
    static uint64_t readOp(void *opaque, hwaddr addr, unsigned size);
    static void writeOp(void *opaque, hwaddr addr, uint64_t val64, unsigned size);
    static int dmaRead(void *opaque, int nchan, int dma_pos, int dma_len);
    static int preLoad(void *opaque);
    static int postLoad(void *opaque, int version_id);

    void reset();
    void realize(Error **errp);
    void init();
    static void classInit(DeviceClass *dc);
};

#define MODE2 (1 << 6)
#define MCE (1 << 6)
#define PMCE (1 << 4)
#define CMCE (1 << 5)
#define TE (1 << 6)
#define PEN (1 << 0)
#define INT (1 << 0)
#define IEN (1 << 1)
#define PPIO (1 << 6)
#define PI (1 << 4)
#define CI (1 << 5)
#define TI (1 << 6)

enum {
    Index_Address,
    Index_Data,
    Status,
    PIO_Data
};

enum {
    Left_ADC_Input_Control,
    Right_ADC_Input_Control,
    Left_AUX1_Input_Control,
    Right_AUX1_Input_Control,
    Left_AUX2_Input_Control,
    Right_AUX2_Input_Control,
    Left_DAC_Output_Control,
    Right_DAC_Output_Control,
    FS_And_Playback_Data_Format,
    Interface_Configuration,
    Pin_Control,
    Error_Status_And_Initialization,
    MODE_And_ID,
    Loopback_Control,
    Playback_Upper_Base_Count,
    Playback_Lower_Base_Count,
    Alternate_Feature_Enable_I,
    Alternate_Feature_Enable_II,
    Left_Line_Input_Control,
    Right_Line_Input_Control,
    Timer_Low_Base,
    Timer_High_Base,
    RESERVED,
    Alternate_Feature_Enable_III,
    Alternate_Feature_Status,
    Version_Chip_ID,
    Mono_Input_And_Output_Control,
    RESERVED_2,
    Capture_Data_Format,
    RESERVED_3,
    Capture_Upper_Base_Count,
    Capture_Lower_Base_Count
};

static const int freqs[2][8] = {
    { 8000, 16000, 27420, 32000,    -1,    -1, 48000, 9000 },
    { 5510, 11025, 18900, 22050, 37800, 44100, 33075, 6620 }
};

/* Tables courtesy http://hazelware.luggle.com/tutorials/mulawcompression.html */
static const int16_t MuLawDecompressTable[256] =
{
     -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
     -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
     -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
     -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
      -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
      -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
      -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
      -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
      -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
      -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
       -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
       -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
       -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
       -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
       -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
        -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
      32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
      23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
      15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
      11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
       7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
       5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
       3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
       2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
       1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
       1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
        876,   844,   812,   780,   748,   716,   684,   652,
        620,   588,   556,   524,   492,   460,   428,   396,
        372,   356,   340,   324,   308,   292,   276,   260,
        244,   228,   212,   196,   180,   164,   148,   132,
        120,   112,   104,    96,    88,    80,    72,    64,
         56,    48,    40,    32,    24,    16,     8,     0
};

static const int16_t ALawDecompressTable[256] =
{
     -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
     -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
     -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
     -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
     -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
     -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
     -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472,
     -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
     -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
     -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
     -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40,
     -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
     -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
     -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
     -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
     -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848,
      5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
      7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
      2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
      3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
      22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
      30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
      11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472,
      15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
      344,   328,   376,   360,   280,   264,   312,   296,
      472,   456,   504,   488,   408,   392,   440,   424,
      88,    72,   120,   104,    24,     8,    56,    40,
      216,   200,   248,   232,   152,   136,   184,   168,
      1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
      1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
      688,   656,   752,   720,   560,   528,   624,   592,
      944,   912,  1008,   976,   816,   784,   880,   848
};

void CSState::reset()
{
    regs[Index_Address] = 0x40;
    regs[Index_Data]    = 0x00;
    regs[Status]        = 0x00;
    regs[PIO_Data]      = 0x00;

    dregs[Left_ADC_Input_Control]          = 0x00;
    dregs[Right_ADC_Input_Control]         = 0x00;
    dregs[Left_AUX1_Input_Control]         = 0x88;
    dregs[Right_AUX1_Input_Control]        = 0x88;
    dregs[Left_AUX2_Input_Control]         = 0x88;
    dregs[Right_AUX2_Input_Control]        = 0x88;
    dregs[Left_DAC_Output_Control]         = 0x80;
    dregs[Right_DAC_Output_Control]        = 0x80;
    dregs[FS_And_Playback_Data_Format]     = 0x00;
    dregs[Interface_Configuration]         = 0x08;
    dregs[Pin_Control]                     = 0x00;
    dregs[Error_Status_And_Initialization] = 0x00;
    dregs[MODE_And_ID]                     = 0x8a;
    dregs[Loopback_Control]                = 0x00;
    dregs[Playback_Upper_Base_Count]       = 0x00;
    dregs[Playback_Lower_Base_Count]       = 0x00;
    dregs[Alternate_Feature_Enable_I]      = 0x00;
    dregs[Alternate_Feature_Enable_II]     = 0x00;
    dregs[Left_Line_Input_Control]         = 0x88;
    dregs[Right_Line_Input_Control]        = 0x88;
    dregs[Timer_Low_Base]                  = 0x00;
    dregs[Timer_High_Base]                 = 0x00;
    dregs[RESERVED]                        = 0x00;
    dregs[Alternate_Feature_Enable_III]    = 0x00;
    dregs[Alternate_Feature_Status]        = 0x00;
    dregs[Version_Chip_ID]                 = 0xa0;
    dregs[Mono_Input_And_Output_Control]   = 0xa0;
    dregs[RESERVED_2]                      = 0x00;
    dregs[Capture_Data_Format]             = 0x00;
    dregs[RESERVED_3]                      = 0x00;
    dregs[Capture_Upper_Base_Count]        = 0x00;
    dregs[Capture_Lower_Base_Count]        = 0x00;
}

void CSState::audioCallback(void *opaque, int free)
{
    CSState *s = static_cast<CSState *>(opaque);
    s->audio_free = free;
}

void CSState::resetVoices(uint32_t val)
{
    int xtal;
    struct audsettings as;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);

#ifdef DEBUG_XLAW
    if (val == 0 || val == 32)
        val = (1 << 4) | (1 << 5);
#endif

    xtal = val & 1;
    as.freq = freqs[xtal][(val >> 1) & 7];

    if (as.freq == -1) {
        lerr("unsupported frequency (val=0x%x)", val);
        goto error;
    }

    as.nchannels = (val & (1 << 4)) ? 2 : 1;
    as.endianness = 0;
    tab = NULL;

    switch ((val >> 5) & ((dregs[MODE_And_ID] & MODE2) ? 7 : 3)) {
    case 0:
        as.fmt = AUDIO_FORMAT_U8;
        shift = as.nchannels == 2;
        break;

    case 1:
        tab = MuLawDecompressTable;
        goto x_law;
    case 3:
        tab = ALawDecompressTable;
    x_law:
        as.fmt = AUDIO_FORMAT_S16;
        as.endianness = HOST_BIG_ENDIAN;
        shift = as.nchannels == 2;
        break;

    case 6:
        as.endianness = 1;
        /* fall through */
    case 2:
        as.fmt = AUDIO_FORMAT_S16;
        shift = as.nchannels;
        break;

    case 7:
    case 4:
        lerr("attempt to use reserved format value (0x%x)", val);
        goto error;

    case 5:
        lerr("ADPCM 4 bit IMA compatible format is not supported");
        goto error;
    }

    voice = AUD_open_out (
        audio_be,
        voice,
        "cs4231a",
        this,
        CSState::audioCallback,
        &as
        );

    if (dregs[Interface_Configuration] & PEN) {
        if (!dma_running) {
            k->hold_DREQ(isa_dma, dma);
            AUD_set_active_out (voice, 1);
            transferred = 0;
        }
        dma_running = 1;
    }
    else {
        if (dma_running) {
            k->release_DREQ(isa_dma, dma);
            AUD_set_active_out (voice, 0);
        }
        dma_running = 0;
    }
    return;

 error:
    if (dma_running) {
        k->release_DREQ(isa_dma, dma);
        AUD_set_active_out (voice, 0);
    }
}

uint64_t CSState::readOp(void *opaque, hwaddr addr, unsigned size)
{
    CSState *s = static_cast<CSState *>(opaque);
    uint32_t saddr, iaddr, ret;

    saddr = addr;
    iaddr = ~0U;

    switch (saddr) {
    case Index_Address:
        ret = s->regs[saddr] & ~0x80;
        break;

    case Index_Data:
        if (!(s->dregs[MODE_And_ID] & MODE2))
            iaddr = s->regs[Index_Address] & 0x0f;
        else
            iaddr = s->regs[Index_Address] & 0x1f;

        ret = s->dregs[iaddr];
        if (iaddr == Error_Status_And_Initialization) {
            /* keep SEAL happy */
            if (s->aci_counter) {
                ret |= 1 << 5;
                s->aci_counter -= 1;
            }
        }
        break;

    default:
        ret = s->regs[saddr];
        break;
    }
    dolog("read %d:%d -> %d", saddr, iaddr, ret);
    return ret;
}

void CSState::writeOp(void *opaque, hwaddr addr,
                       uint64_t val64, unsigned size)
{
    CSState *s = static_cast<CSState *>(opaque);
    uint32_t saddr, iaddr, val;

    saddr = addr;
    val = val64;

    switch (saddr) {
    case Index_Address:
        if (!(s->regs[Index_Address] & MCE) && (val & MCE)
            && (s->dregs[Interface_Configuration] & (3 << 3)))
            s->aci_counter = conf.aci_counter;

        s->regs[Index_Address] = val & ~(1 << 7);
        break;

    case Index_Data:
        if (!(s->dregs[MODE_And_ID] & MODE2))
            iaddr = s->regs[Index_Address] & 0x0f;
        else
            iaddr = s->regs[Index_Address] & 0x1f;

        switch (iaddr) {
        case RESERVED:
        case RESERVED_2:
        case RESERVED_3:
            lwarn("attempt to write 0x%x to reserved indirect register %d",
                   val, iaddr);
            break;

        case FS_And_Playback_Data_Format:
            if (s->regs[Index_Address] & MCE) {
                s->resetVoices(val);
            }
            else {
                if (s->dregs[Alternate_Feature_Status] & PMCE) {
                    val = (val & ~0x0f) | (s->dregs[iaddr] & 0x0f);
                    s->resetVoices(val);
                }
                else {
                    lwarn("[P]MCE(0x%x, 0x%x) is not set, val=0x%x",
                           s->regs[Index_Address],
                           s->dregs[Alternate_Feature_Status],
                           val);
                    break;
                }
            }
            s->dregs[iaddr] = val;
            break;

        case Interface_Configuration:
            val &= ~(1 << 5);   /* D5 is reserved */
            s->dregs[iaddr] = val;
            if (val & PPIO) {
                lwarn("PIO is not supported (0x%x)", val);
                break;
            }
            if (val & PEN) {
                if (!s->dma_running) {
                    s->resetVoices(s->dregs[FS_And_Playback_Data_Format]);
                }
            }
            else {
                if (s->dma_running) {
                    IsaDmaClass *k = ISADMA_GET_CLASS(s->isa_dma);
                    k->release_DREQ(s->isa_dma, s->dma);
                    AUD_set_active_out (s->voice, 0);
                    s->dma_running = 0;
                }
            }
            break;

        case Error_Status_And_Initialization:
            lwarn("attempt to write to read only register %d", iaddr);
            break;

        case MODE_And_ID:
            dolog("val=0x%x", val);
            if (val & MODE2)
                s->dregs[iaddr] |= MODE2;
            else
                s->dregs[iaddr] &= ~MODE2;
            break;

        case Alternate_Feature_Enable_I:
            if (val & TE)
                lerr("timer is not yet supported");
            s->dregs[iaddr] = val;
            break;

        case Alternate_Feature_Status:
            if ((s->dregs[iaddr] & PI) && !(val & PI)) {
                /* XXX: TI CI */
                qemu_irq_lower (s->pic);
                s->regs[Status] &= ~INT;
            }
            s->dregs[iaddr] = val;
            break;

        case Version_Chip_ID:
            lwarn("write to Version_Chip_ID register 0x%x", val);
            s->dregs[iaddr] = val;
            break;

        default:
            s->dregs[iaddr] = val;
            break;
        }
        dolog("written value 0x%x to indirect register %d", val, iaddr);
        break;

    case Status:
        if (s->regs[Status] & INT) {
            qemu_irq_lower (s->pic);
        }
        s->regs[Status] &= ~INT;
        s->dregs[Alternate_Feature_Status] &= ~(PI | CI | TI);
        break;

    case PIO_Data:
        lwarn("attempt to write value 0x%x to PIO register", val);
        break;
    }
}

int CSState::writeAudio(int nchan, int dma_pos,
                         int dma_len, int len)
{
    int temp, net;
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);

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

        copied = k->read_memory(isa_dma, nchan, tmpbuf, dma_pos, to_copy);
        if (tab) {
            int i;
            QEMU_UNINITIALIZED int16_t linbuf[4096];

            for (i = 0; i < copied; ++i)
                linbuf[i] = tab[tmpbuf[i]];
            copied = AUD_write (voice, linbuf, copied << 1);
            copied >>= 1;
        }
        else {
            copied = AUD_write (voice, tmpbuf, copied);
        }

        temp -= copied;
        dma_pos = (dma_pos + copied) % dma_len;
        net += copied;

        if (!copied) {
            break;
        }
    }

    return net;
}

int CSState::dmaRead(void *opaque, int nchan, int dma_pos, int dma_len)
{
    CSState *s = static_cast<CSState *>(opaque);
    int copy, written;
    int till = -1;

    copy = s->voice ? (s->audio_free >> (s->tab != NULL)) : dma_len;

    if (s->dregs[Pin_Control] & IEN) {
        till = (s->dregs[Playback_Lower_Base_Count]
            | (s->dregs[Playback_Upper_Base_Count] << 8)) << s->shift;
        till -= s->transferred;
        copy = MIN (till, copy);
    }

    if ((copy <= 0) || (dma_len <= 0)) {
        return dma_pos;
    }

    written = s->writeAudio(nchan, dma_pos, dma_len, copy);

    dma_pos = (dma_pos + written) % dma_len;
    s->audio_free -= (written << (s->tab != NULL));

    if (written == till) {
        s->regs[Status] |= INT;
        s->dregs[Alternate_Feature_Status] |= PI;
        s->transferred = 0;
        qemu_irq_raise (s->pic);
    }
    else {
        s->transferred += written;
    }

    return dma_pos;
}

int CSState::preLoad(void *opaque)
{
    CSState *s = static_cast<CSState *>(opaque);

    if (s->dma_running) {
        IsaDmaClass *k = ISADMA_GET_CLASS(s->isa_dma);
        k->release_DREQ(s->isa_dma, s->dma);
        AUD_set_active_out (s->voice, 0);
    }
    s->dma_running = 0;
    return 0;
}

int CSState::postLoad(void *opaque, int version_id)
{
    CSState *s = static_cast<CSState *>(opaque);

    if (s->dma_running && (s->dregs[Interface_Configuration] & PEN)) {
        s->dma_running = 0;
        s->resetVoices(s->dregs[FS_And_Playback_Data_Format]);
    }
    return 0;
}

static const VMStateField vmstate_cs4231a_fields[] = {
    VMSTATE_UINT32_ARRAY (regs, CSState, CS_REGS),
    VMSTATE_BUFFER (dregs, CSState),
    VMSTATE_INT32 (dma_running, CSState),
    VMSTATE_INT32 (audio_free, CSState),
    VMSTATE_INT32 (transferred, CSState),
    VMSTATE_INT32 (aci_counter, CSState),
    VMSTATE_END_OF_LIST ()
};

static const VMStateDescription vmstate_cs4231a = {
    .name = "cs4231a",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = CSState::preLoad,
    .post_load = CSState::postLoad,
    .fields = vmstate_cs4231a_fields,
};

static MemoryRegionOps cs_ioport_ops;

static void __attribute__((constructor)) init_cs_ioport_ops(void)
{
    memset(&cs_ioport_ops, 0, sizeof(cs_ioport_ops));
    cs_ioport_ops.read = CSState::readOp;
    cs_ioport_ops.write = CSState::writeOp;
    cs_ioport_ops.impl.min_access_size = 1;
    cs_ioport_ops.impl.max_access_size = 1;
}

void CSState::init()
{
    memory_region_init_io(&ioports, OBJECT(this), &cs_ioport_ops, this,
                          "cs4231a", 4);
}

void CSState::realize(Error **errp)
{
    ISADevice *d = ISA_DEVICE (&dev);
    ISABus *bus = isa_bus_from_device(d);
    IsaDmaClass *k;

    isa_dma = isa_bus_get_dma(bus, dma);
    if (!isa_dma) {
        error_setg(errp, "ISA controller does not support DMA");
        return;
    }

    if (!AUD_backend_check(&audio_be, errp)) {
        return;
    }

    if (irq >= ISA_NUM_IRQS) {
        error_setg(errp, "Invalid IRQ %d (max %d)", irq, ISA_NUM_IRQS - 1);
        return;
    }
    pic = isa_bus_get_irq(bus, irq);
    k = ISADMA_GET_CLASS(isa_dma);
    k->register_channel(isa_dma, dma, CSState::dmaRead, this);

    isa_register_ioport (d, &ioports, port);
}

static const Property cs4231a_properties[] = {
    DEFINE_AUDIO_PROPERTIES(CSState, audio_be),
    DEFINE_PROP_UINT32 ("iobase",  CSState, port, 0x534),
    DEFINE_PROP_UINT32 ("irq",     CSState, irq,  9),
    DEFINE_PROP_UINT32 ("dma",     CSState, dma,  3),
};

void CSState::classInit(DeviceClass *dc)
{
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Crystal Semiconductor CS4231A";
    dc->vmsd = &vmstate_cs4231a;
    device_class_set_props(dc, cs4231a_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(CSState, TYPE_CS4231A, TYPE_ISA_DEVICE)

static void __attribute__((constructor)) cs4231a_audio_init(void)
{
    audio_register_model("cs4231a", "CS4231A", TYPE_CS4231A);
}
