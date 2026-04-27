/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * written by Gerd Hoffmann <kraxel@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/pci/msi.h"
#include "monitor/qdev.h"
#include "qemu/timer.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/audio/model.h"
#include "intel-hda.h"
#include "migration/vmstate.h"
#include "intel-hda-defs.h"
#include "qobject/qdict.h"
#include "qapi/error.h"
#include "qom/object.h"

/* --------------------------------------------------------------------- */
/* hda bus                                                               */

static const Property hda_props[] = {
    DEFINE_PROP_UINT32("cad", HDACodecDevice, cad, -1),
};

static const TypeInfo hda_codec_bus_info = {
    .name = TYPE_HDA_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(HDACodecBus),
};

void hda_codec_bus_init(DeviceState *dev, HDACodecBus *bus, size_t bus_size,
                        hda_codec_response_func response,
                        hda_codec_xfer_func xfer)
{
    qbus_init(bus, bus_size, TYPE_HDA_BUS, dev, NULL);
    bus->response = response;
    bus->xfer = xfer;
}

static void hda_codec_dev_realize(DeviceState *qdev, Error **errp)
{
    HDACodecBus *bus = reinterpret_cast<HDACodecBus *>(qdev->parent_bus);
    HDACodecDevice *dev = reinterpret_cast<HDACodecDevice *>(qdev);
    HDACodecDeviceClass *cdc = HDA_CODEC_DEVICE_GET_CLASS(dev);

    if (dev->cad == -1) {
        dev->cad = bus->next_cad;
    }
    if (dev->cad >= 15) {
        error_setg(errp, "HDA audio codec address is full");
        return;
    }
    bus->next_cad = dev->cad + 1;
    cdc->init(dev, errp);
}

static void hda_codec_dev_unrealize(DeviceState *qdev)
{
    HDACodecDevice *dev = reinterpret_cast<HDACodecDevice *>(qdev);
    HDACodecDeviceClass *cdc = HDA_CODEC_DEVICE_GET_CLASS(dev);

    if (cdc->exit) {
        cdc->exit(dev);
    }
}

HDACodecDevice *hda_codec_find(HDACodecBus *bus, uint32_t cad)
{
    BusChild *kid;
    HDACodecDevice *cdev;

    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        DeviceState *qdev = kid->child;
        cdev = reinterpret_cast<HDACodecDevice *>(qdev);
        if (cdev->cad == cad) {
            return cdev;
        }
    }
    return NULL;
}

void hda_codec_response(HDACodecDevice *dev, bool solicited, uint32_t response)
{
    HDACodecBus *bus = reinterpret_cast<HDACodecBus *>(dev->qdev.parent_bus);
    bus->response(dev, solicited, response);
}

bool hda_codec_xfer(HDACodecDevice *dev, uint32_t stnr, bool output,
                    uint8_t *buf, uint32_t len)
{
    HDACodecBus *bus = reinterpret_cast<HDACodecBus *>(dev->qdev.parent_bus);
    return bus->xfer(dev, stnr, output, buf, len);
}

/* --------------------------------------------------------------------- */
/* intel hda emulation                                                   */

typedef struct IntelHDAStream IntelHDAStream;
typedef struct IntelHDAState IntelHDAState;
typedef struct IntelHDAReg IntelHDAReg;

typedef struct BDLEntry {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} BDLEntry;

struct IntelHDAStream {
    /* registers */
    uint32_t ctl;
    uint32_t lpib;
    uint32_t cbl;
    uint32_t lvi;
    uint32_t fmt;
    uint32_t bdlp_lbase;
    uint32_t bdlp_ubase;

    /* state */
    BDLEntry *bpl;
    uint32_t bentries;
    uint32_t bsize, be, bp;
};

struct IntelHDAState {
    PCIDevice pci;
    const char *name;
    HDACodecBus codecs;

    /* registers */
    uint32_t g_ctl;
    uint32_t wake_en;
    uint32_t state_sts;
    uint32_t int_ctl;
    uint32_t int_sts;
    uint32_t wall_clk;

    uint32_t corb_lbase;
    uint32_t corb_ubase;
    uint32_t corb_rp;
    uint32_t corb_wp;
    uint32_t corb_ctl;
    uint32_t corb_sts;
    uint32_t corb_size;

    uint32_t rirb_lbase;
    uint32_t rirb_ubase;
    uint32_t rirb_wp;
    uint32_t rirb_cnt;
    uint32_t rirb_ctl;
    uint32_t rirb_sts;
    uint32_t rirb_size;

    uint32_t dp_lbase;
    uint32_t dp_ubase;

    uint32_t icw;
    uint32_t irr;
    uint32_t ics;

    /* streams */
    IntelHDAStream st[8];

    /* state */
    MemoryRegion container;
    MemoryRegion mmio;
    MemoryRegion alias;
    uint32_t rirb_count;
    int64_t wall_base_ns;

    /* debug logging */
    const IntelHDAReg *last_reg;
    uint32_t last_val;
    uint32_t last_write;
    uint32_t last_sec;
    uint32_t repeat_count;

    /* properties */
    uint32_t debug;
    OnOffAuto msi;
    bool old_msi_addr;

