/*
 * QEMU GE IP-Octal 232 IndustryPack emulation
 *
 * Copyright (C) 2012 Igalia, S.L.
 * Author: Alberto Garcia <berto@igalia.com>
 *
 * This code is licensed under the GNU GPL v2 or (at your option) any
 * later version.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/ipack/ipack.h"
#include "qom/cpp/object.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

/* #define DEBUG_IPOCTAL */

#ifdef DEBUG_IPOCTAL
#define DPRINTF2(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF2(fmt, ...) do { } while (0)
#endif

#define DPRINTF(fmt, ...) DPRINTF2("IP-Octal: " fmt, ## __VA_ARGS__)

#define RX_FIFO_SIZE 3

/* The IP-Octal has 8 channels (a-h)
   divided into 4 blocks (A-D) */
#define N_CHANNELS 8
#define N_BLOCKS   4

#define REG_MRa  0x01
#define REG_MRb  0x11
#define REG_SRa  0x03
#define REG_SRb  0x13
#define REG_CSRa 0x03
#define REG_CSRb 0x13
#define REG_CRa  0x05
#define REG_CRb  0x15
#define REG_RHRa 0x07
#define REG_RHRb 0x17
#define REG_THRa 0x07
#define REG_THRb 0x17
#define REG_ACR  0x09
#define REG_ISR  0x0B
#define REG_IMR  0x0B
#define REG_OPCR 0x1B

#define CR_ENABLE_RX    BIT(0)
#define CR_DISABLE_RX   BIT(1)
#define CR_ENABLE_TX    BIT(2)
#define CR_DISABLE_TX   BIT(3)
#define CR_CMD(cr)      ((cr) >> 4)
#define CR_NO_OP        0
#define CR_RESET_MR     1
#define CR_RESET_RX     2
#define CR_RESET_TX     3
#define CR_RESET_ERR    4
#define CR_RESET_BRKINT 5
#define CR_START_BRK    6
#define CR_STOP_BRK     7
#define CR_ASSERT_RTSN  8
#define CR_NEGATE_RTSN  9
#define CR_TIMEOUT_ON   10
#define CR_TIMEOUT_OFF  12

#define SR_RXRDY   BIT(0)
#define SR_FFULL   BIT(1)
#define SR_TXRDY   BIT(2)
#define SR_TXEMT   BIT(3)
#define SR_OVERRUN BIT(4)
#define SR_PARITY  BIT(5)
#define SR_FRAMING BIT(6)
#define SR_BREAK   BIT(7)

#define ISR_TXRDYA BIT(0)
#define ISR_RXRDYA BIT(1)
#define ISR_BREAKA BIT(2)
#define ISR_CNTRDY BIT(3)
#define ISR_TXRDYB BIT(4)
#define ISR_RXRDYB BIT(5)
#define ISR_BREAKB BIT(6)
#define ISR_MPICHG BIT(7)
#define ISR_TXRDY(CH) (((CH) & 1) ? BIT(4) : BIT(0))
#define ISR_RXRDY(CH) (((CH) & 1) ? BIT(5) : BIT(1))
#define ISR_BREAK(CH) (((CH) & 1) ? BIT(6) : BIT(2))

#define TYPE_IPOCTAL "ipoctal232"

OBJECT_DECLARE_SIMPLE_TYPE(IPOctalState, IPOCTAL)

typedef struct SCC2698Channel SCC2698Channel;
typedef struct SCC2698Block SCC2698Block;

struct SCC2698Channel {
    IPOctalState *ipoctal;
    CharFrontend dev;
    bool rx_enabled;
    uint8_t mr[2];
    uint8_t mr_idx;
    uint8_t sr;
    uint8_t rhr[RX_FIFO_SIZE];
    uint8_t rhr_idx;
    uint8_t rx_pending;
};

struct SCC2698Block {
    uint8_t imr;
    uint8_t isr;
};

struct IPOctalState {
    IPackDevice parent_obj;

    SCC2698Channel ch[N_CHANNELS];
    SCC2698Block blk[N_BLOCKS];
    uint8_t irq_vector;

    /* methods */
    void updateIrq(unsigned block);
    void writeCr(unsigned channel, uint8_t val);
    void realize(Error **errp);

