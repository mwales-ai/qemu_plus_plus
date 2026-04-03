/*
 * Copyright (C) 2006 InnoTek Systemberatung GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * If you received this file as part of a commercial VirtualBox
 * distribution, then only the terms of your commercial VirtualBox
 * license agreement apply instead of the previous paragraph.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/audio/model.h"
#include "qemu/audio.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/dma.h"
#include "qom/object.h"
#include "qemu/error-report.h"
#include "ac97.h"

#define SOFT_VOLUME
#define SR_FIFOE 16             /* rwc */
#define SR_BCIS  8              /* rwc */
#define SR_LVBCI 4              /* rwc */
#define SR_CELV  2              /* ro */
#define SR_DCH   1              /* ro */
#define SR_VALID_MASK ((1 << 5) - 1)
#define SR_WCLEAR_MASK (SR_FIFOE | SR_BCIS | SR_LVBCI)
#define SR_RO_MASK (SR_DCH | SR_CELV)
#define SR_INT_MASK (SR_FIFOE | SR_BCIS | SR_LVBCI)

#define CR_IOCE  16             /* rw */
#define CR_FEIE  8              /* rw */
#define CR_LVBIE 4              /* rw */
#define CR_RR    2              /* rw */
#define CR_RPBM  1              /* rw */
#define CR_VALID_MASK ((1 << 5) - 1)
#define CR_DONT_CLEAR_MASK (CR_IOCE | CR_FEIE | CR_LVBIE)

#define GC_WR    4              /* rw */
#define GC_CR    2              /* rw */
#define GC_VALID_MASK ((1 << 6) - 1)

#define GS_MD3   (1 << 17)      /* rw */
#define GS_AD3   (1 << 16)      /* rw */
#define GS_RCS   (1 << 15)      /* rwc */
#define GS_B3S12 (1 << 14)      /* ro */
#define GS_B2S12 (1 << 13)      /* ro */
#define GS_B1S12 (1 << 12)      /* ro */
#define GS_S1R1  (1 << 11)      /* rwc */
#define GS_S0R1  (1 << 10)      /* rwc */
#define GS_S1CR  (1 << 9)       /* ro */
#define GS_S0CR  (1 << 8)       /* ro */
#define GS_MINT  (1 << 7)       /* ro */
#define GS_POINT (1 << 6)       /* ro */
#define GS_PIINT (1 << 5)       /* ro */
#define GS_RSRVD ((1 << 4) | (1 << 3))
#define GS_MOINT (1 << 2)       /* ro */
#define GS_MIINT (1 << 1)       /* ro */
#define GS_GSCI  1              /* rwc */
#define GS_RO_MASK (GS_B3S12 | \
                    GS_B2S12 | \
                    GS_B1S12 | \
                    GS_S1CR  | \
                    GS_S0CR  | \
                    GS_MINT  | \
                    GS_POINT | \
                    GS_PIINT | \
                    GS_RSRVD | \
                    GS_MOINT | \
                    GS_MIINT)
#define GS_VALID_MASK ((1 << 18) - 1)
#define GS_WCLEAR_MASK (GS_RCS | GS_S1R1 | GS_S0R1 | GS_GSCI)

#define BD_IOC (1 << 31)
#define BD_BUP (1 << 30)

#define TYPE_AC97 "AC97"
OBJECT_DECLARE_SIMPLE_TYPE(AC97LinkState, AC97)

#define REC_MASK 7
enum {
    REC_MIC = 0,
    REC_CD,
    REC_VIDEO,
    REC_AUX,
    REC_LINE_IN,
    REC_STEREO_MIX,
    REC_MONO_MIX,
    REC_PHONE
};

typedef struct BD {
    uint32_t addr;
    uint32_t ctl_len;
} BD;

typedef struct AC97BusMasterRegs {
    uint32_t bdbar;             /* rw 0 */
    uint8_t civ;                /* ro 0 */
    uint8_t lvi;                /* rw 0 */
    uint16_t sr;                /* rw 1 */
    uint16_t picb;              /* ro 0 */
    uint8_t piv;                /* ro 0 */
    uint8_t cr;                 /* rw 0 */
    unsigned int bd_valid;
    BD bd;
} AC97BusMasterRegs;

struct AC97LinkState {
    PCIDevice dev;
    AudioBackend *audio_be;
    uint32_t glob_cnt;
    uint32_t glob_sta;
    uint32_t cas;
    uint32_t last_samp;
    AC97BusMasterRegs bm_regs[3];
    uint8_t mixer_data[256];
    SWVoiceIn *voice_pi;
    SWVoiceOut *voice_po;
    SWVoiceIn *voice_mc;
    int invalid_freq[3];
    uint8_t silence[128];
    int bup_flag;
    MemoryRegion io_nam;
    MemoryRegion io_nabm;

    /* methods */