    /* Internal methods */
    void updateIntSts();
    void updateIrq();
    int sendCommand(uint32_t verb);
    void corbRun();
    void parseBdl(IntelHDAStream *st);
    void notifyCodecs(uint32_t stream, bool running, bool output);
    const IntelHDAReg *regFind(hwaddr addr);
    uint32_t *regAddr(const IntelHDAReg *reg);
    void regWrite(const IntelHDAReg *reg, uint32_t val, uint32_t wmask);
    uint32_t regRead(const IntelHDAReg *reg, uint32_t rmask);
    void regsReset();

    /* QOM methods */
    void realize(Error **errp);
    void reset();

    static void realizeWrapper(PCIDevice *pci, Error **errp);
    static void resetWrapper(DeviceState *dev);
    static void classInit(DeviceClass *dc);
    static void hdaCodecDeviceClassInit(ObjectClass *klass, const void *data);
};

#define TYPE_INTEL_HDA_GENERIC "intel-hda-generic"

DECLARE_INSTANCE_CHECKER(IntelHDAState, INTEL_HDA,
                         TYPE_INTEL_HDA_GENERIC)

struct IntelHDAReg {
    const char *name;      /* register name */
    uint32_t   size;       /* size in bytes */
    uint32_t   reset;      /* reset value */
    uint32_t   wmask;      /* write mask */
    uint32_t   wclear;     /* write 1 to clear bits */
    uint32_t   offset;     /* location in IntelHDAState */
    uint32_t   shift;      /* byte access entries for dwords */
    uint32_t   stream;
    void       (*whandler)(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
    void       (*rhandler)(IntelHDAState *d, const IntelHDAReg *reg);
};

/* --------------------------------------------------------------------- */

static hwaddr intel_hda_addr(uint32_t lbase, uint32_t ubase)
{
    return ((uint64_t)ubase << 32) | lbase;
}

void IntelHDAState::updateIntSts()
{
    uint32_t sts = 0;
    uint32_t i;

    /* update controller status */
    if (rirb_sts & ICH6_RBSTS_IRQ) {
        sts |= (1 << 30);
    }
    if (rirb_sts & ICH6_RBSTS_OVERRUN) {
        sts |= (1 << 30);
    }
    if (state_sts & wake_en) {
        sts |= (1 << 30);
    }

    /* update stream status */
    for (i = 0; i < 8; i++) {
        /* buffer completion interrupt */
        if (st[i].ctl & (1 << 26)) {
            sts |= (1 << i);
        }
    }

    /* update global status */
    if (sts & int_ctl) {
        sts |= (1U << 31);
    }

    int_sts = sts;
}

void IntelHDAState::updateIrq()
{
    bool msi_on = msi_enabled(&pci);
    int level;

    updateIntSts();
    if (int_sts & (1U << 31) && int_ctl & (1U << 31)) {
        level = 1;
    } else {
        level = 0;
    }
    dprint(this, 2, "%s: level %d [%s]\n", __func__,
           level, msi_on ? "msi" : "intx");
    if (msi_on) {
        if (level) {
            msi_notify(&pci, 0);
        }
    } else {
        pci_set_irq(&pci, level);
    }
}

int IntelHDAState::sendCommand(uint32_t verb)
{
    uint32_t cad, nid, data;
    HDACodecDevice *codec;
    HDACodecDeviceClass *cdc;

    cad = (verb >> 28) & 0x0f;
    if (verb & (1 << 27)) {
        /* indirect node addressing, not specified in HDA 1.0 */
        dprint(this, 1, "%s: indirect node addressing (guest bug?)\n", __func__);
        return -1;
    }
    nid = (verb >> 20) & 0x7f;
    data = verb & 0xfffff;

    codec = hda_codec_find(&codecs, cad);
    if (codec == NULL) {
        dprint(this, 1, "%s: addressed non-existing codec\n", __func__);
        return -1;
    }
    cdc = HDA_CODEC_DEVICE_GET_CLASS(codec);
    cdc->command(codec, nid, data);
    return 0;
}

void IntelHDAState::corbRun()
{
    hwaddr addr;
    uint32_t rp, verb;

    if (ics & ICH6_IRS_BUSY) {
        dprint(this, 2, "%s: [icw] verb 0x%08x\n", __func__, icw);
        sendCommand(icw);
        return;
    }

    for (;;) {
        if (!(corb_ctl & ICH6_CORBCTL_RUN)) {
            dprint(this, 2, "%s: !run\n", __func__);
            return;
        }
        if ((corb_rp & 0xff) == corb_wp) {
            dprint(this, 2, "%s: corb ring empty\n", __func__);
            return;
        }
        if (rirb_count == rirb_cnt) {
            dprint(this, 2, "%s: rirb count reached\n", __func__);
            return;
        }

        rp = (corb_rp + 1) & 0xff;
        addr = intel_hda_addr(corb_lbase, corb_ubase);
        ldl_le_pci_dma(&pci, addr + 4 * rp, &verb, MEMTXATTRS_UNSPECIFIED);
        corb_rp = rp;

        dprint(this, 2, "%s: [rp 0x%x] verb 0x%08x\n", __func__, rp, verb);
        sendCommand(verb);
    }
}

static void intel_hda_response(HDACodecDevice *dev, bool solicited, uint32_t response)
{
    const MemTxAttrs attrs = { .memory = true };
    HDACodecBus *bus = reinterpret_cast<HDACodecBus *>(dev->qdev.parent_bus);
    IntelHDAState *d = container_of(bus, IntelHDAState, codecs);
    hwaddr addr;
    uint32_t wp, ex;
    MemTxResult res = MEMTX_OK;

    if (d->ics & ICH6_IRS_BUSY) {
        dprint(d, 2, "%s: [irr] response 0x%x, cad 0x%x\n",
               __func__, response, dev->cad);
        d->irr = response;
        d->ics &= ~(ICH6_IRS_BUSY | 0xf0);
        d->ics |= (ICH6_IRS_VALID | (dev->cad << 4));
        return;
    }

    if (!(d->rirb_ctl & ICH6_RBCTL_DMA_EN)) {
        dprint(d, 1, "%s: rirb dma disabled, drop codec response\n", __func__);
        return;
    }

    ex = (solicited ? 0 : (1 << 4)) | dev->cad;
    wp = (d->rirb_wp + 1) & 0xff;
    addr = intel_hda_addr(d->rirb_lbase, d->rirb_ubase);
    res |= stl_le_pci_dma(&d->pci, addr + 8 * wp, response, attrs);
    res |= stl_le_pci_dma(&d->pci, addr + 8 * wp + 4, ex, attrs);
    if (res != MEMTX_OK && (d->rirb_ctl & ICH6_RBCTL_OVERRUN_EN)) {
        d->rirb_sts |= ICH6_RBSTS_OVERRUN;
        d->updateIrq();
    }
    d->rirb_wp = wp;

    dprint(d, 2, "%s: [wp 0x%x] response 0x%x, extra 0x%x\n",
           __func__, wp, response, ex);

    d->rirb_count++;
    if (d->rirb_count == d->rirb_cnt) {
        dprint(d, 2, "%s: rirb count reached (%d)\n", __func__, d->rirb_count);
        if (d->rirb_ctl & ICH6_RBCTL_IRQ_EN) {
            d->rirb_sts |= ICH6_RBSTS_IRQ;
            d->updateIrq();
        }
    } else if ((d->corb_rp & 0xff) == d->corb_wp) {
        dprint(d, 2, "%s: corb ring empty (%d/%d)\n", __func__,
               d->rirb_count, d->rirb_cnt);
        if (d->rirb_ctl & ICH6_RBCTL_IRQ_EN) {
            d->rirb_sts |= ICH6_RBSTS_IRQ;
            d->updateIrq();
        }
    }
}

static bool intel_hda_xfer(HDACodecDevice *dev, uint32_t stnr, bool output,
                           uint8_t *buf, uint32_t len)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    HDACodecBus *bus = reinterpret_cast<HDACodecBus *>(dev->qdev.parent_bus);
    IntelHDAState *d = container_of(bus, IntelHDAState, codecs);
    hwaddr addr;
    uint32_t s, copy, left;
    IntelHDAStream *st;
    bool irq = false;