    static uint16_t ioRead(IPackDevice *ip, uint8_t addr);
    static void ioWrite(IPackDevice *ip, uint8_t addr, uint16_t val);
    static uint16_t idRead(IPackDevice *ip, uint8_t addr);
    static void idWrite(IPackDevice *ip, uint8_t addr, uint16_t val);
    static uint16_t intRead(IPackDevice *ip, uint8_t addr);
    static void intWrite(IPackDevice *ip, uint8_t addr, uint16_t val);
    static uint16_t memRead16(IPackDevice *ip, uint32_t addr);
    static void memWrite16(IPackDevice *ip, uint32_t addr, uint16_t val);
    static uint8_t memRead8(IPackDevice *ip, uint32_t addr);
    static void memWrite8(IPackDevice *ip, uint32_t addr, uint8_t val);

    static int hostdevCanReceive(void *opaque);
    static void hostdevReceive(void *opaque, const uint8_t *buf, int size);
    static void hostdevEvent(void *opaque, QEMUChrEvent event);

    static void realizeWrapper(DeviceState *dev, Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
};

static const VMStateField vmstate_scc2698_channel_fields[] = {
    VMSTATE_BOOL(rx_enabled, SCC2698Channel),
    VMSTATE_UINT8_ARRAY(mr, SCC2698Channel, 2),
    VMSTATE_UINT8(mr_idx, SCC2698Channel),
    VMSTATE_UINT8(sr, SCC2698Channel),
    VMSTATE_UINT8_ARRAY(rhr, SCC2698Channel, RX_FIFO_SIZE),
    VMSTATE_UINT8(rhr_idx, SCC2698Channel),
    VMSTATE_UINT8(rx_pending, SCC2698Channel),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_scc2698_channel = {
    .name = "scc2698_channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_scc2698_channel_fields,
};

static const VMStateField vmstate_scc2698_block_fields[] = {
    VMSTATE_UINT8(imr, SCC2698Block),
    VMSTATE_UINT8(isr, SCC2698Block),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_scc2698_block = {
    .name = "scc2698_block",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_scc2698_block_fields,
};

static const VMStateField vmstate_ipoctal_fields[] = {
    VMSTATE_IPACK_DEVICE(parent_obj, IPOctalState),
    VMSTATE_STRUCT_ARRAY(ch, IPOctalState, N_CHANNELS, 1,
                         vmstate_scc2698_channel, SCC2698Channel),
    VMSTATE_STRUCT_ARRAY(blk, IPOctalState, N_BLOCKS, 1,
                         vmstate_scc2698_block, SCC2698Block),
    VMSTATE_UINT8(irq_vector, IPOctalState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_ipoctal = {
    .name = "ipoctal232",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_ipoctal_fields,
};

/* data[10] is 0x0C, not 0x0B as the doc says */
static const uint8_t id_prom_data[] = {
    0x49, 0x50, 0x41, 0x43, 0xF0, 0x22,
    0xA1, 0x00, 0x00, 0x00, 0x0C, 0xCC
};

void IPOctalState::updateIrq(unsigned block)
{
    IPackDevice *idev = reinterpret_cast<IPackDevice *>(this);
    /* Blocks A and B interrupt on INT0#, C and D on INT1#.
       Thus, to get the status we have to check two blocks. */
    SCC2698Block *blk0 = &blk[block];
    SCC2698Block *blk1 = &blk[block^1];
    unsigned intno = block / 2;

    if ((blk0->isr & blk0->imr) || (blk1->isr & blk1->imr)) {
        qemu_irq_raise(&idev->irq[intno]);
    } else {
        qemu_irq_lower(&idev->irq[intno]);
    }
}

void IPOctalState::writeCr(unsigned channel, uint8_t val)
{
    SCC2698Channel *ch_ptr = &ch[channel];
    SCC2698Block *blk_ptr = &blk[channel / 2];

    DPRINTF("Write CR%c %u: ", channel + 'a', val);

    /* The lower 4 bits are used to enable and disable Tx and Rx */
    if (val & CR_ENABLE_RX) {
        DPRINTF2("Rx on, ");
        ch_ptr->rx_enabled = true;
    }
    if (val & CR_DISABLE_RX) {
        DPRINTF2("Rx off, ");
        ch_ptr->rx_enabled = false;
    }
    if (val & CR_ENABLE_TX) {
        DPRINTF2("Tx on, ");
        ch_ptr->sr |= SR_TXRDY | SR_TXEMT;
        blk_ptr->isr |= ISR_TXRDY(channel);
    }
    if (val & CR_DISABLE_TX) {
        DPRINTF2("Tx off, ");
        ch_ptr->sr &= ~(SR_TXRDY | SR_TXEMT);
        blk_ptr->isr &= ~ISR_TXRDY(channel);
    }

    DPRINTF2("cmd: ");

    /* The rest of the bits implement different commands */
    switch (CR_CMD(val)) {
    case CR_NO_OP:
        DPRINTF2("none");
        break;
    case CR_RESET_MR:
        DPRINTF2("reset MR");
        ch_ptr->mr_idx = 0;
        break;
    case CR_RESET_RX:
        DPRINTF2("reset Rx");
        ch_ptr->rx_enabled = false;
        ch_ptr->rx_pending = 0;
        ch_ptr->sr &= ~SR_RXRDY;
        blk_ptr->isr &= ~ISR_RXRDY(channel);
        break;
    case CR_RESET_TX:
        DPRINTF2("reset Tx");
        ch_ptr->sr &= ~(SR_TXRDY | SR_TXEMT);
        blk_ptr->isr &= ~ISR_TXRDY(channel);
        break;
    case CR_RESET_ERR:
        DPRINTF2("reset err");
        ch_ptr->sr &= ~(SR_OVERRUN | SR_PARITY | SR_FRAMING | SR_BREAK);
        break;
    case CR_RESET_BRKINT:
        DPRINTF2("reset brk ch int");
        blk_ptr->isr &= ~(ISR_BREAKA | ISR_BREAKB);
        break;
    default:
        DPRINTF2("unsupported 0x%x", CR_CMD(val));
    }

    DPRINTF2("\n");
}

uint16_t IPOctalState::ioRead(IPackDevice *ip, uint8_t addr)
{
    IPOctalState *dev = reinterpret_cast<IPOctalState *>(ip);
    uint16_t ret = 0;
    /* addr[7:6]: block   (A-D)
       addr[7:5]: channel (a-h)
       addr[5:0]: register */
    unsigned block = addr >> 5;
    unsigned channel = addr >> 4;
    /* Big endian, accessed using 8-bit bytes at odd locations */
    unsigned offset = (addr & 0x1F) ^ 1;
    SCC2698Channel *ch = &dev->ch[channel];
    SCC2698Block *blk = &dev->blk[block];
    uint8_t old_isr = blk->isr;

    switch (offset) {

    case REG_MRa:
    case REG_MRb:
        ret = ch->mr[ch->mr_idx];
        DPRINTF("Read MR%u%c: 0x%x\n", ch->mr_idx + 1, channel + 'a', ret);
        ch->mr_idx = 1;
        break;

    case REG_SRa:
    case REG_SRb:
        ret = ch->sr;
        DPRINTF("Read SR%c: 0x%x\n", channel + 'a', ret);
        break;

    case REG_RHRa:
    case REG_RHRb:
        ret = ch->rhr[ch->rhr_idx];
        if (ch->rx_pending > 0) {
            ch->rx_pending--;
            if (ch->rx_pending == 0) {
                ch->sr &= ~SR_RXRDY;
                blk->isr &= ~ISR_RXRDY(channel);
                qemu_chr_fe_accept_input(&ch->dev);
            } else {
                ch->rhr_idx = (ch->rhr_idx + 1) % RX_FIFO_SIZE;
            }
            if (ch->sr & SR_BREAK) {
                ch->sr &= ~SR_BREAK;
                blk->isr |= ISR_BREAK(channel);
            }
        }
        DPRINTF("Read RHR%c (0x%x)\n", channel + 'a', ret);
        break;

    case REG_ISR:
        ret = blk->isr;
        DPRINTF("Read ISR%c: 0x%x\n", block + 'A', ret);
        break;

    default:
        DPRINTF("Read unknown/unsupported register 0x%02x\n", offset);
    }

    if (old_isr != blk->isr) {
        dev->updateIrq(block);
    }

    return ret;
}

void IPOctalState::ioWrite(IPackDevice *ip, uint8_t addr, uint16_t val)
{
    IPOctalState *dev = reinterpret_cast<IPOctalState *>(ip);
    unsigned reg = val & 0xFF;
    /* addr[7:6]: block   (A-D)
       addr[7:5]: channel (a-h)
       addr[5:0]: register */
    unsigned block = addr >> 5;
    unsigned channel = addr >> 4;
    /* Big endian, accessed using 8-bit bytes at odd locations */
    unsigned offset = (addr & 0x1F) ^ 1;
    SCC2698Channel *ch = &dev->ch[channel];
    SCC2698Block *blk = &dev->blk[block];
    uint8_t old_isr = blk->isr;
    uint8_t old_imr = blk->imr;

    switch (offset) {

    case REG_MRa:
    case REG_MRb:
        ch->mr[ch->mr_idx] = reg;
        DPRINTF("Write MR%u%c 0x%x\n", ch->mr_idx + 1, channel + 'a', reg);
        ch->mr_idx = 1;
        break;

    /* Not implemented */
    case REG_CSRa:
    case REG_CSRb:
        DPRINTF("Write CSR%c: 0x%x\n", channel + 'a', reg);
        break;

    case REG_CRa:
    case REG_CRb:
        dev->writeCr(channel, reg);
        break;

    case REG_THRa:
    case REG_THRb:
        if (ch->sr & SR_TXRDY) {
            uint8_t thr = reg;
            DPRINTF("Write THR%c (0x%x)\n", channel + 'a', reg);
            /* XXX this blocks entire thread. Rewrite to use
             * qemu_chr_fe_write and background I/O callbacks */
            qemu_chr_fe_write_all(&ch->dev, &thr, 1);
        } else {
            DPRINTF("Write THR%c (0x%x), Tx disabled\n", channel + 'a', reg);
        }
        break;

    /* Not implemented */
    case REG_ACR:
        DPRINTF("Write ACR%c 0x%x\n", block + 'A', val);
        break;

    case REG_IMR:
        DPRINTF("Write IMR%c 0x%x\n", block + 'A', val);
        blk->imr = reg;
        break;

    /* Not implemented */
    case REG_OPCR:
        DPRINTF("Write OPCR%c 0x%x\n", block + 'A', val);
        break;

    default:
        DPRINTF("Write unknown/unsupported register 0x%02x %u\n", offset, val);
    }

    if (old_isr != blk->isr || old_imr != blk->imr) {
        dev->updateIrq(block);
    }
}

uint16_t IPOctalState::idRead(IPackDevice *ip, uint8_t addr)
{
    uint16_t ret = 0;
    unsigned pos = addr / 2; /* The ID PROM data is stored every other byte */

    if (pos < ARRAY_SIZE(id_prom_data)) {
        ret = id_prom_data[pos];
    } else {
        DPRINTF("Attempt to read unavailable PROM data at 0x%x\n",  addr);
    }

    return ret;
}

void IPOctalState::idWrite(IPackDevice *ip, uint8_t addr, uint16_t val)
{
    IPOctalState *dev = reinterpret_cast<IPOctalState *>(ip);
    if (addr == 1) {
        DPRINTF("Write IRQ vector: %u\n", (unsigned) val);
        dev->irq_vector = val; /* Undocumented, but the hw works like that */
    } else {
        DPRINTF("Attempt to write 0x%x to 0x%x\n", val, addr);
    }
}

uint16_t IPOctalState::intRead(IPackDevice *ip, uint8_t addr)
{
    IPOctalState *dev = reinterpret_cast<IPOctalState *>(ip);
    /* Read address 0 to ACK INT0# and address 2 to ACK INT1# */
    if (addr != 0 && addr != 2) {
        DPRINTF("Attempt to read from 0x%x\n", addr);
        return 0;
    } else {
        /* Update interrupts if necessary */
        dev->updateIrq(addr);
        return dev->irq_vector;
    }
}

void IPOctalState::intWrite(IPackDevice *ip, uint8_t addr, uint16_t val)
{
    DPRINTF("Attempt to write 0x%x to 0x%x\n", val, addr);
}

uint16_t IPOctalState::memRead16(IPackDevice *ip, uint32_t addr)
{
    DPRINTF("Attempt to read from 0x%x\n", addr);
    return 0;
}

void IPOctalState::memWrite16(IPackDevice *ip, uint32_t addr, uint16_t val)
{
    DPRINTF("Attempt to write 0x%x to 0x%x\n", val, addr);
}

uint8_t IPOctalState::memRead8(IPackDevice *ip, uint32_t addr)
{
    DPRINTF("Attempt to read from 0x%x\n", addr);
    return 0;
}

void IPOctalState::memWrite8(IPackDevice *ip, uint32_t addr, uint8_t val)
{
    IPOctalState *dev = reinterpret_cast<IPOctalState *>(ip);
    if (addr == 1) {
        DPRINTF("Write IRQ vector: %u\n", (unsigned) val);
        dev->irq_vector = val;
    } else {
        DPRINTF("Attempt to write 0x%x to 0x%x\n", val, addr);
    }
}

int IPOctalState::hostdevCanReceive(void *opaque)
{
    SCC2698Channel *ch = static_cast<SCC2698Channel *>(opaque);
    int available_bytes = RX_FIFO_SIZE - ch->rx_pending;
    return ch->rx_enabled ? available_bytes : 0;
}

void IPOctalState::hostdevReceive(void *opaque, const uint8_t *buf, int size)
{
    SCC2698Channel *ch = static_cast<SCC2698Channel *>(opaque);
    IPOctalState *dev = ch->ipoctal;
    unsigned pos = ch->rhr_idx + ch->rx_pending;
    int i;

    assert(size + ch->rx_pending <= RX_FIFO_SIZE);

    /* Copy data to the RxFIFO */
    for (i = 0; i < size; i++) {
        pos %= RX_FIFO_SIZE;
        ch->rhr[pos++] = buf[i];
    }

    ch->rx_pending += size;

    /* If the RxFIFO was empty raise an interrupt */
    if (!(ch->sr & SR_RXRDY)) {
        unsigned block, channel = 0;
        /* Find channel number to update the ISR register */
        while (&dev->ch[channel] != ch) {
            channel++;
        }
        block = channel / 2;
        dev->blk[block].isr |= ISR_RXRDY(channel);
        ch->sr |= SR_RXRDY;
        dev->updateIrq(block);
    }
}

void IPOctalState::hostdevEvent(void *opaque, QEMUChrEvent event)
{
    SCC2698Channel *ch = static_cast<SCC2698Channel *>(opaque);
    switch (event) {
    case CHR_EVENT_OPENED:
        DPRINTF("Device %s opened\n", ch->dev->label);
        break;
    case CHR_EVENT_BREAK: {
        uint8_t zero = 0;
        DPRINTF("Device %s received break\n", ch->dev->label);

        if (!(ch->sr & SR_BREAK)) {
            IPOctalState *dev = ch->ipoctal;
            unsigned block, channel = 0;

            while (&dev->ch[channel] != ch) {
                channel++;
            }
            block = channel / 2;

            ch->sr |= SR_BREAK;
            dev->blk[block].isr |= ISR_BREAK(channel);
        }

        /* Put a zero character in the buffer */
        hostdevReceive(ch, &zero, 1);
    }
        break;
    default:
        DPRINTF("Device %s received event %d\n", ch->dev->label, event);
    }
}

void IPOctalState::realizeWrapper(DeviceState *dev, Error **errp)
{
    reinterpret_cast<IPOctalState *>(dev)->realize(errp);
}

void IPOctalState::realize(Error **errp)
{
    unsigned i;

    for (i = 0; i < N_CHANNELS; i++) {
        SCC2698Channel *ch_ptr = &this->ch[i];
        ch_ptr->ipoctal = this;

        /* Redirect IP-Octal channels to host character devices */
        if (qemu_chr_fe_backend_connected(&ch_ptr->dev)) {
            qemu_chr_fe_set_handlers(&ch_ptr->dev, hostdevCanReceive,
                                     hostdevReceive, hostdevEvent,
                                     NULL, ch_ptr, NULL, true);
            DPRINTF("Redirecting channel %u to %s\n", i, ch_ptr->dev->label);
        } else {
            DPRINTF("Could not redirect channel %u, no chardev set\n", i);
        }
    }
}

static const Property ipoctal_properties[] = {
    DEFINE_PROP_CHR("chardev0", IPOctalState, ch[0].dev),
    DEFINE_PROP_CHR("chardev1", IPOctalState, ch[1].dev),
    DEFINE_PROP_CHR("chardev2", IPOctalState, ch[2].dev),
    DEFINE_PROP_CHR("chardev3", IPOctalState, ch[3].dev),
    DEFINE_PROP_CHR("chardev4", IPOctalState, ch[4].dev),
    DEFINE_PROP_CHR("chardev5", IPOctalState, ch[5].dev),
    DEFINE_PROP_CHR("chardev6", IPOctalState, ch[6].dev),
    DEFINE_PROP_CHR("chardev7", IPOctalState, ch[7].dev),
};

struct IPOctalClass : IPackDeviceClass {
    void realize(DeviceState *dev, Error **errp) override;
    uint16_t io_read(IPackDevice *dev, uint8_t addr) override;
    void io_write(IPackDevice *dev, uint8_t addr, uint16_t val) override;
    uint16_t id_read(IPackDevice *dev, uint8_t addr) override;
    void id_write(IPackDevice *dev, uint8_t addr, uint16_t val) override;
    uint16_t int_read(IPackDevice *dev, uint8_t addr) override;
    void int_write(IPackDevice *dev, uint8_t addr, uint16_t val) override;
    uint16_t mem_read16(IPackDevice *dev, uint32_t addr) override;
    void mem_write16(IPackDevice *dev, uint32_t addr, uint16_t val) override;
    uint8_t mem_read8(IPackDevice *dev, uint32_t addr) override;
    void mem_write8(IPackDevice *dev, uint32_t addr, uint8_t val) override;
};

void IPOctalClass::realize(DeviceState *dev, Error **errp) { IPOctalState::realizeWrapper(dev, errp); }
uint16_t IPOctalClass::io_read(IPackDevice *dev, uint8_t addr) { return IPOctalState::ioRead(dev, addr); }
void IPOctalClass::io_write(IPackDevice *dev, uint8_t addr, uint16_t val) { IPOctalState::ioWrite(dev, addr, val); }
uint16_t IPOctalClass::id_read(IPackDevice *dev, uint8_t addr) { return IPOctalState::idRead(dev, addr); }
void IPOctalClass::id_write(IPackDevice *dev, uint8_t addr, uint16_t val) { IPOctalState::idWrite(dev, addr, val); }
uint16_t IPOctalClass::int_read(IPackDevice *dev, uint8_t addr) { return IPOctalState::intRead(dev, addr); }
void IPOctalClass::int_write(IPackDevice *dev, uint8_t addr, uint16_t val) { IPOctalState::intWrite(dev, addr, val); }
uint16_t IPOctalClass::mem_read16(IPackDevice *dev, uint32_t addr) { return IPOctalState::memRead16(dev, addr); }
void IPOctalClass::mem_write16(IPackDevice *dev, uint32_t addr, uint16_t val) { IPOctalState::memWrite16(dev, addr, val); }
uint8_t IPOctalClass::mem_read8(IPackDevice *dev, uint32_t addr) { return IPOctalState::memRead8(dev, addr); }
void IPOctalClass::mem_write8(IPackDevice *dev, uint32_t addr, uint8_t val) { IPOctalState::memWrite8(dev, addr, val); }

void IPOctalState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    qom_fixup_vtable<IPOctalClass>(klass);

    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc    = "GE IP-Octal 232 8-channel RS-232 IndustryPack";
    device_class_set_props(dc, ipoctal_properties);
    dc->vmsd    = &vmstate_ipoctal;
}

static const TypeInfo ipoctal_info = {
    .name          = TYPE_IPOCTAL,
    .parent        = TYPE_IPACK_DEVICE,
    .instance_size = sizeof(IPOctalState),
    .class_size    = sizeof(IPOctalClass),
    .class_init    = IPOctalState::classInit,
};

static void ipoctal_register_types(void)
{
    type_register_static(&ipoctal_info);
}

type_init(ipoctal_register_types)