    void fetchBd(AC97BusMasterRegs *r);
    void updateSr(AC97BusMasterRegs *r, uint32_t new_sr);
    void voiceSetActive(int bm_index, int on);
    void resetBmRegs(AC97BusMasterRegs *r);
    void mixerStore(uint32_t i, uint16_t v);
    uint16_t mixerLoad(uint32_t i);
    void openVoice(int index, int freq);
    void resetVoices(uint8_t *active);
    void updateCombinedVolumeOut();
    void updateVolumeIn();
    void setVolume(int index, uint32_t val);
    void recordSelect(uint32_t val);
    void mixerReset();
    int writeAudio(AC97BusMasterRegs *r, int max, int *stop);
    void writeBup(int elapsed);
    int readAudio(AC97BusMasterRegs *r, int max, int *stop);
    void transferAudio(int index, int elapsed);
    uint32_t namReadb(uint32_t addr);
    uint32_t namReadw(uint32_t addr);
    uint32_t namReadl(uint32_t addr);
    void namWriteb(uint32_t addr, uint32_t val);
    void namWritew(uint32_t addr, uint32_t val);
    void namWritel(uint32_t addr, uint32_t val);
    uint32_t nabmReadb(uint32_t addr);
    uint32_t nabmReadw(uint32_t addr);
    uint32_t nabmReadl(uint32_t addr);
    void nabmWriteb(uint32_t addr, uint32_t val);
    void nabmWritew(uint32_t addr, uint32_t val);
    void nabmWritel(uint32_t addr, uint32_t val);
    void realize(Error **errp);
    void reset();
    static void realizeWrapper(PCIDevice *dev, Error **errp);
    static void resetWrapper(DeviceState *dev);
    static void exitWrapper(PCIDevice *dev);
    static void classInit(ObjectClass *klass, const void *data);
};

enum {
    BUP_SET = 1,
    BUP_LAST = 2
};