    st = output ? d->st + 4 : d->st;
    for (s = 0; s < 4; s++) {
        if (stnr == ((st[s].ctl >> 20) & 0x0f)) {
            st = st + s;
            break;
        }
    }
    if (s == 4) {
        return false;
    }
    if (st->bpl == NULL) {
        return false;
    }

    left = len;
    s = st->bentries;
    while (left > 0 && s-- > 0) {
        copy = left;
        if (copy > st->bsize - st->lpib)
            copy = st->bsize - st->lpib;
        if (copy > st->bpl[st->be].len - st->bp)
            copy = st->bpl[st->be].len - st->bp;

        dprint(d, 3, "dma: entry %d, pos %d/%d, copy %d\n",
               st->be, st->bp, st->bpl[st->be].len, copy);

        pci_dma_rw(&d->pci, st->bpl[st->be].addr + st->bp, buf, copy,
                   output ? DMA_DIRECTION_FROM_DEVICE : DMA_DIRECTION_TO_DEVICE,
                   attrs);
        st->lpib += copy;
        st->bp += copy;
        buf += copy;
        left -= copy;

        if (st->bpl[st->be].len == st->bp) {
            /* bpl entry filled */
            if (st->bpl[st->be].flags & 0x01) {
                irq = true;
            }
            st->bp = 0;
            st->be++;
            if (st->be == st->bentries) {
                /* bpl wrap around */
                st->be = 0;
                st->lpib = 0;
            }
        }
    }
    if (d->dp_lbase & 0x01) {
        s = st - d->st;
        addr = intel_hda_addr(d->dp_lbase & ~0x01, d->dp_ubase);
        stl_le_pci_dma(&d->pci, addr + 8 * s, st->lpib, attrs);
    }
    dprint(d, 3, "dma: --\n");

    if (irq) {
        st->ctl |= (1 << 26); /* buffer completion interrupt */
        d->updateIrq();
    }
    return true;
}

void IntelHDAState::parseBdl(IntelHDAStream *stream)
{
    hwaddr addr;
    uint8_t buf[16];
    uint32_t i;

    addr = intel_hda_addr(stream->bdlp_lbase, stream->bdlp_ubase);
    stream->bentries = stream->lvi +1;
    g_free(stream->bpl);
    stream->bpl = g_new(BDLEntry, stream->bentries);
    for (i = 0; i < stream->bentries; i++, addr += 16) {
        pci_dma_read(&pci, addr, buf, 16);
        stream->bpl[i].addr  = le64_to_cpu(*(uint64_t *)buf);
        stream->bpl[i].len   = le32_to_cpu(*(uint32_t *)(buf + 8));
        stream->bpl[i].flags = le32_to_cpu(*(uint32_t *)(buf + 12));
        dprint(this, 1, "bdl/%d: 0x%" PRIx64 " +0x%x, 0x%x\n",
               i, stream->bpl[i].addr, stream->bpl[i].len, stream->bpl[i].flags);
    }

    stream->bsize = stream->cbl;
    stream->lpib  = 0;
    stream->be    = 0;
    stream->bp    = 0;
}

