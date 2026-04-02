/*
 * QEMU NE2000 emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include "net/eth.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "ne2000.h"
#include "trace.h"

/* debug NE2000 card */
//#define DEBUG_NE2000

#define MAX_ETH_FRAME_SIZE 1514

#define E8390_CMD       0x00    /* The command register (for all pages) */
/* Page 0 register offsets. */
#define EN0_CLDALO      0x01    /* Low byte of current local dma addr  RD */
#define EN0_STARTPG     0x01    /* Starting page of ring bfr WR */
#define EN0_CLDAHI      0x02    /* High byte of current local dma addr  RD */
#define EN0_STOPPG      0x02    /* Ending page +1 of ring bfr WR */
#define EN0_BOUNDARY    0x03    /* Boundary page of ring bfr RD WR */
#define EN0_TSR         0x04    /* Transmit status reg RD */
#define EN0_TPSR        0x04    /* Transmit starting page WR */
#define EN0_NCR         0x05    /* Number of collision reg RD */
#define EN0_TCNTLO      0x05    /* Low  byte of tx byte count WR */
#define EN0_FIFO        0x06    /* FIFO RD */
#define EN0_TCNTHI      0x06    /* High byte of tx byte count WR */
#define EN0_ISR         0x07    /* Interrupt status reg RD WR */
#define EN0_CRDALO      0x08    /* low byte of current remote dma address RD */
#define EN0_RSARLO      0x08    /* Remote start address reg 0 */
#define EN0_CRDAHI      0x09    /* high byte, current remote dma address RD */
#define EN0_RSARHI      0x09    /* Remote start address reg 1 */
#define EN0_RCNTLO      0x0a    /* Remote byte count reg WR */
#define EN0_RTL8029ID0  0x0a    /* Realtek ID byte #1 RD */
#define EN0_RCNTHI      0x0b    /* Remote byte count reg WR */
#define EN0_RTL8029ID1  0x0b    /* Realtek ID byte #2 RD */
#define EN0_RSR         0x0c    /* rx status reg RD */
#define EN0_RXCR        0x0c    /* RX configuration reg WR */
#define EN0_TXCR        0x0d    /* TX configuration reg WR */
#define EN0_COUNTER0    0x0d    /* Rcv alignment error counter RD */
#define EN0_DCFG        0x0e    /* Data configuration reg WR */
#define EN0_COUNTER1    0x0e    /* Rcv CRC error counter RD */
#define EN0_IMR         0x0f    /* Interrupt mask reg WR */
#define EN0_COUNTER2    0x0f    /* Rcv missed frame error counter RD */

#define EN1_PHYS        0x11
#define EN1_CURPAG      0x17
#define EN1_MULT        0x18

#define EN2_STARTPG     0x21    /* Starting page of ring bfr RD */
#define EN2_STOPPG      0x22    /* Ending page +1 of ring bfr RD */

#define EN3_CONFIG0     0x33
#define EN3_CONFIG1     0x34
#define EN3_CONFIG2     0x35
#define EN3_CONFIG3     0x36

/*  Register accessed at EN_CMD, the 8390 base addr.  */
#define E8390_STOP      0x01    /* Stop and reset the chip */
#define E8390_START     0x02    /* Start the chip, clear reset */
#define E8390_TRANS     0x04    /* Transmit a frame */
#define E8390_RREAD     0x08    /* Remote read */
#define E8390_RWRITE    0x10    /* Remote write  */
#define E8390_NODMA     0x20    /* Remote DMA */
#define E8390_PAGE0     0x00    /* Select page chip registers */
#define E8390_PAGE1     0x40    /* using the two high-order bits */
#define E8390_PAGE2     0x80    /* Page 3 is invalid. */

/* Bits in EN0_ISR - Interrupt status register */
#define ENISR_RX        0x01    /* Receiver, no error */
#define ENISR_TX        0x02    /* Transmitter, no error */
#define ENISR_RX_ERR    0x04    /* Receiver, with error */
#define ENISR_TX_ERR    0x08    /* Transmitter, with error */
#define ENISR_OVER      0x10    /* Receiver overwrote the ring */
#define ENISR_COUNTERS  0x20    /* Counters need emptying */
#define ENISR_RDC       0x40    /* remote dma complete */
#define ENISR_RESET     0x80    /* Reset completed */
#define ENISR_ALL       0x3f    /* Interrupts we will enable */

