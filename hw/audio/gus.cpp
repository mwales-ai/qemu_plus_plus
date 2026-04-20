/*
 * QEMU Proxy for Gravis Ultrasound GF1 emulation by Tibor "TS" Schütz
 *
 * Copyright (c) 2002-2005 Vassili Karpov (malc)
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
#include "qapi/error.h"
#include "hw/audio/model.h"
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

extern "C" {
#include "qemu/module.h"
#include "qemu/audio.h"
#include "qemu/error-report.h"
#include "gusemu.h"
}

#define DEBUG 0

#define ldebug(fmt, ...) do { \
        if (DEBUG) { \
            error_report("gus: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define TYPE_GUS "gus"
OBJECT_DECLARE_SIMPLE_TYPE(GUSState, GUS)

struct GUSState {
    ISADevice dev;
    GUSEmuState emu;
    AudioBackend *audio_be;
    uint32_t freq;
    uint32_t port;
    int pos, left, shift, irqs;
    int16_t *mixbuf;
    uint8_t himem[1024 * 1024 + 32 + 4096];
    int samples;
    SWVoiceOut *voice;
    int64_t last_ticks;
    qemu_irq pic;
    IsaDma *isa_dma;
    PortioList portio_list1;
    PortioList portio_list2;

    static uint32_t readb(void *opaque, uint32_t nport);
    static void writeb(void *opaque, uint32_t nport, uint32_t val);
    int writeAudio(int samples);
    static void audioCallback(void *opaque, int free);
    static int readDMA(void *opaque, int nchan, int dma_pos, int dma_len);
    void realize(DeviceState *dev, Error **errp);
    static void classInit(DeviceClass *dc);
};

uint32_t GUSState::readb(void *opaque, uint32_t nport)
{
    GUSState *s = static_cast<GUSState *>(opaque);

    return gus_read (&s->emu, nport, 1);
}

void GUSState::writeb(void *opaque, uint32_t nport, uint32_t val)
{
    GUSState *s = static_cast<GUSState *>(opaque);

    gus_write (&s->emu, nport, 1, val);
}

int GUSState::writeAudio(int samples)
{
    int net = 0;
    int pos_local = pos;

    while (samples) {
        int nbytes, wbytes, wsampl;

        nbytes = samples << shift;
        wbytes = AUD_write (
            voice,
            mixbuf + (pos_local << (shift - 1)),
            nbytes
            );

        if (wbytes) {
            wsampl = wbytes >> shift;

            samples -= wsampl;
            pos_local = (pos_local + wsampl) % this->samples;

            net += wsampl;
        }
        else {
            break;
        }
    }

    return net;
}

void GUSState::audioCallback(void *opaque, int free)
{
    int samples_local, to_play, net = 0;
    GUSState *s = static_cast<GUSState *>(opaque);

    samples_local = free >> s->shift;
    to_play = MIN (samples_local, s->left);

    while (to_play) {
        int written = s->writeAudio(to_play);

        if (!written) {
            goto reset;
        }

        s->left -= written;
        to_play -= written;
        samples_local -= written;
        net += written;
    }

    samples_local = MIN (samples_local, s->samples);
    if (samples_local) {
        gus_mixvoices (&s->emu, s->freq, samples_local, s->mixbuf);

        while (samples_local) {
            int written = s->writeAudio(samples_local);
            if (!written) {
                break;
            }
            samples_local -= written;
            net += written;
        }
    }
    s->left = samples_local;

 reset:
    gus_irqgen (&s->emu, (uint64_t)net * 1000000 / s->freq);
}

extern "C"
int GUS_irqrequest (GUSEmuState *emu, int hwirq, int n)
{
    GUSState *s = static_cast<GUSState *>(emu->opaque);
    /* qemu_irq_lower (s->pic); */
    qemu_irq_raise (s->pic);
    s->irqs += n;
    ldebug("irqrequest %d %d %d", hwirq, n, s->irqs);
    return n;
}

extern "C"
void GUS_irqclear (GUSEmuState *emu, int hwirq)
{
    GUSState *s = static_cast<GUSState *>(emu->opaque);
    ldebug("irqclear %d %d", hwirq, s->irqs);
    qemu_irq_lower (s->pic);
    s->irqs -= 1;
#ifdef IRQ_STORM
    if (s->irqs > 0) {
        qemu_irq_raise (s->pic[hwirq]);
    }
#endif
}

extern "C"
void GUS_dmarequest (GUSEmuState *emu)
{
    GUSState *s = static_cast<GUSState *>(emu->opaque);
    IsaDmaClass *k = ISADMA_GET_CLASS(s->isa_dma);
    ldebug("dma request %d", s->emu.gusdma);
    k->hold_DREQ(s->isa_dma, s->emu.gusdma);
}