void IntelHDAState::notifyCodecs(uint32_t stream, bool running, bool output)
{
    BusChild *kid;
    HDACodecDevice *cdev;

    QTAILQ_FOREACH(kid, &codecs.qbus.children, sibling) {
        DeviceState *qdev = kid->child;
        HDACodecDeviceClass *cdc;

        cdev = reinterpret_cast<HDACodecDevice *>(qdev);
        cdc = HDA_CODEC_DEVICE_GET_CLASS(cdev);
        if (cdc->stream) {
            cdc->stream(cdev, stream, running, output);
        }
    }
}

/* --------------------------------------------------------------------- */

static void intel_hda_set_g_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    if ((d->g_ctl & ICH6_GCTL_RESET) == 0) {
        device_cold_reset(reinterpret_cast<DeviceState *>(d));
    }
}

static void intel_hda_set_wake_en(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    d->updateIrq();
}

static void intel_hda_set_state_sts(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    d->updateIrq();
}

static void intel_hda_set_int_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    d->updateIrq();
}

static void intel_hda_get_wall_clk(IntelHDAState *d, const IntelHDAReg *reg)
{
    int64_t ns;

    ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - d->wall_base_ns;
    d->wall_clk = (uint32_t)(ns * 24 / 1000);  /* 24 MHz */
}

static void intel_hda_set_corb_wp(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    d->corbRun();
}

static void intel_hda_set_corb_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    d->corbRun();
}

static void intel_hda_set_rirb_wp(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    if (d->rirb_wp & ICH6_RIRBWP_RST) {
        d->rirb_wp = 0;
    }
}

static void intel_hda_set_rirb_sts(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    d->updateIrq();

    if ((old & ICH6_RBSTS_IRQ) && !(d->rirb_sts & ICH6_RBSTS_IRQ)) {
        /* cleared ICH6_RBSTS_IRQ */
        d->rirb_count = 0;
        d->corbRun();
    }
}

static void intel_hda_set_ics(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    if (d->ics & ICH6_IRS_BUSY) {
        d->corbRun();
    }
}

static void intel_hda_set_st_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    bool output = reg->stream >= 4;
    IntelHDAStream *st = d->st + reg->stream;

    if (st->ctl & 0x01) {
        /* reset */
        dprint(d, 1, "st #%d: reset\n", reg->stream);
        st->ctl = SD_STS_FIFO_READY << 24 | SD_CTL_STREAM_RESET;
    }
    if ((st->ctl & 0x02) != (old & 0x02)) {
        uint32_t stnr = (st->ctl >> 20) & 0x0f;
        /* run bit flipped */
        if (st->ctl & 0x02) {
            /* start */
            dprint(d, 1, "st #%d: start %d (ring buf %d bytes)\n",
                   reg->stream, stnr, st->cbl);
            d->parseBdl(st);
            d->notifyCodecs(stnr, true, output);
        } else {
            /* stop */
            dprint(d, 1, "st #%d: stop %d\n", reg->stream, stnr);
            d->notifyCodecs(stnr, false, output);
        }
    }
    d->updateIrq();
}

/* --------------------------------------------------------------------- */

#define ST_REG(_n, _o) (0x80 + (_n) * 0x20 + (_o))

/* Maximum register address in the table */
#define REGTAB_SIZE (ST_REG(7, ICH6_REG_SD_BDLPU) + 1)

static struct IntelHDAReg regtab[REGTAB_SIZE];

static void intel_hda_set_reg(int idx, const char *name, uint32_t size,
                              uint32_t reset, uint32_t wmask, uint32_t wclear,
                              uint32_t offset, uint32_t shift, uint32_t stream,
                              void (*whandler)(IntelHDAState *, const IntelHDAReg *, uint32_t),
                              void (*rhandler)(IntelHDAState *, const IntelHDAReg *))
{
    regtab[idx].name     = name;
    regtab[idx].size     = size;
    regtab[idx].reset    = reset;
    regtab[idx].wmask    = wmask;
    regtab[idx].wclear   = wclear;
    regtab[idx].offset   = offset;
    regtab[idx].shift    = shift;
    regtab[idx].stream   = stream;
    regtab[idx].whandler = whandler;
    regtab[idx].rhandler = rhandler;
}