/* Bits in received packet status byte and EN0_RSR*/
#define ENRSR_RXOK      0x01    /* Received a good packet */
#define ENRSR_CRC       0x02    /* CRC error */
#define ENRSR_FAE       0x04    /* frame alignment error */
#define ENRSR_FO        0x08    /* FIFO overrun */
#define ENRSR_MPA       0x10    /* missed pkt */
#define ENRSR_PHY       0x20    /* physical/multicast address */
#define ENRSR_DIS       0x40    /* receiver disable. set in monitor mode */
#define ENRSR_DEF       0x80    /* deferring */

/* Transmitted packet status, EN0_TSR. */
#define ENTSR_PTX 0x01  /* Packet transmitted without error */
#define ENTSR_ND  0x02  /* The transmit wasn't deferred. */
#define ENTSR_COL 0x04  /* The transmit collided at least once. */
#define ENTSR_ABT 0x08  /* The transmit collided 16 times, and was deferred. */
#define ENTSR_CRS 0x10  /* The carrier sense was lost. */
#define ENTSR_FU  0x20  /* A "FIFO underrun" occurred during transmit. */
#define ENTSR_CDH 0x40  /* The collision detect "heartbeat" signal was lost. */
#define ENTSR_OWC 0x80  /* There was an out-of-window collision. */

void ne2000_reset(NE2000State *s)
{
    int i;

    s->isr = ENISR_RESET;
    memcpy(s->mem, &s->c.macaddr, 6);
    s->mem[14] = 0x57;
    s->mem[15] = 0x57;

    /* duplicate prom data */
    for(i = 15;i >= 0; i--) {
        s->mem[2 * i] = s->mem[i];
        s->mem[2 * i + 1] = s->mem[i];
    }
}

void NE2000State::updateIrq()
{
    int isr;
    isr = (this->isr & this->imr) & 0x7f;
#if defined(DEBUG_NE2000)
    printf("NE2000: Set IRQ to %d (%02x %02x)\n",
           isr ? 1 : 0, this->isr, this->imr);
#endif
    qemu_set_irq(this->irq, (isr != 0));
}

int NE2000State::bufferFull()
{
    int avail, index, boundary;

    if (stop <= start) {
        return 1;
    }

    index = curpag << 8;
    boundary = this->boundary << 8;
    if (index < boundary)
        avail = boundary - index;
    else
        avail = (stop - start) - (index - boundary);
    if (avail < (MAX_ETH_FRAME_SIZE + 4))
        return 1;
    return 0;
}

