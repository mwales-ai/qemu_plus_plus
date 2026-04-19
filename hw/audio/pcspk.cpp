/*
 * QEMU PC speaker emulation
 *
 * Copyright (c) 2006 Joachim Henke
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

extern "C" {
#include "hw/isa/isa.h"
#include "hw/audio/model.h"
#include "qemu/audio.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "hw/timer/i8254.h"
#include "migration/vmstate.h"
#include "hw/audio/pcspk.h"
#include "qapi/error.h"
}

#include "qom/object.h"
#include "trace.h"

#define PCSPK_BUF_LEN 1792
#define PCSPK_SAMPLE_RATE 32000
#define PCSPK_MAX_FREQ (PCSPK_SAMPLE_RATE >> 1)
#define PCSPK_MIN_COUNT DIV_ROUND_UP(PIT_FREQ, PCSPK_MAX_FREQ)

OBJECT_DECLARE_SIMPLE_TYPE(PCSpkState, PC_SPEAKER)

struct PCSpkState {
    ISADevice parent_obj;

    MemoryRegion ioport;
    uint32_t iobase;
    uint8_t sample_buf[PCSPK_BUF_LEN];
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    PITCommonState *pit;
    unsigned int pit_count;
    unsigned int samples;
    unsigned int play_pos;
    uint8_t data_on;
    uint8_t dummy_refresh_clock;
    bool migrate;

    void generateSamples()
    {
        unsigned int i;

        if (pit_count) {
            const uint32_t m = PCSPK_SAMPLE_RATE * pit_count;
            const uint32_t n = ((uint64_t)PIT_FREQ << 32) / m;

            /* multiple of wavelength for gapless looping */
            samples = (QEMU_ALIGN_DOWN(PCSPK_BUF_LEN * PIT_FREQ, m) / (PIT_FREQ >> 1) + 1) >> 1;
            for (i = 0; i < samples; ++i)
                sample_buf[i] = (64 & (n * i >> 25)) - 32;
        } else {
            samples = PCSPK_BUF_LEN;
            for (i = 0; i < PCSPK_BUF_LEN; ++i)
                sample_buf[i] = 128; /* silence */
        }
    }

    static void callback(void *opaque, int free)
    {
        PCSpkState *s = static_cast<PCSpkState *>(opaque);
        PITChannelInfo ch;
        unsigned int n;

        pit_get_channel_info(s->pit, 2, &ch);

        if (ch.mode != 3) {
            return;
        }

        n = ch.initial_count;
        /* avoid frequencies that are not reproducible with sample rate */
        if (n < PCSPK_MIN_COUNT)
            n = 0;

        if (s->pit_count != n) {
            s->pit_count = n;
            s->play_pos = 0;
            s->generateSamples();
        }

        while (free > 0) {
            n = MIN(s->samples - s->play_pos, (unsigned int)free);
            n = AUD_write(s->voice, &s->sample_buf[s->play_pos], n);
            if (!n)
                break;
            s->play_pos = (s->play_pos + n) % s->samples;
            free -= n;
        }
    }

    int audioInit()
    {
        struct audsettings as;
        memset(&as, 0, sizeof(as));
        as.freq = PCSPK_SAMPLE_RATE;
        as.nchannels = 1;
        as.fmt = AUDIO_FORMAT_U8;
        as.endianness = 0;

        if (voice) {
            /* already initialized */
            return 0;
        }

        voice = AUD_open_out(audio_be, voice, s_spk, this, callback, &as);
        if (!voice) {
            error_report("pcspk: Could not open voice");
            return -1;
        }

        return 0;
    }

    static uint64_t ioRead(void *opaque, hwaddr addr,
                           unsigned size)
    {
        PCSpkState *s = static_cast<PCSpkState *>(opaque);
        PITChannelInfo ch;
        uint8_t val;

        pit_get_channel_info(s->pit, 2, &ch);

        s->dummy_refresh_clock ^= (1 << 4);

        val = ch.gate | (s->data_on << 1) | s->dummy_refresh_clock |
           (ch.out << 5);

        trace_pcspk_io_read(s->iobase, val);

        return val;
    }

    static void ioWrite(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
    {
        PCSpkState *s = static_cast<PCSpkState *>(opaque);
        const int gate = val & 1;

        trace_pcspk_io_write(s->iobase, val);

        s->data_on = (val >> 1) & 1;
        pit_set_gate(s->pit, 2, gate);
        if (s->voice) {
            if (gate) /* restart */
                s->play_pos = 0;
            AUD_set_active_out(s->voice, gate & s->data_on);
        }
    }

    static MemoryRegionOps ioOps;

    void realize(Error **errp)
    {
        ISADevice *isadev = reinterpret_cast<ISADevice *>(this);

        if (!pit) {
            error_setg(errp, "pcspk: No \"pit\" set or available");
            return;
        }

        isa_register_ioport(isadev, &ioport, iobase);

        if (audio_be && AUD_backend_check(&audio_be, errp)) {
            audioInit();
            return;
        }
    }

    void init()
    {
        memory_region_init_io(&ioport, OBJECT(this), &ioOps, this, "pcspk", 1);
    }

    static const char *s_spk;

    static void classInit(DeviceClass *dc);
};

const char *PCSpkState::s_spk = "pcspk";

static bool migrate_needed(void *opaque)
{
    PCSpkState *s = static_cast<PCSpkState *>(opaque);

    return s->migrate;
}

static const VMStateField vmstate_spk_fields[] = {
    VMSTATE_UINT8(data_on, PCSpkState),
    VMSTATE_UINT8(dummy_refresh_clock, PCSpkState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_spk = {
    .name = "pcspk",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = migrate_needed,
    .fields = vmstate_spk_fields,
};

static const Property pcspk_properties[] = {
    DEFINE_AUDIO_PROPERTIES(PCSpkState, audio_be),
    DEFINE_PROP_UINT32("iobase", PCSpkState, iobase,  0x61),
    DEFINE_PROP_BOOL("migrate", PCSpkState, migrate,  true),
    DEFINE_PROP_LINK("pit", PCSpkState, pit, TYPE_PIT_COMMON, PITCommonState *),
};

void PCSpkState::classInit(DeviceClass *dc)
{
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->vmsd = &vmstate_spk;
    device_class_set_props(dc, pcspk_properties);
    /* Reason: pit object link */
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(PCSpkState, TYPE_PC_SPEAKER, TYPE_ISA_DEVICE)

MemoryRegionOps PCSpkState::ioOps;

static void __attribute__((constructor)) init_pcspk_io_ops(void)
{
    memset(&PCSpkState::ioOps, 0, sizeof(PCSpkState::ioOps));
    PCSpkState::ioOps.read = PCSpkState::ioRead;
    PCSpkState::ioOps.write = PCSpkState::ioWrite;
    PCSpkState::ioOps.impl.min_access_size = 1;
    PCSpkState::ioOps.impl.max_access_size = 1;
}