#define DEBUG_AC97 0
#define dolog(fmt, ...) do { \
        if (DEBUG_AC97) { \
            error_report("ac97: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define MKREGS(prefix, start)                   \
enum {                                          \
    prefix ## _BDBAR = start,                   \
    prefix ## _CIV = start + 4,                 \
    prefix ## _LVI = start + 5,                 \
    prefix ## _SR = start + 6,                  \
    prefix ## _PICB = start + 8,                \
    prefix ## _PIV = start + 10,                \
    prefix ## _CR = start + 11                  \
}

enum {
    PI_INDEX = 0,
    PO_INDEX,
    MC_INDEX,
    LAST_INDEX
};

MKREGS(PI, PI_INDEX * 16);
MKREGS(PO, PO_INDEX * 16);
MKREGS(MC, MC_INDEX * 16);

enum {
    GLOB_CNT = 0x2c,
    GLOB_STA = 0x30,
    CAS      = 0x34
};

#define GET_BM(index) (((index) >> 4) & 3)

static void po_callback(void *opaque, int free);
static void pi_callback(void *opaque, int avail);
static void mc_callback(void *opaque, int avail);

void AC97LinkState::fetchBd(AC97BusMasterRegs *r)
{
    uint8_t b[8];

    pci_dma_read(&dev, r->bdbar + r->civ * 8, b, 8);
    r->bd_valid = 1;
    r->bd.addr = le32_to_cpu(*(uint32_t *) &b[0]) & ~3;
    r->bd.ctl_len = le32_to_cpu(*(uint32_t *) &b[4]);
    r->picb = r->bd.ctl_len & 0xffff;
    dolog("bd %2d addr=0x%x ctl=0x%06x len=0x%x(%d bytes)",
          r->civ, r->bd.addr, r->bd.ctl_len >> 16,
          r->bd.ctl_len & 0xffff, (r->bd.ctl_len & 0xffff) << 1);
}

void AC97LinkState::updateSr(AC97BusMasterRegs *r, uint32_t new_sr)
{
    int event = 0;
    int level = 0;
    uint32_t new_mask = new_sr & SR_INT_MASK;
    uint32_t old_mask = r->sr & SR_INT_MASK;
    uint32_t masks[] = {GS_PIINT, GS_POINT, GS_MINT};

    if (new_mask ^ old_mask) {
        /** @todo is IRQ deasserted when only one of status bits is cleared? */
        if (!new_mask) {
            event = 1;
            level = 0;
        } else {
            if ((new_mask & SR_LVBCI) && (r->cr & CR_LVBIE)) {
                event = 1;
                level = 1;
            }
            if ((new_mask & SR_BCIS) && (r->cr & CR_IOCE)) {
                event = 1;
                level = 1;
            }
        }
    }

    r->sr = new_sr;

    dolog("IOC%d LVB%d sr=0x%x event=%d level=%d",
          r->sr & SR_BCIS, r->sr & SR_LVBCI, r->sr, event, level);

    if (!event) {
        return;
    }

    if (level) {
        glob_sta |= masks[r - bm_regs];
        dolog("set irq level=1");
        pci_irq_assert(&dev);
    } else {
        glob_sta &= ~masks[r - bm_regs];
        dolog("set irq level=0");
        pci_irq_deassert(&dev);
    }
}

void AC97LinkState::voiceSetActive(int bm_index, int on)
{
    switch (bm_index) {
    case PI_INDEX:
        AUD_set_active_in(voice_pi, on);
        break;

    case PO_INDEX:
        AUD_set_active_out(voice_po, on);
        break;

    case MC_INDEX:
        AUD_set_active_in(voice_mc, on);
        break;

    default:
        error_report("ac97: invalid bm_index(%d) in voice_set_active", bm_index);
        break;
    }
}

void AC97LinkState::resetBmRegs(AC97BusMasterRegs *r)
{
    dolog("reset_bm_regs");
    r->bdbar = 0;
    r->civ = 0;
    r->lvi = 0;
    /** todo do we need to do that? */
    updateSr(r, SR_DCH);
    r->picb = 0;
    r->piv = 0;
    r->cr = r->cr & CR_DONT_CLEAR_MASK;
    r->bd_valid = 0;

    voiceSetActive(r - bm_regs, 0);
    memset(silence, 0, sizeof(silence));
}

void AC97LinkState::mixerStore(uint32_t i, uint16_t v)
{
    if (i + 2 > sizeof(mixer_data)) {
        dolog("mixer_store: index %d out of bounds %zd",
              i, sizeof(mixer_data));
        return;
    }

    mixer_data[i + 0] = v & 0xff;
    mixer_data[i + 1] = v >> 8;
}

uint16_t AC97LinkState::mixerLoad(uint32_t i)
{
    uint16_t val = 0xffff;

    if (i + 2 > sizeof(mixer_data)) {
        dolog("mixer_load: index %d out of bounds %zd",
              i, sizeof(mixer_data));
    } else {
        val = mixer_data[i + 0] | (mixer_data[i + 1] << 8);
    }

    return val;
}

void AC97LinkState::openVoice(int index, int freq)
{
    struct audsettings as;

    as.freq = freq;
    as.nchannels = 2;
    as.fmt = AUDIO_FORMAT_S16;
    as.endianness = 0;

    if (freq > 0) {
        invalid_freq[index] = 0;
        switch (index) {
        case PI_INDEX:
            voice_pi = AUD_open_in(
                audio_be,
                voice_pi,
                "ac97.pi",
                this,
                pi_callback,
                &as
                );
            break;

        case PO_INDEX:
            voice_po = AUD_open_out(
                audio_be,
                voice_po,
                "ac97.po",
                this,
                po_callback,
                &as
                );
            break;

        case MC_INDEX:
            voice_mc = AUD_open_in(
                audio_be,
                voice_mc,
                "ac97.mc",
                this,
                mc_callback,
                &as
                );
            break;
        }
    } else {
        invalid_freq[index] = freq;
        switch (index) {
        case PI_INDEX:
            AUD_close_in(audio_be, voice_pi);
            voice_pi = NULL;
            break;

        case PO_INDEX:
            AUD_close_out(audio_be, voice_po);
            voice_po = NULL;
            break;

        case MC_INDEX:
            AUD_close_in(audio_be, voice_mc);
            voice_mc = NULL;
            break;
        }
    }
}

void AC97LinkState::resetVoices(uint8_t active[LAST_INDEX])
{
    uint16_t freq;

    freq = mixerLoad(AC97_PCM_LR_ADC_Rate);
    openVoice(PI_INDEX, freq);
    AUD_set_active_in(voice_pi, active[PI_INDEX]);

    freq = mixerLoad(AC97_PCM_Front_DAC_Rate);
    openVoice(PO_INDEX, freq);
    AUD_set_active_out(voice_po, active[PO_INDEX]);

    freq = mixerLoad(AC97_MIC_ADC_Rate);
    openVoice(MC_INDEX, freq);
    AUD_set_active_in(voice_mc, active[MC_INDEX]);
}

static void get_volume(uint16_t vol, uint16_t mask, int inverse,
                       int *mute, uint8_t *lvol, uint8_t *rvol)
{
    *mute = (vol >> MUTE_SHIFT) & 1;
    *rvol = (255 * (vol & mask)) / mask;
    *lvol = (255 * ((vol >> 8) & mask)) / mask;

    if (inverse) {
        *rvol = 255 - *rvol;
        *lvol = 255 - *lvol;
    }
}

void AC97LinkState::updateCombinedVolumeOut()
{
    uint8_t lvol, rvol, plvol, prvol;
    int mute, pmute;

    get_volume(mixerLoad(AC97_Master_Volume_Mute), 0x3f, 1,
               &mute, &lvol, &rvol);
    get_volume(mixerLoad(AC97_PCM_Out_Volume_Mute), 0x1f, 1,
               &pmute, &plvol, &prvol);

    mute = mute | pmute;
    lvol = (lvol * plvol) / 255;
    rvol = (rvol * prvol) / 255;

    AUD_set_volume_out_lr(voice_po, mute, lvol, rvol);
}

void AC97LinkState::updateVolumeIn()
{
    uint8_t lvol, rvol;
    int mute;

    get_volume(mixerLoad(AC97_Record_Gain_Mute), 0x0f, 0,
               &mute, &lvol, &rvol);

    AUD_set_volume_in_lr(voice_pi, mute, lvol, rvol);
}

void AC97LinkState::setVolume(int index, uint32_t val)
{
    switch (index) {
    case AC97_Master_Volume_Mute:
        val &= 0xbf3f;
        mixerStore(index, val);
        updateCombinedVolumeOut();
        break;
    case AC97_PCM_Out_Volume_Mute:
        val &= 0x9f1f;
        mixerStore(index, val);
        updateCombinedVolumeOut();
        break;
    case AC97_Record_Gain_Mute:
        val &= 0x8f0f;
        mixerStore(index, val);
        updateVolumeIn();
        break;
    }
}

void AC97LinkState::recordSelect(uint32_t val)
{
    uint8_t rs = val & REC_MASK;
    uint8_t ls = (val >> 8) & REC_MASK;
    mixerStore(AC97_Record_Select, rs | (ls << 8));
}

void AC97LinkState::mixerReset()
{
    uint8_t active[LAST_INDEX];

    dolog("mixer_reset");
    memset(mixer_data, 0, sizeof(mixer_data));
    memset(active, 0, sizeof(active));
    mixerStore(AC97_Reset, 0x0000); /* 6940 */
    mixerStore(AC97_Headphone_Volume_Mute, 0x0000);
    mixerStore(AC97_Master_Volume_Mono_Mute, 0x0000);
    mixerStore(AC97_Master_Tone_RL, 0x0000);
    mixerStore(AC97_PC_BEEP_Volume_Mute, 0x0000);
    mixerStore(AC97_Phone_Volume_Mute, 0x0000);
    mixerStore(AC97_Mic_Volume_Mute, 0x0000);
    mixerStore(AC97_Line_In_Volume_Mute, 0x0000);
    mixerStore(AC97_CD_Volume_Mute, 0x0000);
    mixerStore(AC97_Video_Volume_Mute, 0x0000);
    mixerStore(AC97_Aux_Volume_Mute, 0x0000);
    mixerStore(AC97_Record_Gain_Mic_Mute, 0x0000);
    mixerStore(AC97_General_Purpose, 0x0000);
    mixerStore(AC97_3D_Control, 0x0000);
    mixerStore(AC97_Powerdown_Ctrl_Stat, 0x000f);

    /*
     * Sigmatel 9700 (STAC9700)
     */
    mixerStore(AC97_Vendor_ID1, 0x8384);
    mixerStore(AC97_Vendor_ID2, 0x7600); /* 7608 */

    mixerStore(AC97_Extended_Audio_ID, 0x0809);
    mixerStore(AC97_Extended_Audio_Ctrl_Stat, 0x0009);
    mixerStore(AC97_PCM_Front_DAC_Rate, 0xbb80);
    mixerStore(AC97_PCM_Surround_DAC_Rate, 0xbb80);
    mixerStore(AC97_PCM_LFE_DAC_Rate, 0xbb80);
    mixerStore(AC97_PCM_LR_ADC_Rate, 0xbb80);
    mixerStore(AC97_MIC_ADC_Rate, 0xbb80);

    recordSelect(0);
    setVolume(AC97_Master_Volume_Mute, 0x8000);
    setVolume(AC97_PCM_Out_Volume_Mute, 0x8808);
    setVolume(AC97_Record_Gain_Mute, 0x8808);

    resetVoices(active);
}

/**
 * Native audio mixer
 * I/O Reads
 */
uint32_t AC97LinkState::namReadb(uint32_t addr)
{
    dolog("U nam readb 0x%x", addr);
    cas = 0;
    return ~0U;
}

uint32_t AC97LinkState::namReadw(uint32_t addr)
{
    cas = 0;
    return mixerLoad(addr);
}

uint32_t AC97LinkState::namReadl(uint32_t addr)
{
    dolog("U nam readl 0x%x", addr);
    cas = 0;
    return ~0U;
}

/**
 * Native audio mixer
 * I/O Writes
 */
void AC97LinkState::namWriteb(uint32_t addr, uint32_t val)
{
    dolog("U nam writeb 0x%x <- 0x%x", addr, val);
    cas = 0;
}

void AC97LinkState::namWritew(uint32_t addr, uint32_t val)
{
    cas = 0;
    switch (addr) {
    case AC97_Reset:
        mixerReset();
        break;
    case AC97_Powerdown_Ctrl_Stat:
        val &= ~0x800f;
        val |= mixerLoad(addr) & 0xf;
        mixerStore(addr, val);
        break;
    case AC97_PCM_Out_Volume_Mute:
    case AC97_Master_Volume_Mute:
    case AC97_Record_Gain_Mute:
        setVolume(addr, val);
        break;
    case AC97_Record_Select:
        recordSelect(val);
        break;
    case AC97_Vendor_ID1:
    case AC97_Vendor_ID2:
        dolog("Attempt to write vendor ID to 0x%x", val);
        break;
    case AC97_Extended_Audio_ID:
        dolog("Attempt to write extended audio ID to 0x%x", val);
        break;
    case AC97_Extended_Audio_Ctrl_Stat:
        if (!(val & EACS_VRA)) {
            mixerStore(AC97_PCM_Front_DAC_Rate, 0xbb80);
            mixerStore(AC97_PCM_LR_ADC_Rate,    0xbb80);
            openVoice(PI_INDEX, 48000);
            openVoice(PO_INDEX, 48000);
        }
        if (!(val & EACS_VRM)) {
            mixerStore(AC97_MIC_ADC_Rate, 0xbb80);
            openVoice(MC_INDEX, 48000);
        }
        dolog("Setting extended audio control to 0x%x", val);
        mixerStore(AC97_Extended_Audio_Ctrl_Stat, val);
        break;
    case AC97_PCM_Front_DAC_Rate:
        if (mixerLoad(AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            mixerStore(addr, val);
            dolog("Set front DAC rate to %d", val);
            openVoice(PO_INDEX, val);
        } else {
            dolog("Attempt to set front DAC rate to %d, but VRA is not set",
                  val);
        }
        break;
    case AC97_MIC_ADC_Rate:
        if (mixerLoad(AC97_Extended_Audio_Ctrl_Stat) & EACS_VRM) {
            mixerStore(addr, val);
            dolog("Set MIC ADC rate to %d", val);
            openVoice(MC_INDEX, val);
        } else {
            dolog("Attempt to set MIC ADC rate to %d, but VRM is not set",
                  val);
        }
        break;
    case AC97_PCM_LR_ADC_Rate:
        if (mixerLoad(AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            mixerStore(addr, val);
            dolog("Set front LR ADC rate to %d", val);
            openVoice(PI_INDEX, val);
        } else {
            dolog("Attempt to set LR ADC rate to %d, but VRA is not set",
                  val);
        }
        break;
    case AC97_Headphone_Volume_Mute:
    case AC97_Master_Volume_Mono_Mute:
    case AC97_Master_Tone_RL:
    case AC97_PC_BEEP_Volume_Mute:
    case AC97_Phone_Volume_Mute:
    case AC97_Mic_Volume_Mute:
    case AC97_Line_In_Volume_Mute:
    case AC97_CD_Volume_Mute:
    case AC97_Video_Volume_Mute:
    case AC97_Aux_Volume_Mute:
    case AC97_Record_Gain_Mic_Mute:
    case AC97_General_Purpose:
    case AC97_3D_Control:
    case AC97_Sigmatel_Analog:
    case AC97_Sigmatel_Dac2Invert:
        /* None of the features in these regs are emulated, so they are RO */
        break;
    default:
        dolog("U nam writew 0x%x <- 0x%x", addr, val);
        mixerStore(addr, val);
        break;
    }
}

void AC97LinkState::namWritel(uint32_t addr, uint32_t val)
{
    dolog("U nam writel 0x%x <- 0x%x", addr, val);
    cas = 0;
}

/**
 * Native audio bus master
 * I/O Reads
 */
uint32_t AC97LinkState::nabmReadb(uint32_t addr)
{
    AC97BusMasterRegs *r = NULL;
    uint32_t val = ~0U;

    switch (addr) {
    case CAS:
        dolog("CAS %d", cas);
        val = cas;
        cas = 1;
        break;
    case PI_CIV:
    case PO_CIV:
    case MC_CIV:
        r = &bm_regs[GET_BM(addr)];
        val = r->civ;
        dolog("CIV[%d] -> 0x%x", GET_BM(addr), val);
        break;
    case PI_LVI:
    case PO_LVI:
    case MC_LVI:
        r = &bm_regs[GET_BM(addr)];
        val = r->lvi;
        dolog("LVI[%d] -> 0x%x", GET_BM(addr), val);
        break;
    case PI_PIV:
    case PO_PIV:
    case MC_PIV:
        r = &bm_regs[GET_BM(addr)];
        val = r->piv;
        dolog("PIV[%d] -> 0x%x", GET_BM(addr), val);
        break;
    case PI_CR:
    case PO_CR:
    case MC_CR:
        r = &bm_regs[GET_BM(addr)];
        val = r->cr;
        dolog("CR[%d] -> 0x%x", GET_BM(addr), val);
        break;
    case PI_SR:
    case PO_SR:
    case MC_SR:
        r = &bm_regs[GET_BM(addr)];
        val = r->sr & 0xff;
        dolog("SRb[%d] -> 0x%x", GET_BM(addr), val);
        break;
    default:
        dolog("U nabm readb 0x%x -> 0x%x", addr, val);
        break;
    }
    return val;
}

uint32_t AC97LinkState::nabmReadw(uint32_t addr)
{
    AC97BusMasterRegs *r = NULL;
    uint32_t val = ~0U;

    switch (addr) {
    case PI_SR:
    case PO_SR:
    case MC_SR:
        r = &bm_regs[GET_BM(addr)];
        val = r->sr;
        dolog("SR[%d] -> 0x%x", GET_BM(addr), val);
        break;
    case PI_PICB:
    case PO_PICB:
    case MC_PICB:
        r = &bm_regs[GET_BM(addr)];
        val = r->picb;
        dolog("PICB[%d] -> 0x%x", GET_BM(addr), val);
        break;
    default:
        dolog("U nabm readw 0x%x -> 0x%x", addr, val);
        break;
    }
    return val;
}

uint32_t AC97LinkState::nabmReadl(uint32_t addr)
{
    AC97BusMasterRegs *r = NULL;
    uint32_t val = ~0U;

    switch (addr) {
    case PI_BDBAR:
    case PO_BDBAR:
    case MC_BDBAR:
        r = &bm_regs[GET_BM(addr)];
        val = r->bdbar;
        dolog("BMADDR[%d] -> 0x%x", GET_BM(addr), val);
        break;
    case PI_CIV:
    case PO_CIV:
    case MC_CIV:
        r = &bm_regs[GET_BM(addr)];
        val = r->civ | (r->lvi << 8) | (r->sr << 16);
        dolog("CIV LVI SR[%d] -> 0x%x, 0x%x, 0x%x", GET_BM(addr),
               r->civ, r->lvi, r->sr);
        break;
    case PI_PICB:
    case PO_PICB:
    case MC_PICB:
        r = &bm_regs[GET_BM(addr)];
        val = r->picb | (r->piv << 16) | (r->cr << 24);
        dolog("PICB PIV CR[%d] -> 0x%x 0x%x 0x%x 0x%x", GET_BM(addr),
               val, r->picb, r->piv, r->cr);
        break;
    case GLOB_CNT:
        val = glob_cnt;
        dolog("glob_cnt -> 0x%x", val);
        break;
    case GLOB_STA:
        val = glob_sta | GS_S0CR;
        dolog("glob_sta -> 0x%x", val);
        break;
    default:
        dolog("U nabm readl 0x%x -> 0x%x", addr, val);
        break;
    }
    return val;
}

/**
 * Native audio bus master
 * I/O Writes
 */
void AC97LinkState::nabmWriteb(uint32_t addr, uint32_t val)
{
    AC97BusMasterRegs *r = NULL;

    switch (addr) {
    case PI_LVI:
    case PO_LVI:
    case MC_LVI:
        r = &bm_regs[GET_BM(addr)];
        if ((r->cr & CR_RPBM) && (r->sr & SR_DCH)) {
            r->sr &= ~(SR_DCH | SR_CELV);
            r->civ = r->piv;
            r->piv = (r->piv + 1) % 32;
            fetchBd(r);
        }
        r->lvi = val % 32;
        dolog("LVI[%d] <- 0x%x", GET_BM(addr), val);
        break;
    case PI_CR:
    case PO_CR:
    case MC_CR:
        r = &bm_regs[GET_BM(addr)];
        if (val & CR_RR) {
            resetBmRegs(r);
        } else {
            r->cr = val & CR_VALID_MASK;
            if (!(r->cr & CR_RPBM)) {
                voiceSetActive(r - bm_regs, 0);
                r->sr |= SR_DCH;
            } else {
                r->civ = r->piv;
                r->piv = (r->piv + 1) % 32;
                fetchBd(r);
                r->sr &= ~SR_DCH;
                voiceSetActive(r - bm_regs, 1);
            }
        }
        dolog("CR[%d] <- 0x%x (cr 0x%x)", GET_BM(addr), val, r->cr);
        break;
    case PI_SR:
    case PO_SR:
    case MC_SR:
        r = &bm_regs[GET_BM(addr)];
        r->sr |= val & ~(SR_RO_MASK | SR_WCLEAR_MASK);
        updateSr(r, r->sr & ~(val & SR_WCLEAR_MASK));
        dolog("SR[%d] <- 0x%x (sr 0x%x)", GET_BM(addr), val, r->sr);
        break;
    default:
        dolog("U nabm writeb 0x%x <- 0x%x", addr, val);
        break;
    }
}

void AC97LinkState::nabmWritew(uint32_t addr, uint32_t val)
{
    AC97BusMasterRegs *r = NULL;

    switch (addr) {
    case PI_SR:
    case PO_SR:
    case MC_SR:
        r = &bm_regs[GET_BM(addr)];
        r->sr |= val & ~(SR_RO_MASK | SR_WCLEAR_MASK);
        updateSr(r, r->sr & ~(val & SR_WCLEAR_MASK));
        dolog("SR[%d] <- 0x%x (sr 0x%x)", GET_BM(addr), val, r->sr);
        break;
    default:
        dolog("U nabm writew 0x%x <- 0x%x", addr, val);
        break;
    }
}

void AC97LinkState::nabmWritel(uint32_t addr, uint32_t val)
{
    AC97BusMasterRegs *r = NULL;

    switch (addr) {
    case PI_BDBAR:
    case PO_BDBAR:
    case MC_BDBAR:
        r = &bm_regs[GET_BM(addr)];
        r->bdbar = val & ~3;
        dolog("BDBAR[%d] <- 0x%x (bdbar 0x%x)", GET_BM(addr), val, r->bdbar);
        break;
    case GLOB_CNT:
        /* TODO: Handle WR or CR being set (warm/cold reset requests) */
        if (!(val & (GC_WR | GC_CR))) {
            glob_cnt = val & GC_VALID_MASK;
        }
        dolog("glob_cnt <- 0x%x (glob_cnt 0x%x)", val, glob_cnt);
        break;
    case GLOB_STA:
        glob_sta &= ~(val & GS_WCLEAR_MASK);
        glob_sta |= (val & ~(GS_WCLEAR_MASK | GS_RO_MASK)) & GS_VALID_MASK;
        dolog("glob_sta <- 0x%x (glob_sta 0x%x)", val, glob_sta);
        break;
    default:
        dolog("U nabm writel 0x%x <- 0x%x", addr, val);
        break;
    }
}

int AC97LinkState::writeAudio(AC97BusMasterRegs *r,
                       int max, int *stop)
{
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];
    uint32_t addr = r->bd.addr;
    uint32_t temp = r->picb << 1;
    uint32_t written = 0;
    int to_copy = 0;
    temp = MIN(temp, max);

    if (!temp) {
        *stop = 1;
        return 0;
    }

    while (temp) {
        int copied;
        to_copy = MIN(temp, sizeof(tmpbuf));
        pci_dma_read(&dev, addr, tmpbuf, to_copy);
        copied = AUD_write(voice_po, tmpbuf, to_copy);
        dolog("write_audio max=%x to_copy=%x copied=%x",
              max, to_copy, copied);
        if (!copied) {
            *stop = 1;
            break;
        }
        temp -= copied;
        addr += copied;
        written += copied;
    }

    if (!temp) {
        if (to_copy < 4) {
            dolog("whoops");
            last_samp = 0;
        } else {
            last_samp = *(uint32_t *)&tmpbuf[to_copy - 4];
        }
    }

    r->bd.addr = addr;
    return written;
}

void AC97LinkState::writeBup(int elapsed)
{
    dolog("write_bup");
    if (!(bup_flag & BUP_SET)) {
        if (bup_flag & BUP_LAST) {
            int i;
            uint8_t *p = silence;
            for (i = 0; i < sizeof(silence) / 4; i++, p += 4) {
                *(uint32_t *) p = last_samp;
            }
        } else {
            memset(silence, 0, sizeof(silence));
        }
        bup_flag |= BUP_SET;
    }

    while (elapsed) {
        int temp = MIN(elapsed, sizeof(silence));
        while (temp) {
            int copied = AUD_write(voice_po, silence, temp);
            if (!copied) {
                return;
            }
            temp -= copied;
            elapsed -= copied;
        }
    }
}

int AC97LinkState::readAudio(AC97BusMasterRegs *r,
                      int max, int *stop)
{
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];
    uint32_t addr = r->bd.addr;
    uint32_t temp = r->picb << 1;
    uint32_t nread = 0;
    int to_copy = 0;
    SWVoiceIn *voice = (r - bm_regs) == MC_INDEX ? voice_mc : voice_pi;

    temp = MIN(temp, max);

    if (!temp) {
        *stop = 1;
        return 0;
    }

    while (temp) {
        int acquired;
        to_copy = MIN(temp, sizeof(tmpbuf));
        acquired = AUD_read(voice, tmpbuf, to_copy);
        if (!acquired) {
            *stop = 1;
            break;
        }
        pci_dma_write(&dev, addr, tmpbuf, acquired);
        temp -= acquired;
        addr += acquired;
        nread += acquired;
    }

    r->bd.addr = addr;
    return nread;
}

void AC97LinkState::transferAudio(int index, int elapsed)
{
    AC97BusMasterRegs *r = &bm_regs[index];
    int stop = 0;

    if (invalid_freq[index]) {
        error_report("ac97: attempt to use voice %d with invalid frequency %d",
                index, invalid_freq[index]);
        return;
    }

    if (r->sr & SR_DCH) {
        if (r->cr & CR_RPBM) {
            switch (index) {
            case PO_INDEX:
                writeBup(elapsed);
                break;
            }
        }
        return;
    }

    while ((elapsed >> 1) && !stop) {
        int temp;

        if (!r->bd_valid) {
            dolog("invalid bd");
            fetchBd(r);
        }

        if (!r->picb) {
            dolog("fresh bd %d is empty 0x%x 0x%x",
                  r->civ, r->bd.addr, r->bd.ctl_len);
            if (r->civ == r->lvi) {
                r->sr |= SR_DCH; /* CELV? */
                bup_flag = 0;
                break;
            }
            r->sr &= ~SR_CELV;
            r->civ = r->piv;
            r->piv = (r->piv + 1) % 32;
            fetchBd(r);
            return;
        }

        switch (index) {
        case PO_INDEX:
            temp = writeAudio(r, elapsed, &stop);
            elapsed -= temp;
            r->picb -= (temp >> 1);
            break;

        case PI_INDEX:
        case MC_INDEX:
            temp = readAudio(r, elapsed, &stop);
            elapsed -= temp;
            r->picb -= (temp >> 1);
            break;
        }

        if (!r->picb) {
            uint32_t new_sr = r->sr & ~SR_CELV;

            if (r->bd.ctl_len & BD_IOC) {
                new_sr |= SR_BCIS;
            }

            if (r->civ == r->lvi) {
                dolog("Underrun civ (%d) == lvi (%d)", r->civ, r->lvi);

                new_sr |= SR_LVBCI | SR_DCH | SR_CELV;
                stop = 1;
                bup_flag = (r->bd.ctl_len & BD_BUP) ? BUP_LAST : 0;
            } else {
                r->civ = r->piv;
                r->piv = (r->piv + 1) % 32;
                fetchBd(r);
            }

            updateSr(r, new_sr);
        }
    }
}

static void pi_callback(void *opaque, int avail)
{
    static_cast<AC97LinkState *>(opaque)->transferAudio(PI_INDEX, avail);
}

static void mc_callback(void *opaque, int avail)
{
    static_cast<AC97LinkState *>(opaque)->transferAudio(MC_INDEX, avail);
}

static void po_callback(void *opaque, int free)
{
    static_cast<AC97LinkState *>(opaque)->transferAudio(PO_INDEX, free);
}

static const VMStateField vmstate_ac97_bm_regs_fields[] = {
    VMSTATE_UINT32(bdbar, AC97BusMasterRegs),
    VMSTATE_UINT8(civ, AC97BusMasterRegs),
    VMSTATE_UINT8(lvi, AC97BusMasterRegs),
    VMSTATE_UINT16(sr, AC97BusMasterRegs),
    VMSTATE_UINT16(picb, AC97BusMasterRegs),
    VMSTATE_UINT8(piv, AC97BusMasterRegs),
    VMSTATE_UINT8(cr, AC97BusMasterRegs),
    VMSTATE_UINT32(bd_valid, AC97BusMasterRegs),
    VMSTATE_UINT32(bd.addr, AC97BusMasterRegs),
    VMSTATE_UINT32(bd.ctl_len, AC97BusMasterRegs),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_ac97_bm_regs = {
    .name = "ac97_bm_regs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_ac97_bm_regs_fields,
};

static int ac97_post_load(void *opaque, int version_id)
{
    uint8_t active[LAST_INDEX];
    AC97LinkState *s = static_cast<AC97LinkState *>(opaque);

    s->recordSelect(s->mixerLoad(AC97_Record_Select));
    s->setVolume(AC97_Master_Volume_Mute,
               s->mixerLoad(AC97_Master_Volume_Mute));
    s->setVolume(AC97_PCM_Out_Volume_Mute,
               s->mixerLoad(AC97_PCM_Out_Volume_Mute));
    s->setVolume(AC97_Record_Gain_Mute,
               s->mixerLoad(AC97_Record_Gain_Mute));

    active[PI_INDEX] = !!(s->bm_regs[PI_INDEX].cr & CR_RPBM);
    active[PO_INDEX] = !!(s->bm_regs[PO_INDEX].cr & CR_RPBM);
    active[MC_INDEX] = !!(s->bm_regs[MC_INDEX].cr & CR_RPBM);
    s->resetVoices(active);

    s->bup_flag = 0;
    s->last_samp = 0;
    return 0;
}

static bool is_version_2(void *opaque, int version_id)
{
    return version_id == 2;
}

static const VMStateField vmstate_ac97_fields[] = {
    VMSTATE_PCI_DEVICE(dev, AC97LinkState),
    VMSTATE_UINT32(glob_cnt, AC97LinkState),
    VMSTATE_UINT32(glob_sta, AC97LinkState),
    VMSTATE_UINT32(cas, AC97LinkState),
    VMSTATE_STRUCT_ARRAY(bm_regs, AC97LinkState, 3, 1,
                         vmstate_ac97_bm_regs, AC97BusMasterRegs),
    VMSTATE_BUFFER(mixer_data, AC97LinkState),
    VMSTATE_UNUSED_TEST(is_version_2, 3),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_ac97 = {
    .name = "ac97",
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = ac97_post_load,
    .fields = vmstate_ac97_fields,
};

static uint64_t nam_read(void *opaque, hwaddr addr, unsigned size)
{
    if ((addr / size) > 256) {
        return -1;
    }

    AC97LinkState *s = static_cast<AC97LinkState *>(opaque);
    switch (size) {
    case 1:
        return s->namReadb(addr);
    case 2:
        return s->namReadw(addr);
    case 4:
        return s->namReadl(addr);
    default:
        return -1;
    }
}

static void nam_write(void *opaque, hwaddr addr, uint64_t val,
                      unsigned size)
{
    if ((addr / size) > 256) {
        return;
    }

    AC97LinkState *s = static_cast<AC97LinkState *>(opaque);
    switch (size) {
    case 1:
        s->namWriteb(addr, val);
        break;
    case 2:
        s->namWritew(addr, val);
        break;
    case 4:
        s->namWritel(addr, val);
        break;
    }
}

static const MemoryRegionOps ac97_io_nam_ops = {
    .read = nam_read,
    .write = nam_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t nabm_read(void *opaque, hwaddr addr, unsigned size)
{
    if ((addr / size) > 64) {
        return -1;
    }

    AC97LinkState *s = static_cast<AC97LinkState *>(opaque);
    switch (size) {
    case 1:
        return s->nabmReadb(addr);
    case 2:
        return s->nabmReadw(addr);
    case 4:
        return s->nabmReadl(addr);
    default:
        return -1;
    }
}

static void nabm_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    if ((addr / size) > 64) {
        return;
    }

    AC97LinkState *s = static_cast<AC97LinkState *>(opaque);
    switch (size) {
    case 1:
        s->nabmWriteb(addr, val);
        break;
    case 2:
        s->nabmWritew(addr, val);
        break;
    case 4:
        s->nabmWritel(addr, val);
        break;
    }
}


static const MemoryRegionOps ac97_io_nabm_ops = {
    .read = nabm_read,
    .write = nabm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

void AC97LinkState::reset()
{
    resetBmRegs(&bm_regs[0]);
    resetBmRegs(&bm_regs[1]);
    resetBmRegs(&bm_regs[2]);

    /*
     * Reset the mixer too. The Windows XP driver seems to rely on
     * this. At least it wants to read the vendor id before it resets
     * the codec manually.
     */
    mixerReset();
}

void AC97LinkState::resetWrapper(DeviceState *dev)
{
    reinterpret_cast<AC97LinkState *>(dev)->reset();
}

void AC97LinkState::realizeWrapper(PCIDevice *dev, Error **errp)
{
    reinterpret_cast<AC97LinkState *>(dev)->realize(errp);
}

void AC97LinkState::realize(Error **errp)
{
    uint8_t *c = dev.config;

    if (!AUD_backend_check (&audio_be, errp)) {
        return;
    }

    c[PCI_STATUS] = PCI_STATUS_FAST_BACK;      /* pcists pci status rwc, ro */
    c[PCI_STATUS + 1] = PCI_STATUS_DEVSEL_MEDIUM >> 8;

    c[PCI_CLASS_PROG] = 0x00;      /* pi programming interface ro */

    c[PCI_INTERRUPT_LINE] = 0x00;      /* intr_ln interrupt line rw */
    c[PCI_INTERRUPT_PIN] = 0x01;      /* intr_pn interrupt pin ro */

    memory_region_init_io(&io_nam, OBJECT(this), &ac97_io_nam_ops, this,
                          "ac97-nam", 1024);
    memory_region_init_io(&io_nabm, OBJECT(this), &ac97_io_nabm_ops, this,
                          "ac97-nabm", 256);
    pci_register_bar(&dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &io_nam);
    pci_register_bar(&dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &io_nabm);

    reset();
}

void AC97LinkState::exitWrapper(PCIDevice *pci_dev)
{
    AC97LinkState *s = reinterpret_cast<AC97LinkState *>(pci_dev);

    AUD_close_in(s->audio_be, s->voice_pi);
    AUD_close_out(s->audio_be, s->voice_po);
    AUD_close_in(s->audio_be, s->voice_mc);
}

static const Property ac97_properties[] = {
    DEFINE_AUDIO_PROPERTIES(AC97LinkState, audio_be),
};

void AC97LinkState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = realizeWrapper;
    k->exit = exitWrapper;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82801AA_5;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel 82801AA AC97 Audio";
    dc->vmsd = &vmstate_ac97;
    device_class_set_props(dc, ac97_properties);
    device_class_set_legacy_reset(dc, resetWrapper);
}

static const InterfaceInfo ac97_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo ac97_info = {
    .name          = TYPE_AC97,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AC97LinkState),
    .class_init    = AC97LinkState::classInit,
    .interfaces = ac97_interfaces,
};

static void ac97_register_types(void)
{
    type_register_static(&ac97_info);
    audio_register_model("ac97", "Intel 82801AA AC97 Audio", TYPE_AC97);
}

type_init(ac97_register_types)