ssize_t ne2000_receive(NetClientState *nc, const uint8_t *buf, size_t size_)
{
    NE2000State *s = static_cast<NE2000State *>(qemu_get_nic_opaque(nc));
    size_t size = size_;
    uint8_t *p;
    unsigned int total_len, next, avail, len, index, mcast_idx;
    static const uint8_t broadcast_macaddr[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#if defined(DEBUG_NE2000)
    printf("NE2000: received len=%zu\n", size);
#endif

    if (s->cmd & E8390_STOP || s->bufferFull())
        return -1;

    /* XXX: check this */
    if (s->rxcr & 0x10) {
        /* promiscuous: receive all */
    } else {
        if (!memcmp(buf,  broadcast_macaddr, 6)) {
            /* broadcast address */
            if (!(s->rxcr & 0x04))
                return size;
        } else if (buf[0] & 0x01) {
            /* multicast */
            if (!(s->rxcr & 0x08))
                return size;
            mcast_idx = net_crc32(buf, ETH_ALEN) >> 26;
            if (!(s->mult[mcast_idx >> 3] & (1 << (mcast_idx & 7))))
                return size;
        } else if (s->mem[0] == buf[0] &&
                   s->mem[2] == buf[1] &&
                   s->mem[4] == buf[2] &&
                   s->mem[6] == buf[3] &&
                   s->mem[8] == buf[4] &&
                   s->mem[10] == buf[5]) {
            /* match */
        } else {
            return size;
        }
    }

    index = s->curpag << 8;
    if (index >= NE2000_PMEM_END) {
        index = s->start;
    }
    /* 4 bytes for header */
    total_len = size + 4;
    /* address for next packet (4 bytes for CRC) */
    next = index + ((total_len + 4 + 255) & ~0xff);
    if (next >= s->stop)
        next -= (s->stop - s->start);
    /* prepare packet header */
    p = s->mem + index;
    s->rsr = ENRSR_RXOK; /* receive status */
    /* XXX: check this */
    if (buf[0] & 0x01)
        s->rsr |= ENRSR_PHY;
    p[0] = s->rsr;
    p[1] = next >> 8;
    p[2] = total_len;
    p[3] = total_len >> 8;
    index += 4;

    /* write packet data */
    while (size > 0) {
        if (index <= s->stop)
            avail = s->stop - index;
        else
            break;
        len = size;
        if (len > avail)
            len = avail;
        memcpy(s->mem + index, buf, len);
        buf += len;
        index += len;
        if (index == s->stop)
            index = s->start;
        size -= len;
    }
    s->curpag = next >> 8;

    /* now we can signal we have received something */
    s->isr |= ENISR_RX;
    s->updateIrq();

    return size_;
}

void NE2000State::ioportWrite(uint32_t addr, uint32_t val)
{
    int offset, page, index;

    addr &= 0xf;
    trace_ne2000_ioport_write(addr, val);
    if (addr == E8390_CMD) {
        /* control register */
        cmd = val;
        if (!(val & E8390_STOP)) { /* START bit makes no sense on RTL8029... */
            isr &= ~ENISR_RESET;
            /* test specific case: zero length transfer */
            if ((val & (E8390_RREAD | E8390_RWRITE)) &&
                rcnt == 0) {
                isr |= ENISR_RDC;
                updateIrq();
            }
            if (val & E8390_TRANS) {
                index = (tpsr << 8);
                /* XXX: next 2 lines are a hack to make netware 3.11 work */
                if (index >= NE2000_PMEM_END)
                    index -= NE2000_PMEM_SIZE;
                /* fail safe: check range on the transmitted length  */
                if (index + tcnt <= NE2000_PMEM_END) {
                    qemu_send_packet(qemu_get_queue(nic), mem + index,
                                     tcnt);
                }
                /* signal end of transfer */
                tsr = ENTSR_PTX;
                isr |= ENISR_TX;
                cmd &= ~E8390_TRANS;
                updateIrq();
            }
        }
    } else {
        page = cmd >> 6;
        offset = addr | (page << 4);
        switch(offset) {
        case EN0_STARTPG:
            if (val << 8 <= NE2000_PMEM_END) {
                start = val << 8;
            }
            break;
        case EN0_STOPPG:
            if (val << 8 <= NE2000_PMEM_END) {
                stop = val << 8;
            }
            break;
        case EN0_BOUNDARY:
            if (val << 8 < NE2000_PMEM_END) {
                boundary = val;
            }
            break;
        case EN0_IMR:
            imr = val;
            updateIrq();
            break;
        case EN0_TPSR:
            tpsr = val;
            break;
        case EN0_TCNTLO:
            tcnt = (tcnt & 0xff00) | val;
            break;
        case EN0_TCNTHI:
            tcnt = (tcnt & 0x00ff) | (val << 8);
            break;
        case EN0_RSARLO:
            rsar = (rsar & 0xff00) | val;
            break;
        case EN0_RSARHI:
            rsar = (rsar & 0x00ff) | (val << 8);
            break;
        case EN0_RCNTLO:
            rcnt = (rcnt & 0xff00) | val;
            break;
        case EN0_RCNTHI:
            rcnt = (rcnt & 0x00ff) | (val << 8);
            break;
        case EN0_RXCR:
            rxcr = val;
            break;
        case EN0_DCFG:
            dcfg = val;
            break;
        case EN0_ISR:
            isr &= ~(val & 0x7f);
            updateIrq();
            break;
        case EN1_PHYS ... EN1_PHYS + 5:
            phys[offset - EN1_PHYS] = val;
            break;
        case EN1_CURPAG:
            if (val << 8 < NE2000_PMEM_END) {
                curpag = val;
            }
            break;
        case EN1_MULT ... EN1_MULT + 7:
            mult[offset - EN1_MULT] = val;
            break;
        }
    }
}

uint32_t NE2000State::ioportRead(uint32_t addr)
{
    int offset, page, ret;

    addr &= 0xf;
    if (addr == E8390_CMD) {
        ret = cmd;
    } else {
        page = cmd >> 6;
        offset = addr | (page << 4);
        switch(offset) {
        case EN0_TSR:
            ret = tsr;
            break;
        case EN0_BOUNDARY:
            ret = boundary;
            break;
        case EN0_ISR:
            ret = isr;
            break;
        case EN0_RSARLO:
            ret = rsar & 0x00ff;
            break;
        case EN0_RSARHI:
            ret = rsar >> 8;
            break;
        case EN1_PHYS ... EN1_PHYS + 5:
            ret = phys[offset - EN1_PHYS];
            break;
        case EN1_CURPAG:
            ret = curpag;
            break;
        case EN1_MULT ... EN1_MULT + 7:
            ret = mult[offset - EN1_MULT];
            break;
        case EN0_RSR:
            ret = rsr;
            break;
        case EN2_STARTPG:
            ret = start >> 8;
            break;
        case EN2_STOPPG:
            ret = stop >> 8;
            break;
        case EN0_RTL8029ID0:
            ret = 0x50;
            break;
        case EN0_RTL8029ID1:
            ret = 0x43;
            break;
        case EN3_CONFIG0:
            ret = 0;          /* 10baseT media */
            break;
        case EN3_CONFIG2:
            ret = 0x40;       /* 10baseT active */
            break;
        case EN3_CONFIG3:
            ret = 0x40;       /* Full duplex */
            break;
        default:
            ret = 0x00;
            break;
        }
    }
    trace_ne2000_ioport_read(addr, ret);
    return ret;
}

void NE2000State::memWriteb(uint32_t addr, uint32_t val)
{
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE)) {
        mem[addr] = val;
    }
}