int GUSState::readDMA(void *opaque, int nchan, int dma_pos, int dma_len)
{
    GUSState *s = static_cast<GUSState *>(opaque);
    IsaDmaClass *k = ISADMA_GET_CLASS(s->isa_dma);
    QEMU_UNINITIALIZED char tmpbuf[4096];
    int pos_local = dma_pos, mode, left_local = dma_len - dma_pos;

    ldebug("read DMA 0x%x %d", dma_pos, dma_len);
    mode = k->has_autoinitialization(s->isa_dma, s->emu.gusdma);
    while (left_local) {
        int to_copy = MIN ((size_t) left_local, sizeof (tmpbuf));
        int copied;

        ldebug("left=%d to_copy=%d pos=%d", left_local, to_copy, pos_local);
        copied = k->read_memory(s->isa_dma, nchan, tmpbuf, pos_local, to_copy);
        gus_dma_transferdata (&s->emu, tmpbuf, copied, left_local == copied);
        left_local -= copied;
        pos_local += copied;
    }

    if (((mode >> 4) & 1) == 0) {
        k->release_DREQ(s->isa_dma, s->emu.gusdma);
    }
    return dma_len;
}

static const VMStateField vmstate_gus_fields[] = {
    VMSTATE_INT32 (pos, GUSState),
    VMSTATE_INT32 (left, GUSState),
    VMSTATE_INT32 (shift, GUSState),
    VMSTATE_INT32 (irqs, GUSState),
    VMSTATE_INT32 (samples, GUSState),
    VMSTATE_INT64 (last_ticks, GUSState),
    VMSTATE_BUFFER (himem, GUSState),
    VMSTATE_END_OF_LIST ()
};

static const VMStateDescription vmstate_gus = {
    .name = "gus",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = vmstate_gus_fields,
};

static const MemoryRegionPortio gus_portio_list1[] = {
    {0x000,  1, 1, .write = GUSState::writeb },
    {0x006, 10, 1, .read = GUSState::readb, .write = GUSState::writeb },
    {0x100,  8, 1, .read = GUSState::readb, .write = GUSState::writeb },
    PORTIO_END_OF_LIST (),
};

static const MemoryRegionPortio gus_portio_list2[] = {
    {0, 2, 1, .read = GUSState::readb },
    PORTIO_END_OF_LIST (),
};

static void gus_realizefn(DeviceState *dev, Error **errp)
{
    GUSState *s = GUS(dev);
    s->realize(dev, errp);
}

void GUSState::realize(DeviceState *dev, Error **errp)
{
    ISADevice *d = ISA_DEVICE(dev);
    ISABus *bus = isa_bus_from_device(d);
    GUSState *s = GUS (dev);
    IsaDmaClass *k;
    struct audsettings as;

    if (!AUD_backend_check(&s->audio_be, errp)) {
        return;
    }

    s->isa_dma = isa_bus_get_dma(bus, s->emu.gusdma);
    if (!s->isa_dma) {
        error_setg(errp, "ISA controller does not support DMA");
        return;
    }

    as.freq = s->freq;
    as.nchannels = 2;
    as.fmt = AUDIO_FORMAT_S16;
    as.endianness = HOST_BIG_ENDIAN;

    s->voice = AUD_open_out (
        s->audio_be,
        NULL,
        "gus",
        s,
        GUSState::audioCallback,
        &as
        );

    if (!s->voice) {
        error_setg(errp, "No voice");
        return;
    }

    s->shift = 2;
    s->samples = AUD_get_buffer_size_out (s->voice) >> s->shift;
    s->mixbuf = static_cast<int16_t *>(g_malloc0 (s->samples << s->shift));

    isa_register_portio_list(d, &s->portio_list1, s->port,
                             gus_portio_list1, s, "gus");
    isa_register_portio_list(d, &s->portio_list2, (s->port + 0x100) & 0xf00,
                             gus_portio_list2, s, "gus");

    k = ISADMA_GET_CLASS(s->isa_dma);
    k->register_channel(s->isa_dma, s->emu.gusdma, GUSState::readDMA, s);
    s->emu.himemaddr = s->himem;
    s->emu.gusdatapos = s->emu.himemaddr + 1024 * 1024 + 32;
    s->emu.opaque = s;
    s->pic = isa_bus_get_irq(bus, s->emu.gusirq);

    AUD_set_active_out (s->voice, 1);
}

static const Property gus_properties[] = {
    DEFINE_AUDIO_PROPERTIES(GUSState, audio_be),
    DEFINE_PROP_UINT32 ("freq",    GUSState, freq,        44100),
    DEFINE_PROP_UINT32 ("iobase",  GUSState, port,        0x240),
    DEFINE_PROP_UINT32 ("irq",     GUSState, emu.gusirq,  7),
    DEFINE_PROP_UINT32 ("dma",     GUSState, emu.gusdma,  3),
};

void GUSState::classInit(DeviceClass *dc)
{
    dc->realize = gus_realizefn;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Gravis Ultrasound GF1";
    dc->vmsd = &vmstate_gus;
    device_class_set_props(dc, gus_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(GUSState, TYPE_GUS, TYPE_ISA_DEVICE)

static void __attribute__((constructor)) gus_audio_init(void)
{
    audio_register_model("gus", "Gravis Ultrasound GF1", TYPE_GUS);
}