#define HDA_STREAM_INIT(_t, _i)  do {                                 \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_CTL),                    \
        _t stringify(_i) " CTL", 4, 0, 0x1cff001f, 0,                \
        offsetof(IntelHDAState, st[_i].ctl), 0, _i,                   \
        intel_hda_set_st_ctl, NULL);                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_CTL) + 2,                \
        _t stringify(_i) " CTL(stnr)", 1, 0, 0x00ff0000, 0,          \
        offsetof(IntelHDAState, st[_i].ctl), 16, _i,                  \
        intel_hda_set_st_ctl, NULL);                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_STS),                    \
        _t stringify(_i) " CTL(sts)", 1, SD_STS_FIFO_READY << 24,    \
        0x1c000000, 0x1c000000,                                       \
        offsetof(IntelHDAState, st[_i].ctl), 24, _i,                  \
        intel_hda_set_st_ctl, NULL);                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_LPIB),                   \
        _t stringify(_i) " LPIB", 4, 0, 0, 0,                        \
        offsetof(IntelHDAState, st[_i].lpib), 0, _i,                  \
        NULL, NULL);                                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_CBL),                    \
        _t stringify(_i) " CBL", 4, 0, 0xffffffff, 0,                \
        offsetof(IntelHDAState, st[_i].cbl), 0, _i,                   \
        NULL, NULL);                                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_LVI),                    \
        _t stringify(_i) " LVI", 2, 0, 0x00ff, 0,                    \
        offsetof(IntelHDAState, st[_i].lvi), 0, _i,                   \
        NULL, NULL);                                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_FIFOSIZE),               \
        _t stringify(_i) " FIFOS", 2, HDA_BUFFER_SIZE, 0, 0,         \
        0, 0, _i, NULL, NULL);                                        \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_FORMAT),                 \
        _t stringify(_i) " FMT", 2, 0, 0x7f7f, 0,                   \
        offsetof(IntelHDAState, st[_i].fmt), 0, _i,                   \
        NULL, NULL);                                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_BDLPL),                  \
        _t stringify(_i) " BDLPL", 4, 0, 0xffffff80, 0,              \
        offsetof(IntelHDAState, st[_i].bdlp_lbase), 0, _i,            \
        NULL, NULL);                                                   \
    intel_hda_set_reg(ST_REG(_i, ICH6_REG_SD_BDLPU),                  \
        _t stringify(_i) " BDLPU", 4, 0, 0xffffffff, 0,              \
        offsetof(IntelHDAState, st[_i].bdlp_ubase), 0, _i,            \
        NULL, NULL);                                                   \
    } while (0)

static void __attribute__((constructor)) init_regtab(void)
{
    memset(regtab, 0, sizeof(regtab));

    /* global */
    intel_hda_set_reg(ICH6_REG_GCAP,
        "GCAP", 2, 0x4401, 0, 0, 0, 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_VMIN,
        "VMIN", 1, 0, 0, 0, 0, 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_VMAJ,
        "VMAJ", 1, 1, 0, 0, 0, 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_OUTPAY,
        "OUTPAY", 2, 0x3c, 0, 0, 0, 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_INPAY,
        "INPAY", 2, 0x1d, 0, 0, 0, 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_GCTL,
        "GCTL", 4, 0, 0x0103, 0,
        offsetof(IntelHDAState, g_ctl), 0, 0,
        intel_hda_set_g_ctl, NULL);
    intel_hda_set_reg(ICH6_REG_WAKEEN,
        "WAKEEN", 2, 0, 0x7fff, 0,
        offsetof(IntelHDAState, wake_en), 0, 0,
        intel_hda_set_wake_en, NULL);
    intel_hda_set_reg(ICH6_REG_STATESTS,
        "STATESTS", 2, 0, 0x7fff, 0x7fff,
        offsetof(IntelHDAState, state_sts), 0, 0,
        intel_hda_set_state_sts, NULL);

    /* interrupts */
    intel_hda_set_reg(ICH6_REG_INTCTL,
        "INTCTL", 4, 0, 0xc00000ff, 0,
        offsetof(IntelHDAState, int_ctl), 0, 0,
        intel_hda_set_int_ctl, NULL);
    intel_hda_set_reg(ICH6_REG_INTSTS,
        "INTSTS", 4, 0, 0xc00000ff, 0xc00000ff,
        offsetof(IntelHDAState, int_sts), 0, 0, NULL, NULL);

    /* misc */
    intel_hda_set_reg(ICH6_REG_WALLCLK,
        "WALLCLK", 4, 0, 0, 0,
        offsetof(IntelHDAState, wall_clk), 0, 0,
        NULL, intel_hda_get_wall_clk);

    /* dma engine */
    intel_hda_set_reg(ICH6_REG_CORBLBASE,
        "CORBLBASE", 4, 0, 0xffffff80, 0,
        offsetof(IntelHDAState, corb_lbase), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_CORBUBASE,
        "CORBUBASE", 4, 0, 0xffffffff, 0,
        offsetof(IntelHDAState, corb_ubase), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_CORBWP,
        "CORBWP", 2, 0, 0xff, 0,
        offsetof(IntelHDAState, corb_wp), 0, 0,
        intel_hda_set_corb_wp, NULL);
    intel_hda_set_reg(ICH6_REG_CORBRP,
        "CORBRP", 2, 0, 0x80ff, 0,
        offsetof(IntelHDAState, corb_rp), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_CORBCTL,
        "CORBCTL", 1, 0, 0x03, 0,
        offsetof(IntelHDAState, corb_ctl), 0, 0,
        intel_hda_set_corb_ctl, NULL);
    intel_hda_set_reg(ICH6_REG_CORBSTS,
        "CORBSTS", 1, 0, 0x01, 0x01,
        offsetof(IntelHDAState, corb_sts), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_CORBSIZE,
        "CORBSIZE", 1, 0x42, 0, 0,
        offsetof(IntelHDAState, corb_size), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_RIRBLBASE,
        "RIRBLBASE", 4, 0, 0xffffff80, 0,
        offsetof(IntelHDAState, rirb_lbase), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_RIRBUBASE,
        "RIRBUBASE", 4, 0, 0xffffffff, 0,
        offsetof(IntelHDAState, rirb_ubase), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_RIRBWP,
        "RIRBWP", 2, 0, 0x8000, 0,
        offsetof(IntelHDAState, rirb_wp), 0, 0,
        intel_hda_set_rirb_wp, NULL);
    intel_hda_set_reg(ICH6_REG_RINTCNT,
        "RINTCNT", 2, 0, 0xff, 0,
        offsetof(IntelHDAState, rirb_cnt), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_RIRBCTL,
        "RIRBCTL", 1, 0, 0x07, 0,
        offsetof(IntelHDAState, rirb_ctl), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_RIRBSTS,
        "RIRBSTS", 1, 0, 0x05, 0x05,
        offsetof(IntelHDAState, rirb_sts), 0, 0,
        intel_hda_set_rirb_sts, NULL);
    intel_hda_set_reg(ICH6_REG_RIRBSIZE,
        "RIRBSIZE", 1, 0x42, 0, 0,
        offsetof(IntelHDAState, rirb_size), 0, 0, NULL, NULL);

    intel_hda_set_reg(ICH6_REG_DPLBASE,
        "DPLBASE", 4, 0, 0xffffff81, 0,
        offsetof(IntelHDAState, dp_lbase), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_DPUBASE,
        "DPUBASE", 4, 0, 0xffffffff, 0,
        offsetof(IntelHDAState, dp_ubase), 0, 0, NULL, NULL);

    intel_hda_set_reg(ICH6_REG_IC,
        "ICW", 4, 0, 0xffffffff, 0,
        offsetof(IntelHDAState, icw), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_IR,
        "IRR", 4, 0, 0, 0,
        offsetof(IntelHDAState, irr), 0, 0, NULL, NULL);
    intel_hda_set_reg(ICH6_REG_IRS,
        "ICS", 2, 0, 0x0003, 0x0002,
        offsetof(IntelHDAState, ics), 0, 0,
        intel_hda_set_ics, NULL);

    HDA_STREAM_INIT("IN", 0);
    HDA_STREAM_INIT("IN", 1);
    HDA_STREAM_INIT("IN", 2);
    HDA_STREAM_INIT("IN", 3);

    HDA_STREAM_INIT("OUT", 4);
    HDA_STREAM_INIT("OUT", 5);
    HDA_STREAM_INIT("OUT", 6);
    HDA_STREAM_INIT("OUT", 7);
}