void NE2000State::memWritew(uint32_t addr, uint32_t val)
{
    addr &= ~1; /* XXX: check exact behaviour if not even */
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE)) {
        *(uint16_t *)(mem + addr) = cpu_to_le16(val);
    }
}

void NE2000State::memWritel(uint32_t addr, uint32_t val)
{
    addr &= ~1; /* XXX: check exact behaviour if not even */
    if (addr < 32
        || (addr >= NE2000_PMEM_START
            && addr + sizeof(uint32_t) <= NE2000_MEM_SIZE)) {
        stl_le_p(mem + addr, val);
    }
}

uint32_t NE2000State::memReadb(uint32_t addr)
{
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE)) {
        return mem[addr];
    } else {
        return 0xff;
    }
}

uint32_t NE2000State::memReadw(uint32_t addr)
{
    addr &= ~1; /* XXX: check exact behaviour if not even */
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE)) {
        return le16_to_cpu(*(uint16_t *)(mem + addr));
    } else {
        return 0xffff;
    }
}

uint32_t NE2000State::memReadl(uint32_t addr)
{
    addr &= ~1; /* XXX: check exact behaviour if not even */
    if (addr < 32
        || (addr >= NE2000_PMEM_START
            && addr + sizeof(uint32_t) <= NE2000_MEM_SIZE)) {
        return ldl_le_p(mem + addr);
    } else {
        return 0xffffffff;
    }
}

void NE2000State::dmaUpdate(int len)
{
    rsar += len;
    /* wrap */
    /* XXX: check what to do if rsar > stop */
    if (rsar == stop)
        rsar = start;

    if (rcnt <= len) {
        rcnt = 0;
        /* signal end of transfer */
        isr |= ENISR_RDC;
        updateIrq();
    } else {
        rcnt -= len;
    }
}

void NE2000State::asicIoportWrite(uint32_t addr, uint32_t val)
{
#ifdef DEBUG_NE2000
    printf("NE2000: asic write val=0x%04x\n", val);
#endif
    if (rcnt == 0)
        return;
    if (dcfg & 0x01) {
        /* 16 bit access */
        memWritew(rsar, val);
        dmaUpdate(2);
    } else {
        /* 8 bit access */
        memWriteb(rsar, val);
        dmaUpdate(1);
    }
}

uint32_t NE2000State::asicIoportRead(uint32_t addr)
{
    int ret;

    if (dcfg & 0x01) {
        /* 16 bit access */
        ret = memReadw(rsar);
        dmaUpdate(2);
    } else {
        /* 8 bit access */
        ret = memReadb(rsar);
        dmaUpdate(1);
    }
#ifdef DEBUG_NE2000
    printf("NE2000: asic read val=0x%04x\n", ret);
#endif
    return ret;
}

void NE2000State::asicIoportWritel(uint32_t addr, uint32_t val)
{
#ifdef DEBUG_NE2000
    printf("NE2000: asic writel val=0x%04x\n", val);
#endif
    if (rcnt == 0)
        return;
    /* 32 bit access */
    memWritel(rsar, val);
    dmaUpdate(4);
}

uint32_t NE2000State::asicIoportReadl(uint32_t addr)
{
    int ret;

    /* 32 bit access */
    ret = memReadl(rsar);
    dmaUpdate(4);
#ifdef DEBUG_NE2000
    printf("NE2000: asic readl val=0x%04x\n", ret);
#endif
    return ret;
}

static void ne2000_reset_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    /* nothing to do (end of reset pulse) */
}

uint32_t NE2000State::resetIoportRead(uint32_t addr)
{
    ne2000_reset(this);
    return 0;
}

static int ne2000_post_load(void* opaque, int version_id)
{
    NE2000State *s = static_cast<NE2000State *>(opaque);

    if (version_id < 2) {
        s->rxcr = 0x0c;
    }
    return 0;
}

static const VMStateField vmstate_ne2000_fields[] = {
    VMSTATE_UINT8_V(rxcr, NE2000State, 2),
    VMSTATE_UINT8(cmd, NE2000State),
    VMSTATE_UINT32(start, NE2000State),
    VMSTATE_UINT32(stop, NE2000State),
    VMSTATE_UINT8(boundary, NE2000State),
    VMSTATE_UINT8(tsr, NE2000State),
    VMSTATE_UINT8(tpsr, NE2000State),
    VMSTATE_UINT16(tcnt, NE2000State),
    VMSTATE_UINT16(rcnt, NE2000State),
    VMSTATE_UINT32(rsar, NE2000State),
    VMSTATE_UINT8(rsr, NE2000State),
    VMSTATE_UINT8(isr, NE2000State),
    VMSTATE_UINT8(dcfg, NE2000State),
    VMSTATE_UINT8(imr, NE2000State),
    VMSTATE_BUFFER(phys, NE2000State),
    VMSTATE_UINT8(curpag, NE2000State),
    VMSTATE_BUFFER(mult, NE2000State),
    VMSTATE_UNUSED(4), /* was irq */
    VMSTATE_BUFFER(mem, NE2000State),
    VMSTATE_END_OF_LIST()
};

const VMStateDescription vmstate_ne2000 = {
    .name = "ne2000",
    .version_id = 2,
    .minimum_version_id = 0,
    .post_load = ne2000_post_load,
    .fields = vmstate_ne2000_fields,
};

static uint64_t ne2000_read(void *opaque, hwaddr addr,
                            unsigned size)
{
    NE2000State *s = static_cast<NE2000State *>(opaque);
    uint64_t val;

    if (addr < 0x10 && size == 1) {
        val = s->ioportRead(addr);
    } else if (addr == 0x10) {
        if (size <= 2) {
            val = s->asicIoportRead(addr);
        } else {
            val = s->asicIoportReadl(addr);
        }
    } else if (addr == 0x1f && size == 1) {
        val = s->resetIoportRead(addr);
    } else {
        val = ((uint64_t)1 << (size * 8)) - 1;
    }
    trace_ne2000_read(addr, val);

    return val;
}

static void ne2000_write(void *opaque, hwaddr addr,
                         uint64_t data, unsigned size)
{
    NE2000State *s = static_cast<NE2000State *>(opaque);

    trace_ne2000_write(addr, data);
    if (addr < 0x10 && size == 1) {
        s->ioportWrite(addr, data);
    } else if (addr == 0x10) {
        if (size <= 2) {
            s->asicIoportWrite(addr, data);
        } else {
            s->asicIoportWritel(addr, data);
        }
    } else if (addr == 0x1f && size == 1) {
        ne2000_reset_ioport_write(s, addr, data);
    }
}

static const MemoryRegionOps ne2000_ops = {
    .read = ne2000_read,
    .write = ne2000_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/***********************************************************/
/* PCI NE2000 definitions */

void ne2000_setup_io(NE2000State *s, DeviceState *dev, unsigned size)
{
    memory_region_init_io(&s->io, OBJECT(dev), &ne2000_ops, s, "ne2000", size);
}