const IntelHDAReg *IntelHDAState::regFind(hwaddr addr)
{
    const IntelHDAReg *reg;

    if (addr >= ARRAY_SIZE(regtab)) {
        goto noreg;
    }
    reg = regtab+addr;
    if (reg->name == NULL) {
        goto noreg;
    }
    return reg;

noreg:
    dprint(this, 1, "unknown register, addr 0x%x\n", (int) addr);
    return NULL;
}

uint32_t *IntelHDAState::regAddr(const IntelHDAReg *reg)
{
    uint8_t *addr = reinterpret_cast<uint8_t *>(this);

    addr += reg->offset;
    return (uint32_t*)addr;
}

void IntelHDAState::regWrite(const IntelHDAReg *reg, uint32_t val,
                             uint32_t wmask)
{
    uint32_t *addr;
    uint32_t old;

    if (!reg) {
        return;
    }
    if (!reg->wmask) {
        qemu_log_mask(LOG_GUEST_ERROR, "intel-hda: write to r/o reg %s\n",
                      reg->name);
        return;
    }

    if (debug) {
        time_t now = time(NULL);
        if (last_write && last_reg == reg && last_val == val) {
            repeat_count++;
            if (last_sec != now) {
                dprint(this, 2, "previous register op repeated %d times\n", repeat_count);
                last_sec = now;
                repeat_count = 0;
            }
        } else {
            if (repeat_count) {
                dprint(this, 2, "previous register op repeated %d times\n", repeat_count);
            }
            dprint(this, 2, "write %-16s: 0x%x (%x)\n", reg->name, val, wmask);
            last_write = 1;
            last_reg   = reg;
            last_val   = val;
            last_sec   = now;
            repeat_count = 0;
        }
    }
    assert(reg->offset != 0);

    addr = regAddr(reg);
    old = *addr;

    if (reg->shift) {
        val <<= reg->shift;
        wmask <<= reg->shift;
    }
    wmask &= reg->wmask;
    *addr &= ~wmask;
    *addr |= wmask & val;
    *addr &= ~(val & reg->wclear);

    if (reg->whandler) {
        reg->whandler(this, reg, old);
    }
}

uint32_t IntelHDAState::regRead(const IntelHDAReg *reg,
                                uint32_t rmask)
{
    uint32_t *addr, ret;

    if (!reg) {
        return 0;
    }

    if (reg->rhandler) {
        reg->rhandler(this, reg);
    }

    if (reg->offset == 0) {
        /* constant read-only register */
        ret = reg->reset;
    } else {
        addr = regAddr(reg);
        ret = *addr;
        if (reg->shift) {
            ret >>= reg->shift;
        }
        ret &= rmask;
    }
    if (debug) {
        time_t now = time(NULL);
        if (!last_write && last_reg == reg && last_val == ret) {
            repeat_count++;
            if (last_sec != now) {
                dprint(this, 2, "previous register op repeated %d times\n", repeat_count);
                last_sec = now;
                repeat_count = 0;
            }
        } else {
            if (repeat_count) {
                dprint(this, 2, "previous register op repeated %d times\n", repeat_count);
            }
            dprint(this, 2, "read  %-16s: 0x%x (%x)\n", reg->name, ret, rmask);
            last_write = 0;
            last_reg   = reg;
            last_val   = ret;
            last_sec   = now;
            repeat_count = 0;
        }
    }
    return ret;
}

void IntelHDAState::regsReset()
{
    uint32_t *addr;
    int i;

    for (i = 0; i < ARRAY_SIZE(regtab); i++) {
        if (regtab[i].name == NULL) {
            continue;
        }
        if (regtab[i].offset == 0) {
            continue;
        }
        addr = regAddr(regtab + i);
        *addr = regtab[i].reset;
    }
}

/* --------------------------------------------------------------------- */

static void intel_hda_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    IntelHDAState *d = static_cast<IntelHDAState *>(opaque);
    const IntelHDAReg *reg = d->regFind(addr);

    d->regWrite(reg, val, MAKE_64BIT_MASK(0, size * 8));
}

static uint64_t intel_hda_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    IntelHDAState *d = static_cast<IntelHDAState *>(opaque);
    const IntelHDAReg *reg = d->regFind(addr);

    return d->regRead(reg, MAKE_64BIT_MASK(0, size * 8));
}

static const MemoryRegionOps intel_hda_mmio_ops = {
    .read = intel_hda_mmio_read,
    .write = intel_hda_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* --------------------------------------------------------------------- */

void IntelHDAState::resetWrapper(DeviceState *dev)
{
    reinterpret_cast<IntelHDAState *>(dev)->reset();
}

void IntelHDAState::reset()
{
    BusChild *kid;
    HDACodecDevice *cdev;

    regsReset();
    wall_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    QTAILQ_FOREACH(kid, &codecs.qbus.children, sibling) {
        DeviceState *qdev = kid->child;
        cdev = reinterpret_cast<HDACodecDevice *>(qdev);
        state_sts |= (1 << cdev->cad);
    }
    updateIrq();
}

void IntelHDAState::realizeWrapper(PCIDevice *pci_dev, Error **errp)
{
    reinterpret_cast<IntelHDAState *>(pci_dev)->realize(errp);
}

void IntelHDAState::realize(Error **errp)
{
    uint8_t *conf = pci.config;
    Error *err = NULL;
    int ret;

    name = object_get_typename(reinterpret_cast<Object *>(this));

    pci_config_set_interrupt_pin(conf, 1);

    /* HDCTL off 0x40 bit 0 selects signaling mode (1-HDA, 0 - Ac97) 18.1.19 */
    conf[0x40] = 0x01;

    if (msi != ON_OFF_AUTO_OFF) {
        ret = msi_init(&pci, old_msi_addr ? 0x50 : 0x60,
                       1, true, false, &err);
        /* Any error other than -ENOTSUP(board's MSI support is broken)
         * is a programming error */
        assert(!ret || ret == -ENOTSUP);
        if (ret && msi == ON_OFF_AUTO_ON) {
            /* Can't satisfy user's explicit msi=on request, fail */
            error_append_hint(&err, "You have to use msi=auto (default) or "
                    "msi=off with this machine type.\n");
            error_propagate(errp, err);
            return;
        }
        assert(!err || msi == ON_OFF_AUTO_AUTO);
        /* With msi=auto, we fall back to MSI off silently */
        error_free(err);
    }

    memory_region_init(&container, reinterpret_cast<Object *>(this),
                       "intel-hda-container", 0x4000);
    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this), &intel_hda_mmio_ops, this,
                          "intel-hda", 0x2000);
    memory_region_add_subregion(&container, 0x0000, &mmio);
    memory_region_init_alias(&alias, reinterpret_cast<Object *>(this), "intel-hda-alias",
                             &mmio, 0, 0x2000);
    memory_region_add_subregion(&container, 0x2000, &alias);
    pci_register_bar(&pci, 0, 0, &container);

    hda_codec_bus_init(reinterpret_cast<DeviceState *>(this), &codecs, sizeof(codecs),
                       intel_hda_response, intel_hda_xfer);
}

static void intel_hda_exit(PCIDevice *pci)
{
    IntelHDAState *d = reinterpret_cast<IntelHDAState *>(pci);

    msi_uninit(&d->pci);
}

static int intel_hda_post_load(void *opaque, int version)
{
    IntelHDAState* d = static_cast<IntelHDAState *>(opaque);
    int i;

    dprint(d, 1, "%s\n", __func__);
    for (i = 0; i < ARRAY_SIZE(d->st); i++) {
        if (d->st[i].ctl & 0x02) {
            d->parseBdl(&d->st[i]);
        }
    }
    d->updateIrq();
    return 0;
}

static const VMStateField vmstate_intel_hda_stream_fields[] = {
    VMSTATE_UINT32(ctl, IntelHDAStream),
    VMSTATE_UINT32(lpib, IntelHDAStream),
    VMSTATE_UINT32(cbl, IntelHDAStream),
    VMSTATE_UINT32(lvi, IntelHDAStream),
    VMSTATE_UINT32(fmt, IntelHDAStream),
    VMSTATE_UINT32(bdlp_lbase, IntelHDAStream),
    VMSTATE_UINT32(bdlp_ubase, IntelHDAStream),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_intel_hda_stream = {
    .name = "intel-hda-stream",
    .version_id = 1,
    .fields = vmstate_intel_hda_stream_fields,
};

static const VMStateField vmstate_intel_hda_fields[] = {
    VMSTATE_PCI_DEVICE(pci, IntelHDAState),

    /* registers */
    VMSTATE_UINT32(g_ctl, IntelHDAState),
    VMSTATE_UINT32(wake_en, IntelHDAState),
    VMSTATE_UINT32(state_sts, IntelHDAState),
    VMSTATE_UINT32(int_ctl, IntelHDAState),
    VMSTATE_UINT32(int_sts, IntelHDAState),
    VMSTATE_UINT32(wall_clk, IntelHDAState),
    VMSTATE_UINT32(corb_lbase, IntelHDAState),
    VMSTATE_UINT32(corb_ubase, IntelHDAState),
    VMSTATE_UINT32(corb_rp, IntelHDAState),
    VMSTATE_UINT32(corb_wp, IntelHDAState),
    VMSTATE_UINT32(corb_ctl, IntelHDAState),
    VMSTATE_UINT32(corb_sts, IntelHDAState),
    VMSTATE_UINT32(corb_size, IntelHDAState),
    VMSTATE_UINT32(rirb_lbase, IntelHDAState),
    VMSTATE_UINT32(rirb_ubase, IntelHDAState),
    VMSTATE_UINT32(rirb_wp, IntelHDAState),
    VMSTATE_UINT32(rirb_cnt, IntelHDAState),
    VMSTATE_UINT32(rirb_ctl, IntelHDAState),
    VMSTATE_UINT32(rirb_sts, IntelHDAState),
    VMSTATE_UINT32(rirb_size, IntelHDAState),
    VMSTATE_UINT32(dp_lbase, IntelHDAState),
    VMSTATE_UINT32(dp_ubase, IntelHDAState),
    VMSTATE_UINT32(icw, IntelHDAState),
    VMSTATE_UINT32(irr, IntelHDAState),
    VMSTATE_UINT32(ics, IntelHDAState),
    VMSTATE_STRUCT_ARRAY(st, IntelHDAState, 8, 0,
                         vmstate_intel_hda_stream,
                         IntelHDAStream),

    /* additional state info */
    VMSTATE_UINT32(rirb_count, IntelHDAState),
    VMSTATE_INT64(wall_base_ns, IntelHDAState),

    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_intel_hda = {
    .name = "intel-hda",
    .version_id = 1,
    .post_load = intel_hda_post_load,
    .fields = vmstate_intel_hda_fields,
};

static const Property intel_hda_properties[] = {
    DEFINE_PROP_UINT32("debug", IntelHDAState, debug, 0),
    DEFINE_PROP_ON_OFF_AUTO("msi", IntelHDAState, msi, ON_OFF_AUTO_AUTO),
    DEFINE_PROP_BOOL("old_msi_addr", IntelHDAState, old_msi_addr, false),
};

void IntelHDAState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->realize = IntelHDAState::realizeWrapper;
    k->exit = intel_hda_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->class_id = PCI_CLASS_MULTIMEDIA_HD_AUDIO;
    device_class_set_legacy_reset(dc, IntelHDAState::resetWrapper);
    dc->vmsd = &vmstate_intel_hda;
    device_class_set_props(dc, intel_hda_properties);
}

static void intel_hda_class_init_ich6(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->device_id = 0x2668;
    k->revision = 1;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (ich6)";
}

static void intel_hda_class_init_ich9(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->device_id = 0x293e;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (ich9)";
}

static const InterfaceInfo intel_hda_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

/* intel_hda_info registered via REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS_IFACES below */

static const TypeInfo intel_hda_info_ich6 = {
    .name          = "intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_ich6,
};

static const TypeInfo intel_hda_info_ich9 = {
    .name          = "ich9-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_ich9,
};

void IntelHDAState::hdaCodecDeviceClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *k = reinterpret_cast<DeviceClass *>(klass);
    k->realize = hda_codec_dev_realize;
    k->unrealize = hda_codec_dev_unrealize;
    set_bit(DEVICE_CATEGORY_SOUND, k->categories);
    k->bus_type = TYPE_HDA_BUS;
    device_class_set_props(k, hda_props);
}

void HDACodecDevice::classInit(DeviceClass *dc)
{
    IntelHDAState::hdaCodecDeviceClassInit(
        reinterpret_cast<ObjectClass *>(dc), nullptr);
}

/*
 * create intel hda controller with codec attached to it,
 * so '-soundhw hda' works.
 */
static void intel_hda_and_codec_init(const char *audiodev)
{
    g_autoptr(QDict) props = qdict_new();
    DeviceState *intel_hda, *codec;
    BusState *hdabus;

    qdict_put_str(props, "driver", "intel-hda");
    intel_hda = qdev_device_add_from_qdict(props, false, &error_fatal);
    hdabus = QLIST_FIRST(&intel_hda->child_bus);

    codec = qdev_new("hda-duplex");
    qdev_prop_set_string(codec, "audiodev", audiodev);
    qdev_realize_and_unref(codec, hdabus, &error_fatal);
    object_unref(intel_hda);
}

static void intel_hda_register_types(void)
{
    type_register_static(&hda_codec_bus_info);
    type_register_static(&intel_hda_info_ich6);
    type_register_static(&intel_hda_info_ich9);
    audio_register_model_with_cb("hda", "Intel HD Audio", intel_hda_and_codec_init);
}

type_init(intel_hda_register_types)

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_ABSTRACT(HDACodecDevice, HDACodecDeviceClass,
                               TYPE_HDA_CODEC_DEVICE, TYPE_DEVICE)

REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS_IFACES(IntelHDAState,
                                            TYPE_INTEL_HDA_GENERIC,
                                            TYPE_PCI_DEVICE,
                                            intel_hda_interfaces)
