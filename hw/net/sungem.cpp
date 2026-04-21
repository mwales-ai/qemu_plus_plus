/*
 * QEMU model of SUN GEM ethernet controller
 *
 * As found in Apple ASICs among others
 *
 * Copyright 2016 Ben Herrenschmidt
 * Copyright 2017 Mark Cave-Ayland
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/checksum.h"
#include "hw/net/mii.h"
#include "system/system.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_SUNGEM "sungem"

OBJECT_DECLARE_SIMPLE_TYPE(SunGEMState, SUNGEM)

#define MAX_PACKET_SIZE 9016

#define SUNGEM_MMIO_SIZE        0x200000

/* Global registers */
#define SUNGEM_MMIO_GREG_SIZE   0x2000

#define GREG_SEBSTATE     0x0000UL    /* SEB State Register */

#define GREG_STAT         0x000CUL    /* Status Register */
#define GREG_STAT_TXINTME     0x00000001    /* TX INTME frame transferred */
#define GREG_STAT_TXALL       0x00000002    /* All TX frames transferred */
#define GREG_STAT_TXDONE      0x00000004    /* One TX frame transferred */
#define GREG_STAT_RXDONE      0x00000010    /* One RX frame arrived */
#define GREG_STAT_RXNOBUF     0x00000020    /* No free RX buffers available */
#define GREG_STAT_RXTAGERR    0x00000040    /* RX tag framing is corrupt */
#define GREG_STAT_TXMAC       0x00004000    /* TX MAC signalled interrupt */
#define GREG_STAT_RXMAC       0x00008000    /* RX MAC signalled interrupt */
#define GREG_STAT_MAC         0x00010000    /* MAC Control signalled irq */
#define GREG_STAT_TXNR        0xfff80000    /* == TXDMA_TXDONE reg val */
#define GREG_STAT_TXNR_SHIFT  19

/* These interrupts are edge latches in the status register,
 * reading it (or writing the corresponding bit in IACK) will
 * clear them
 */
#define GREG_STAT_LATCH       (GREG_STAT_TXALL  | GREG_STAT_TXINTME | \
                               GREG_STAT_RXDONE | GREG_STAT_RXDONE |  \
                               GREG_STAT_RXNOBUF | GREG_STAT_RXTAGERR)

#define GREG_IMASK        0x0010UL    /* Interrupt Mask Register */
#define GREG_IACK         0x0014UL    /* Interrupt ACK Register */
#define GREG_STAT2        0x001CUL    /* Alias of GREG_STAT */
#define GREG_PCIESTAT     0x1000UL    /* PCI Error Status Register */
#define GREG_PCIEMASK     0x1004UL    /* PCI Error Mask Register */

#define GREG_SWRST        0x1010UL    /* Software Reset Register */
#define GREG_SWRST_TXRST      0x00000001    /* TX Software Reset */
#define GREG_SWRST_RXRST      0x00000002    /* RX Software Reset */
#define GREG_SWRST_RSTOUT     0x00000004    /* Force RST# pin active */

/* TX DMA Registers */
#define SUNGEM_MMIO_TXDMA_SIZE   0x1000

#define TXDMA_KICK        0x0000UL    /* TX Kick Register */

#define TXDMA_CFG         0x0004UL    /* TX Configuration Register */
#define TXDMA_CFG_ENABLE      0x00000001    /* Enable TX DMA channel */
#define TXDMA_CFG_RINGSZ      0x0000001e    /* TX descriptor ring size */

#define TXDMA_DBLOW       0x0008UL    /* TX Desc. Base Low */
#define TXDMA_DBHI        0x000CUL    /* TX Desc. Base High */
#define TXDMA_PCNT        0x0024UL    /* TX FIFO Packet Counter */
#define TXDMA_SMACHINE    0x0028UL    /* TX State Machine Register */
#define TXDMA_DPLOW       0x0030UL    /* TX Data Pointer Low */
#define TXDMA_DPHI        0x0034UL    /* TX Data Pointer High */
#define TXDMA_TXDONE      0x0100UL    /* TX Completion Register */
#define TXDMA_FTAG        0x0108UL    /* TX FIFO Tag */
#define TXDMA_FSZ         0x0118UL    /* TX FIFO Size */

/* Receive DMA Registers */
#define SUNGEM_MMIO_RXDMA_SIZE   0x2000

#define RXDMA_CFG         0x0000UL    /* RX Configuration Register */
#define RXDMA_CFG_ENABLE      0x00000001    /* Enable RX DMA channel */
#define RXDMA_CFG_RINGSZ      0x0000001e    /* RX descriptor ring size */
#define RXDMA_CFG_FBOFF       0x00001c00    /* Offset of first data byte */
#define RXDMA_CFG_CSUMOFF     0x000fe000    /* Skip bytes before csum calc */

#define RXDMA_DBLOW       0x0004UL    /* RX Descriptor Base Low */
#define RXDMA_DBHI        0x0008UL    /* RX Descriptor Base High */
#define RXDMA_PCNT        0x0018UL    /* RX FIFO Packet Counter */
#define RXDMA_SMACHINE    0x001CUL    /* RX State Machine Register */
#define RXDMA_PTHRESH     0x0020UL    /* Pause Thresholds */
#define RXDMA_DPLOW       0x0024UL    /* RX Data Pointer Low */
#define RXDMA_DPHI        0x0028UL    /* RX Data Pointer High */
#define RXDMA_KICK        0x0100UL    /* RX Kick Register */
#define RXDMA_DONE        0x0104UL    /* RX Completion Register */
#define RXDMA_BLANK       0x0108UL    /* RX Blanking Register */
#define RXDMA_FTAG        0x0110UL    /* RX FIFO Tag */
#define RXDMA_FSZ         0x0120UL    /* RX FIFO Size */

/* WOL Registers */
#define SUNGEM_MMIO_WOL_SIZE   0x14

#define WOL_MATCH0        0x0000UL
#define WOL_MATCH1        0x0004UL
#define WOL_MATCH2        0x0008UL
#define WOL_MCOUNT        0x000CUL
#define WOL_WAKECSR       0x0010UL

/* MAC Registers */
#define SUNGEM_MMIO_MAC_SIZE   0x200

#define MAC_TXRST         0x0000UL    /* TX MAC Software Reset Command */
#define MAC_RXRST         0x0004UL    /* RX MAC Software Reset Command */
#define MAC_TXSTAT        0x0010UL    /* TX MAC Status Register */
#define MAC_RXSTAT        0x0014UL    /* RX MAC Status Register */

#define MAC_CSTAT         0x0018UL    /* MAC Control Status Register */
#define MAC_CSTAT_PTR         0xffff0000    /* Pause Time Received */

#define MAC_TXMASK        0x0020UL    /* TX MAC Mask Register */
#define MAC_RXMASK        0x0024UL    /* RX MAC Mask Register */
#define MAC_MCMASK        0x0028UL    /* MAC Control Mask Register */

#define MAC_TXCFG         0x0030UL    /* TX MAC Configuration Register */
#define MAC_TXCFG_ENAB        0x00000001    /* TX MAC Enable */

#define MAC_RXCFG         0x0034UL    /* RX MAC Configuration Register */
#define MAC_RXCFG_ENAB        0x00000001    /* RX MAC Enable */
#define MAC_RXCFG_SFCS        0x00000004    /* Strip FCS */
#define MAC_RXCFG_PROM        0x00000008    /* Promiscuous Mode */
#define MAC_RXCFG_PGRP        0x00000010    /* Promiscuous Group */
#define MAC_RXCFG_HFE         0x00000020    /* Hash Filter Enable */

#define MAC_XIFCFG        0x003CUL    /* XIF Configuration Register */
#define MAC_XIFCFG_LBCK       0x00000002    /* Loopback TX to RX */

#define MAC_MINFSZ        0x0050UL    /* MinFrameSize Register */
#define MAC_MAXFSZ        0x0054UL    /* MaxFrameSize Register */
#define MAC_ADDR0         0x0080UL    /* MAC Address 0 Register */
#define MAC_ADDR1         0x0084UL    /* MAC Address 1 Register */
#define MAC_ADDR2         0x0088UL    /* MAC Address 2 Register */
#define MAC_ADDR3         0x008CUL    /* MAC Address 3 Register */
#define MAC_ADDR4         0x0090UL    /* MAC Address 4 Register */
#define MAC_ADDR5         0x0094UL    /* MAC Address 5 Register */
#define MAC_HASH0         0x00C0UL    /* Hash Table 0 Register */
#define MAC_PATMPS        0x0114UL    /* Peak Attempts Register */
#define MAC_SMACHINE      0x0134UL    /* State Machine Register */

/* MIF Registers */
#define SUNGEM_MMIO_MIF_SIZE   0x20

#define MIF_FRAME         0x000CUL    /* MIF Frame/Output Register */
#define MIF_FRAME_OP          0x30000000    /* OPcode */
#define MIF_FRAME_PHYAD       0x0f800000    /* PHY ADdress */
#define MIF_FRAME_REGAD       0x007c0000    /* REGister ADdress */
#define MIF_FRAME_TALSB       0x00010000    /* Turn Around LSB */
#define MIF_FRAME_DATA        0x0000ffff    /* Instruction Payload */

#define MIF_CFG           0x0010UL    /* MIF Configuration Register */
#define MIF_CFG_MDI0          0x00000100    /* MDIO_0 present or read-bit */
#define MIF_CFG_MDI1          0x00000200    /* MDIO_1 present or read-bit */

#define MIF_STATUS        0x0018UL    /* MIF Status Register */
#define MIF_SMACHINE      0x001CUL    /* MIF State Machine Register */

/* PCS/Serialink Registers */
#define SUNGEM_MMIO_PCS_SIZE   0x60
#define PCS_MIISTAT       0x0004UL    /* PCS MII Status Register */
#define PCS_ISTAT         0x0018UL    /* PCS Interrupt Status Reg */

#define PCS_SSTATE        0x005CUL    /* Serialink State Register */

/* Descriptors */
struct gem_txd {
    uint64_t control_word;
    uint64_t buffer;
};

#define TXDCTRL_BUFSZ     0x0000000000007fffULL  /* Buffer Size */
#define TXDCTRL_CSTART    0x00000000001f8000ULL  /* CSUM Start Offset */
#define TXDCTRL_COFF      0x000000001fe00000ULL  /* CSUM Stuff Offset */
#define TXDCTRL_CENAB     0x0000000020000000ULL  /* CSUM Enable */
#define TXDCTRL_EOF       0x0000000040000000ULL  /* End of Frame */
#define TXDCTRL_SOF       0x0000000080000000ULL  /* Start of Frame */
#define TXDCTRL_INTME     0x0000000100000000ULL  /* "Interrupt Me" */

struct gem_rxd {
    uint64_t status_word;
    uint64_t buffer;
};

#define RXDCTRL_HPASS     0x1000000000000000ULL  /* Passed Hash Filter */
#define RXDCTRL_ALTMAC    0x2000000000000000ULL  /* Matched ALT MAC */


struct SunGEMState {
    PCIDevice pdev;

    MemoryRegion sungem;
    MemoryRegion greg;
    MemoryRegion txdma;
    MemoryRegion rxdma;
    MemoryRegion wol;
    MemoryRegion mac;
    MemoryRegion mif;
    MemoryRegion pcs;
    NICState *nic;
    NICConf conf;
    uint32_t phy_addr;

    uint32_t gregs[SUNGEM_MMIO_GREG_SIZE >> 2];
    uint32_t txdmaregs[SUNGEM_MMIO_TXDMA_SIZE >> 2];
    uint32_t rxdmaregs[SUNGEM_MMIO_RXDMA_SIZE >> 2];
    uint32_t macregs[SUNGEM_MMIO_MAC_SIZE >> 2];
    uint32_t mifregs[SUNGEM_MMIO_MIF_SIZE >> 2];
    uint32_t pcsregs[SUNGEM_MMIO_PCS_SIZE >> 2];

    /* Cache some useful things */
    uint32_t rx_mask;
    uint32_t tx_mask;

    /* Current tx packet */
    uint8_t tx_data[MAX_PACKET_SIZE];
    uint32_t tx_size;
    uint64_t tx_first_ctl;

    /* Methods */
    void realize(PCIDevice *pci_dev, Error **errp);
    void reset();
    void instanceInit();
    void evalIrq();
    void updateStatus(uint32_t bits, bool val);
    void evalCascadeIrq();
    void doTxCsum();
    void sendPacket(const uint8_t *buf, int size);
    void processTxDesc(struct gem_txd *desc);
    void txKick();
    bool rxFull(uint32_t kick, uint32_t done);
    int checkRxMac(const uint8_t *mac, uint32_t crc);
    void updateMasks();
    void resetRx();
    void resetTx();
    void resetAll(bool pci_reset);
    void miiWrite(uint8_t phy_addr, uint8_t reg_addr, uint16_t val);
    uint16_t miiReadInternal(uint8_t phy_addr, uint8_t reg_addr);
    uint16_t miiRead(uint8_t phy_addr, uint8_t reg_addr);
    uint32_t miiOp(uint32_t val);
    void mmioGregWrite(hwaddr addr, uint64_t val, unsigned size);
    uint64_t mmioGregRead(hwaddr addr, unsigned size);
    void mmioTxdmaWrite(hwaddr addr, uint64_t val, unsigned size);
    uint64_t mmioTxdmaRead(hwaddr addr, unsigned size);
    void mmioRxdmaWrite(hwaddr addr, uint64_t val, unsigned size);
    uint64_t mmioRxdmaRead(hwaddr addr, unsigned size);
    void mmioMacWrite(hwaddr addr, uint64_t val, unsigned size);
    uint64_t mmioMacRead(hwaddr addr, unsigned size);
    void mmioMifWrite(hwaddr addr, uint64_t val, unsigned size);
    uint64_t mmioMifRead(hwaddr addr, unsigned size);
    void mmioPcsWrite(hwaddr addr, uint64_t val, unsigned size);
    uint64_t mmioPcsRead(hwaddr addr, unsigned size);

    /* Static callbacks */
    static void realizeWrapper(PCIDevice *pci_dev, Error **errp);
    static void resetWrapper(DeviceState *dev);
    void init();
    static void classInit(DeviceClass *dc);
    static void mmioGregWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioGregReadCb(void *opaque, hwaddr addr, unsigned size);
    static void mmioTxdmaWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioTxdmaReadCb(void *opaque, hwaddr addr, unsigned size);
    static void mmioRxdmaWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioRxdmaReadCb(void *opaque, hwaddr addr, unsigned size);
    static void mmioWolWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioWolReadCb(void *opaque, hwaddr addr, unsigned size);
    static void mmioMacWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioMacReadCb(void *opaque, hwaddr addr, unsigned size);
    static void mmioMifWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioMifReadCb(void *opaque, hwaddr addr, unsigned size);
    static void mmioPcsWriteCb(void *opaque, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t mmioPcsReadCb(void *opaque, hwaddr addr, unsigned size);
    static bool canReceiveCb(NetClientState *nc);
    static ssize_t receiveCb(NetClientState *nc, const uint8_t *buf, size_t size);
    static void setLinkStatusCb(NetClientState *nc);
    static void uninit(PCIDevice *dev);
};


void SunGEMState::evalIrq()
{
    uint32_t stat, mask;

    mask = gregs[GREG_IMASK >> 2];
    stat = gregs[GREG_STAT >> 2] & ~GREG_STAT_TXNR;
    if (stat & ~mask) {
        pci_set_irq(reinterpret_cast<PCIDevice *>(this), 1);
    } else {
        pci_set_irq(reinterpret_cast<PCIDevice *>(this), 0);
    }
}

void SunGEMState::updateStatus(uint32_t bits, bool val)
{
    uint32_t stat;

    stat = gregs[GREG_STAT >> 2];
    if (val) {
        stat |= bits;
    } else {
        stat &= ~bits;
    }
    gregs[GREG_STAT >> 2] = stat;
    evalIrq();
}

void SunGEMState::evalCascadeIrq()
{
    uint32_t stat, mask;

    mask = macregs[MAC_TXSTAT >> 2];
    stat = macregs[MAC_TXMASK >> 2];
    if (stat & ~mask) {
        updateStatus(GREG_STAT_TXMAC, true);
    } else {
        updateStatus(GREG_STAT_TXMAC, false);
    }

    mask = macregs[MAC_RXSTAT >> 2];
    stat = macregs[MAC_RXMASK >> 2];
    if (stat & ~mask) {
        updateStatus(GREG_STAT_RXMAC, true);
    } else {
        updateStatus(GREG_STAT_RXMAC, false);
    }

    mask = macregs[MAC_CSTAT >> 2];
    stat = macregs[MAC_MCMASK >> 2] & ~MAC_CSTAT_PTR;
    if (stat & ~mask) {
        updateStatus(GREG_STAT_MAC, true);
    } else {
        updateStatus(GREG_STAT_MAC, false);
    }
}

void SunGEMState::doTxCsum()
{
    uint16_t start, off;
    uint32_t csum;

    start = (tx_first_ctl & TXDCTRL_CSTART) >> 15;
    off = (tx_first_ctl & TXDCTRL_COFF) >> 21;

    trace_sungem_tx_checksum(start, off);

    if (start > (tx_size - 2) || off > (tx_size - 2)) {
        trace_sungem_tx_checksum_oob();
        return;
    }

    csum = net_raw_checksum(tx_data + start, tx_size - start);
    stw_be_p(tx_data + off, csum);
}

void SunGEMState::sendPacket(const uint8_t *buf, int size)
{
    NetClientState *nc = qemu_get_queue(nic);

    if (macregs[MAC_XIFCFG >> 2] & MAC_XIFCFG_LBCK) {
        qemu_receive_packet(nc, buf, size);
    } else {
        qemu_send_packet(nc, buf, size);
    }
}

void SunGEMState::processTxDesc(struct gem_txd *desc)
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(this);
    uint32_t len;

    /* If it's a start of frame, discard anything we had in the
     * buffer and start again. This should be an error condition
     * if we had something ... for now we ignore it
     */
    if (desc->control_word & TXDCTRL_SOF) {
        if (tx_first_ctl) {
            trace_sungem_tx_unfinished();
        }
        tx_size = 0;
        tx_first_ctl = desc->control_word;
    }

    /* Grab data size */
    len = desc->control_word & TXDCTRL_BUFSZ;

    /* Clamp it to our max size */
    if ((tx_size + len) > MAX_PACKET_SIZE) {
        trace_sungem_tx_overflow();
        len = MAX_PACKET_SIZE - tx_size;
    }

    /* Read the data */
    pci_dma_read(d, desc->buffer, &tx_data[tx_size], len);
    tx_size += len;

    /* If end of frame, send packet */
    if (desc->control_word & TXDCTRL_EOF) {
        trace_sungem_tx_finished(tx_size);

        /* Handle csum */
        if (tx_first_ctl & TXDCTRL_CENAB) {
            doTxCsum();
        }

        /* Send it */
        sendPacket(tx_data, tx_size);

        /* No more pending packet */
        tx_size = 0;
        tx_first_ctl = 0;
    }
}

void SunGEMState::txKick()
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(this);
    uint32_t comp, kick;
    uint32_t txdma_cfg, txmac_cfg, ints;
    uint64_t dbase;

    trace_sungem_tx_kick();

    /* Check that both TX MAC and TX DMA are enabled. We don't
     * handle DMA-less direct FIFO operations (we don't emulate
     * the FIFO at all).
     *
     * A write to TXDMA_KICK while DMA isn't enabled can happen
     * when the driver is resetting the pointer.
     */
    txdma_cfg = txdmaregs[TXDMA_CFG >> 2];
    txmac_cfg = macregs[MAC_TXCFG >> 2];
    if (!(txdma_cfg & TXDMA_CFG_ENABLE) ||
        !(txmac_cfg & MAC_TXCFG_ENAB)) {
        trace_sungem_tx_disabled();
        return;
    }

    /* XXX Test min frame size register ? */
    /* XXX Test max frame size register ? */

    dbase = txdmaregs[TXDMA_DBHI >> 2];
    dbase = (dbase << 32) | txdmaregs[TXDMA_DBLOW >> 2];

    comp = txdmaregs[TXDMA_TXDONE >> 2] & tx_mask;
    kick = txdmaregs[TXDMA_KICK >> 2] & tx_mask;

    trace_sungem_tx_process(comp, kick, tx_mask + 1);

    /* This is rather primitive for now, we just send everything we
     * can in one go, like e1000. Ideally we should do the sending
     * from some kind of background task
     */
    while (comp != kick) {
        struct gem_txd desc;

        /* Read the next descriptor */
        pci_dma_read(d, dbase + comp * sizeof(desc), &desc, sizeof(desc));

        /* Byteswap descriptor */
        desc.control_word = le64_to_cpu(desc.control_word);
        desc.buffer = le64_to_cpu(desc.buffer);
        trace_sungem_tx_desc(comp, desc.control_word, desc.buffer);

        /* Send it for processing */
        processTxDesc(&desc);

        /* Interrupt */
        ints = GREG_STAT_TXDONE;
        if (desc.control_word & TXDCTRL_INTME) {
            ints |= GREG_STAT_TXINTME;
        }
        updateStatus(ints, true);

        /* Next ! */
        comp = (comp + 1) & tx_mask;
        txdmaregs[TXDMA_TXDONE >> 2] = comp;
    }

    /* We sent everything, set status/irq bit */
    updateStatus(GREG_STAT_TXALL, true);
}

bool SunGEMState::rxFull(uint32_t kick, uint32_t done)
{
    return kick == ((done + 1) & rx_mask);
}

bool SunGEMState::canReceiveCb(NetClientState *nc)
{
    SunGEMState *s = static_cast<SunGEMState *>(qemu_get_nic_opaque(nc));
    uint32_t kick, done, rxdma_cfg, rxmac_cfg;
    bool full;

    rxmac_cfg = s->macregs[MAC_RXCFG >> 2];
    rxdma_cfg = s->rxdmaregs[RXDMA_CFG >> 2];

    /* If MAC disabled, can't receive */
    if ((rxmac_cfg & MAC_RXCFG_ENAB) == 0) {
        trace_sungem_rx_mac_disabled();
        return false;
    }
    if ((rxdma_cfg & RXDMA_CFG_ENABLE) == 0) {
        trace_sungem_rx_txdma_disabled();
        return false;
    }

    /* Check RX availability */
    kick = s->rxdmaregs[RXDMA_KICK >> 2];
    done = s->rxdmaregs[RXDMA_DONE >> 2];
    full = s->rxFull(kick, done);

    trace_sungem_rx_check(!full, kick, done);

    return !full;
}

enum {
        rx_no_match,
        rx_match_promisc,
        rx_match_bcast,
        rx_match_allmcast,
        rx_match_mcast,
        rx_match_mac,
        rx_match_altmac,
};

int SunGEMState::checkRxMac(const uint8_t *mac, uint32_t crc)
{
    uint32_t rxcfg = macregs[MAC_RXCFG >> 2];
    uint32_t mac0, mac1, mac2;

    /* Promisc enabled ? */
    if (rxcfg & MAC_RXCFG_PROM) {
        return rx_match_promisc;
    }

    /* Format MAC address into dwords */
    mac0 = (mac[4] << 8) | mac[5];
    mac1 = (mac[2] << 8) | mac[3];
    mac2 = (mac[0] << 8) | mac[1];

    trace_sungem_rx_mac_check(mac0, mac1, mac2);

    /* Is this a broadcast frame ? */
    if (mac0 == 0xffff && mac1 == 0xffff && mac2 == 0xffff) {
        return rx_match_bcast;
    }

    /* TODO: Implement address filter registers (or we don't care ?) */

    /* Is this a multicast frame ? */
    if (mac[0] & 1) {
        trace_sungem_rx_mac_multicast();

        /* Promisc group enabled ? */
        if (rxcfg & MAC_RXCFG_PGRP) {
            return rx_match_allmcast;
        }

        /* TODO: Check MAC control frames (or we don't care) ? */

        /* Check hash filter (somebody check that's correct ?) */
        if (rxcfg & MAC_RXCFG_HFE) {
            uint32_t hash, idx;

            crc >>= 24;
            idx = (crc >> 2) & 0x3c;
            hash = macregs[(MAC_HASH0 + idx) >> 2];
            if (hash & (1 << (15 - (crc & 0xf)))) {
                return rx_match_mcast;
            }
        }
        return rx_no_match;
    }

    /* Main MAC check */
    trace_sungem_rx_mac_compare(macregs[MAC_ADDR0 >> 2],
                                macregs[MAC_ADDR1 >> 2],
                                macregs[MAC_ADDR2 >> 2]);

    if (mac0 == macregs[MAC_ADDR0 >> 2] &&
        mac1 == macregs[MAC_ADDR1 >> 2] &&
        mac2 == macregs[MAC_ADDR2 >> 2]) {
        return rx_match_mac;
    }

    /* Alt MAC check */
    if (mac0 == macregs[MAC_ADDR3 >> 2] &&
        mac1 == macregs[MAC_ADDR4 >> 2] &&
        mac2 == macregs[MAC_ADDR5 >> 2]) {
        return rx_match_altmac;
    }

    return rx_no_match;
}

ssize_t SunGEMState::receiveCb(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    SunGEMState *s = static_cast<SunGEMState *>(qemu_get_nic_opaque(nc));
    PCIDevice *d = reinterpret_cast<PCIDevice *>(s);
    uint32_t mac_crc, done, kick, max_fsize;
    uint32_t fcs_size, ints, rxdma_cfg, rxmac_cfg, csum, coff;
    struct gem_rxd desc;
    uint64_t dbase, baddr;
    unsigned int rx_cond;

    trace_sungem_rx_packet(size);

    rxmac_cfg = s->macregs[MAC_RXCFG >> 2];
    rxdma_cfg = s->rxdmaregs[RXDMA_CFG >> 2];
    max_fsize = s->macregs[MAC_MAXFSZ >> 2] & 0x7fff;

    /* If MAC or DMA disabled, can't receive */
    if (!(rxdma_cfg & RXDMA_CFG_ENABLE) ||
        !(rxmac_cfg & MAC_RXCFG_ENAB)) {
        trace_sungem_rx_disabled();
        return 0;
    }

    /* Size adjustment for FCS */
    if (rxmac_cfg & MAC_RXCFG_SFCS) {
        fcs_size = 0;
    } else {
        fcs_size = 4;
    }

    /* Discard frame smaller than a MAC or larger than max frame size
     * (when accounting for FCS)
     */
    if (size < 6 || (size + 4) > max_fsize) {
        trace_sungem_rx_bad_frame_size(size);
        /* XXX Increment error statistics ? */
        return size;
    }

    /* Get MAC crc */
    mac_crc = net_crc32_le(buf, ETH_ALEN);

    /* Packet isn't for me ? */
    rx_cond = s->checkRxMac(buf, mac_crc);
    if (rx_cond == rx_no_match) {
        /* Just drop it */
        trace_sungem_rx_unmatched();
        return size;
    }

    /* Get ring pointers */
    kick = s->rxdmaregs[RXDMA_KICK >> 2] & s->rx_mask;
    done = s->rxdmaregs[RXDMA_DONE >> 2] & s->rx_mask;

    trace_sungem_rx_process(done, kick, s->rx_mask + 1);

    /* Ring full ? Can't receive */
    if (s->rxFull(kick, done)) {
        trace_sungem_rx_ringfull();
        return 0;
    }

    /* Note: The real GEM will fetch descriptors in blocks of 4,
     * for now we handle them one at a time, I think the driver will
     * cope
     */

    dbase = s->rxdmaregs[RXDMA_DBHI >> 2];
    dbase = (dbase << 32) | s->rxdmaregs[RXDMA_DBLOW >> 2];

    /* Read the next descriptor */
    pci_dma_read(d, dbase + done * sizeof(desc), &desc, sizeof(desc));

    trace_sungem_rx_desc(le64_to_cpu(desc.status_word),
                         le64_to_cpu(desc.buffer));

    /* Effective buffer address */
    baddr = le64_to_cpu(desc.buffer) & ~7ull;
    baddr |= (rxdma_cfg & RXDMA_CFG_FBOFF) >> 10;

    /* Write buffer out */
    pci_dma_write(d, baddr, buf, size);

    if (fcs_size) {
        /* Should we add an FCS ? Linux doesn't ask us to strip it,
         * however I believe nothing checks it... For now we just
         * do nothing. It's faster this way.
         */
    }

    /* Calculate the checksum */
    coff = (rxdma_cfg & RXDMA_CFG_CSUMOFF) >> 13;
    csum = net_raw_checksum((uint8_t *)buf + coff, size - coff);

    /* Build the updated descriptor */
    desc.status_word = (size + fcs_size) << 16;
    desc.status_word |= ((uint64_t)(mac_crc >> 16)) << 44;
    desc.status_word |= csum;
    if (rx_cond == rx_match_mcast) {
        desc.status_word |= RXDCTRL_HPASS;
    }
    if (rx_cond == rx_match_altmac) {
        desc.status_word |= RXDCTRL_ALTMAC;
    }
    desc.status_word = cpu_to_le64(desc.status_word);

    pci_dma_write(d, dbase + done * sizeof(desc), &desc, sizeof(desc));

    done = (done + 1) & s->rx_mask;
    s->rxdmaregs[RXDMA_DONE >> 2] = done;

    /* XXX Unconditionally set RX interrupt for now. The interrupt
     * mitigation timer might well end up adding more overhead than
     * helping here...
     */
    ints = GREG_STAT_RXDONE;
    if (s->rxFull(kick, done)) {
        ints |= GREG_STAT_RXNOBUF;
    }
    s->updateStatus(ints, true);

    return size;
}

void SunGEMState::setLinkStatusCb(NetClientState *nc)
{
    /* We don't do anything for now as I believe none of the OSes
     * drivers use the MIF autopoll feature nor the PHY interrupt
     */
}

void SunGEMState::updateMasks()
{
    uint32_t sz;

    sz = 1 << (((rxdmaregs[RXDMA_CFG >> 2] & RXDMA_CFG_RINGSZ) >> 1) + 5);
    rx_mask = sz - 1;

    sz = 1 << (((txdmaregs[TXDMA_CFG >> 2] & TXDMA_CFG_RINGSZ) >> 1) + 5);
    tx_mask = sz - 1;
}

void SunGEMState::resetRx()
{
    trace_sungem_rx_reset();

    /* XXX Do RXCFG */
    /* XXX Check value */
    rxdmaregs[RXDMA_FSZ >> 2] = 0x140;
    rxdmaregs[RXDMA_DONE >> 2] = 0;
    rxdmaregs[RXDMA_KICK >> 2] = 0;
    rxdmaregs[RXDMA_CFG >> 2] = 0x1000010;
    rxdmaregs[RXDMA_PTHRESH >> 2] = 0xf8;
    rxdmaregs[RXDMA_BLANK >> 2] = 0;

    updateMasks();
}

void SunGEMState::resetTx()
{
    trace_sungem_tx_reset();

    /* XXX Do TXCFG */
    /* XXX Check value */
    txdmaregs[TXDMA_FSZ >> 2] = 0x90;
    txdmaregs[TXDMA_TXDONE >> 2] = 0;
    txdmaregs[TXDMA_KICK >> 2] = 0;
    txdmaregs[TXDMA_CFG >> 2] = 0x118010;

    updateMasks();

    tx_size = 0;
    tx_first_ctl = 0;
}

void SunGEMState::resetAll(bool pci_reset)
{
    trace_sungem_reset(pci_reset);

    resetRx();
    resetTx();

    gregs[GREG_IMASK >> 2] = 0xFFFFFFF;
    gregs[GREG_STAT >> 2] = 0;
    if (pci_reset) {
        uint8_t *ma = conf.macaddr.a;

        gregs[GREG_SWRST >> 2] = 0;
        macregs[MAC_ADDR0 >> 2] = (ma[4] << 8) | ma[5];
        macregs[MAC_ADDR1 >> 2] = (ma[2] << 8) | ma[3];
        macregs[MAC_ADDR2 >> 2] = (ma[0] << 8) | ma[1];
    } else {
        gregs[GREG_SWRST >> 2] &= GREG_SWRST_RSTOUT;
    }
    mifregs[MIF_CFG >> 2] = MIF_CFG_MDI0;
}

void SunGEMState::miiWrite(uint8_t phy_addr, uint8_t reg_addr, uint16_t val)
{
    trace_sungem_mii_write(phy_addr, reg_addr, val);

    /* XXX TODO */
}

uint16_t SunGEMState::miiReadInternal(uint8_t phy_addr, uint8_t reg_addr)
{
    if (phy_addr != this->phy_addr) {
        return 0xffff;
    }
    /* Primitive emulation of a BCM5201 to please the driver,
     * ID is 0x00406210. TODO: Do a gigabit PHY like BCM5400
     */
    switch (reg_addr) {
    case MII_BMCR:
        return 0;
    case MII_PHYID1:
        return 0x0040;
    case MII_PHYID2:
        return 0x6210;
    case MII_BMSR:
        if (qemu_get_queue(nic)->link_down) {
            return MII_BMSR_100TX_FD  | MII_BMSR_AUTONEG;
        } else {
            return MII_BMSR_100TX_FD | MII_BMSR_AN_COMP |
                    MII_BMSR_AUTONEG | MII_BMSR_LINK_ST;
        }
    case MII_ANLPAR:
    case MII_ANAR:
        return MII_ANLPAR_TXFD;
    case 0x18: /* 5201 AUX status */
        return 3; /* 100FD */
    default:
        return 0;
    };
}
uint16_t SunGEMState::miiRead(uint8_t phy_addr, uint8_t reg_addr)
{
    uint16_t val;

    val = miiReadInternal(phy_addr, reg_addr);

    trace_sungem_mii_read(phy_addr, reg_addr, val);

    return val;
}

uint32_t SunGEMState::miiOp(uint32_t val)
{
    uint8_t phy_addr, reg_addr, op;

    /* Ignore not start of frame */
    if ((val >> 30) != 1) {
        trace_sungem_mii_invalid_sof(val >> 30);
        return 0xffff;
    }
    phy_addr = (val & MIF_FRAME_PHYAD) >> 23;
    reg_addr = (val & MIF_FRAME_REGAD) >> 18;
    op = (val & MIF_FRAME_OP) >> 28;
    switch (op) {
    case 1:
        miiWrite(phy_addr, reg_addr, val & MIF_FRAME_DATA);
        return val | MIF_FRAME_TALSB;
    case 2:
        return miiRead(phy_addr, reg_addr) | MIF_FRAME_TALSB;
    default:
        trace_sungem_mii_invalid_op(op);
    }
    return 0xffff | MIF_FRAME_TALSB;
}

void SunGEMState::mmioGregWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    static_cast<SunGEMState *>(opaque)->mmioGregWrite(addr, val, size);
}

void SunGEMState::mmioGregWrite(hwaddr addr, uint64_t val, unsigned size)
{

    if (!(addr < 0x20) && !(addr >= 0x1000 && addr <= 0x1010)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown GREG register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }

    trace_sungem_mmio_greg_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case GREG_SEBSTATE:
    case GREG_STAT:
    case GREG_STAT2:
    case GREG_PCIESTAT:
        return; /* No actual write */
    case GREG_IACK:
        val &= GREG_STAT_LATCH;
        gregs[GREG_STAT >> 2] &= ~val;
        evalIrq();
        return; /* No actual write */
    case GREG_PCIEMASK:
        val &= 0x7;
        break;
    }

    gregs[addr  >> 2] = val;

    /* Post write action */
    switch (addr) {
    case GREG_IMASK:
        /* Re-evaluate interrupt */
        evalIrq();
        break;
    case GREG_SWRST:
        switch (val & (GREG_SWRST_TXRST | GREG_SWRST_RXRST)) {
        case GREG_SWRST_RXRST:
            resetRx();
            break;
        case GREG_SWRST_TXRST:
            resetTx();
            break;
        case GREG_SWRST_RXRST | GREG_SWRST_TXRST:
            resetAll(false);
        }
        break;
    }
}

uint64_t SunGEMState::mmioGregReadCb(void *opaque, hwaddr addr, unsigned size)
{
    return static_cast<SunGEMState *>(opaque)->mmioGregRead(addr, size);
}

uint64_t SunGEMState::mmioGregRead(hwaddr addr, unsigned size)
{
    uint32_t val;

    if (!(addr < 0x20) && !(addr >= 0x1000 && addr <= 0x1010)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown GREG register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }

    val = gregs[addr >> 2];

    trace_sungem_mmio_greg_read(addr, val);

    switch (addr) {
    case GREG_STAT:
        /* Side effect, clear bottom 7 bits */
        gregs[GREG_STAT >> 2] &= ~GREG_STAT_LATCH;
        evalIrq();

        /* Inject TX completion in returned value */
        val = (val & ~GREG_STAT_TXNR) |
                (txdmaregs[TXDMA_TXDONE >> 2] << GREG_STAT_TXNR_SHIFT);
        break;
    case GREG_STAT2:
        /* Return the status reg without side effect
         * (and inject TX completion in returned value)
         */
        val = (gregs[GREG_STAT >> 2] & ~GREG_STAT_TXNR) |
              (txdmaregs[TXDMA_TXDONE >> 2] << GREG_STAT_TXNR_SHIFT);
        break;
    }

    return val;
}

static const MemoryRegionOps sungem_mmio_greg_ops = {
    .read = SunGEMState::mmioGregReadCb,
    .write = SunGEMState::mmioGregWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::mmioTxdmaWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    static_cast<SunGEMState *>(opaque)->mmioTxdmaWrite(addr, val, size);
}

void SunGEMState::mmioTxdmaWrite(hwaddr addr, uint64_t val, unsigned size)
{

    if (!(addr < 0x38) && !(addr >= 0x100 && addr <= 0x118)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown TXDMA register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }

    trace_sungem_mmio_txdma_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case TXDMA_TXDONE:
    case TXDMA_PCNT:
    case TXDMA_SMACHINE:
    case TXDMA_DPLOW:
    case TXDMA_DPHI:
    case TXDMA_FSZ:
    case TXDMA_FTAG:
        return; /* No actual write */
    }

    txdmaregs[addr >> 2] = val;

    /* Post write action */
    switch (addr) {
    case TXDMA_KICK:
        txKick();
        break;
    case TXDMA_CFG:
        updateMasks();
        break;
    }
}

uint64_t SunGEMState::mmioTxdmaReadCb(void *opaque, hwaddr addr, unsigned size)
{
    return static_cast<SunGEMState *>(opaque)->mmioTxdmaRead(addr, size);
}

uint64_t SunGEMState::mmioTxdmaRead(hwaddr addr, unsigned size)
{
    uint32_t val;

    if (!(addr < 0x38) && !(addr >= 0x100 && addr <= 0x118)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown TXDMA register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }

    val = txdmaregs[addr >> 2];

    trace_sungem_mmio_txdma_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_txdma_ops = {
    .read = SunGEMState::mmioTxdmaReadCb,
    .write = SunGEMState::mmioTxdmaWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::mmioRxdmaWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    static_cast<SunGEMState *>(opaque)->mmioRxdmaWrite(addr, val, size);
}

void SunGEMState::mmioRxdmaWrite(hwaddr addr, uint64_t val, unsigned size)
{

    if (!(addr <= 0x28) && !(addr >= 0x100 && addr <= 0x120)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown RXDMA register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }

    trace_sungem_mmio_rxdma_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case RXDMA_DONE:
    case RXDMA_PCNT:
    case RXDMA_SMACHINE:
    case RXDMA_DPLOW:
    case RXDMA_DPHI:
    case RXDMA_FSZ:
    case RXDMA_FTAG:
        return; /* No actual write */
    }

    rxdmaregs[addr >> 2] = val;

    /* Post write action */
    switch (addr) {
    case RXDMA_KICK:
        trace_sungem_rx_kick(val);
        break;
    case RXDMA_CFG:
        updateMasks();
        if ((macregs[MAC_RXCFG >> 2] & MAC_RXCFG_ENAB) != 0 &&
            (rxdmaregs[RXDMA_CFG >> 2] & RXDMA_CFG_ENABLE) != 0) {
            qemu_flush_queued_packets(qemu_get_queue(nic));
        }
        break;
    }
}

uint64_t SunGEMState::mmioRxdmaReadCb(void *opaque, hwaddr addr, unsigned size)
{
    return static_cast<SunGEMState *>(opaque)->mmioRxdmaRead(addr, size);
}

uint64_t SunGEMState::mmioRxdmaRead(hwaddr addr, unsigned size)
{
    uint32_t val;

    if (!(addr <= 0x28) && !(addr >= 0x100 && addr <= 0x120)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown RXDMA register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }

    val = rxdmaregs[addr >> 2];

    trace_sungem_mmio_rxdma_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_rxdma_ops = {
    .read = SunGEMState::mmioRxdmaReadCb,
    .write = SunGEMState::mmioRxdmaWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::mmioWolWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    trace_sungem_mmio_wol_write(addr, val);

    switch (addr) {
    case WOL_WAKECSR:
        if (val != 0) {
            qemu_log_mask(LOG_UNIMP, "sungem: WOL not supported\n");
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sungem: WOL not supported\n");
    }
}

uint64_t SunGEMState::mmioWolReadCb(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t val = -1;

    qemu_log_mask(LOG_UNIMP, "sungem: WOL not supported\n");

    trace_sungem_mmio_wol_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_wol_ops = {
    .read = SunGEMState::mmioWolReadCb,
    .write = SunGEMState::mmioWolWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::mmioMacWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    static_cast<SunGEMState *>(opaque)->mmioMacWrite(addr, val, size);
}

void SunGEMState::mmioMacWrite(hwaddr addr, uint64_t val, unsigned size)
{

    if (!(addr <= 0x134)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown MAC register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }

    trace_sungem_mmio_mac_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case MAC_TXRST: /* Not technically read-only but will do for now */
    case MAC_RXRST: /* Not technically read-only but will do for now */
    case MAC_TXSTAT:
    case MAC_RXSTAT:
    case MAC_CSTAT:
    case MAC_PATMPS:
    case MAC_SMACHINE:
        return; /* No actual write */
    case MAC_MINFSZ:
        /* 10-bits implemented */
        val &= 0x3ff;
        break;
    }

    macregs[addr >> 2] = val;

    /* Post write action */
    switch (addr) {
    case MAC_TXMASK:
    case MAC_RXMASK:
    case MAC_MCMASK:
        evalCascadeIrq();
        break;
    case MAC_RXCFG:
        updateMasks();
        if ((macregs[MAC_RXCFG >> 2] & MAC_RXCFG_ENAB) != 0 &&
            (rxdmaregs[RXDMA_CFG >> 2] & RXDMA_CFG_ENABLE) != 0) {
            qemu_flush_queued_packets(qemu_get_queue(nic));
        }
        break;
    }
}

uint64_t SunGEMState::mmioMacReadCb(void *opaque, hwaddr addr, unsigned size)
{
    return static_cast<SunGEMState *>(opaque)->mmioMacRead(addr, size);
}

uint64_t SunGEMState::mmioMacRead(hwaddr addr, unsigned size)
{
    uint32_t val;

    if (!(addr <= 0x134)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown MAC register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }

    val = macregs[addr >> 2];

    trace_sungem_mmio_mac_read(addr, val);

    switch (addr) {
    case MAC_TXSTAT:
        /* Side effect, clear all */
        macregs[addr >> 2] = 0;
        updateStatus(GREG_STAT_TXMAC, false);
        break;
    case MAC_RXSTAT:
        /* Side effect, clear all */
        macregs[addr >> 2] = 0;
        updateStatus(GREG_STAT_RXMAC, false);
        break;
    case MAC_CSTAT:
        /* Side effect, interrupt bits */
        macregs[addr >> 2] &= MAC_CSTAT_PTR;
        updateStatus(GREG_STAT_MAC, false);
        break;
    }

    return val;
}

static const MemoryRegionOps sungem_mmio_mac_ops = {
    .read = SunGEMState::mmioMacReadCb,
    .write = SunGEMState::mmioMacWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::mmioMifWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    static_cast<SunGEMState *>(opaque)->mmioMifWrite(addr, val, size);
}

void SunGEMState::mmioMifWrite(hwaddr addr, uint64_t val, unsigned size)
{

    if (!(addr <= 0x1c)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown MIF register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }

    trace_sungem_mmio_mif_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case MIF_STATUS:
    case MIF_SMACHINE:
        return; /* No actual write */
    case MIF_CFG:
        /* Maintain the RO MDI bits to advertise an MDIO PHY on MDI0 */
        val &= ~MIF_CFG_MDI1;
        val |= MIF_CFG_MDI0;
        break;
    }

    mifregs[addr >> 2] = val;

    /* Post write action */
    switch (addr) {
    case MIF_FRAME:
        mifregs[addr >> 2] = miiOp(val);
        break;
    }
}

uint64_t SunGEMState::mmioMifReadCb(void *opaque, hwaddr addr, unsigned size)
{
    return static_cast<SunGEMState *>(opaque)->mmioMifRead(addr, size);
}

uint64_t SunGEMState::mmioMifRead(hwaddr addr, unsigned size)
{
    uint32_t val;

    if (!(addr <= 0x1c)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown MIF register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }

    val = mifregs[addr >> 2];

    trace_sungem_mmio_mif_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_mif_ops = {
    .read = SunGEMState::mmioMifReadCb,
    .write = SunGEMState::mmioMifWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::mmioPcsWriteCb(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    static_cast<SunGEMState *>(opaque)->mmioPcsWrite(addr, val, size);
}

void SunGEMState::mmioPcsWrite(hwaddr addr, uint64_t val, unsigned size)
{

    if (!(addr <= 0x18) && !(addr >= 0x50 && addr <= 0x5c)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown PCS register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }

    trace_sungem_mmio_pcs_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case PCS_MIISTAT:
    case PCS_ISTAT:
    case PCS_SSTATE:
        return; /* No actual write */
    }

    pcsregs[addr >> 2] = val;
}

uint64_t SunGEMState::mmioPcsReadCb(void *opaque, hwaddr addr, unsigned size)
{
    return static_cast<SunGEMState *>(opaque)->mmioPcsRead(addr, size);
}

uint64_t SunGEMState::mmioPcsRead(hwaddr addr, unsigned size)
{
    uint32_t val;

    if (!(addr <= 0x18) && !(addr >= 0x50 && addr <= 0x5c)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown PCS register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }

    val = pcsregs[addr >> 2];

    trace_sungem_mmio_pcs_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_pcs_ops = {
    .read = SunGEMState::mmioPcsReadCb,
    .write = SunGEMState::mmioPcsWriteCb,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 0,
        .max_access_size = 0,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void SunGEMState::uninit(PCIDevice *dev)
{
    SunGEMState *s = reinterpret_cast<SunGEMState *>(dev);

    qemu_del_nic(s->nic);
}

static NetClientInfo net_sungem_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = SunGEMState::receiveCb,
    .can_receive = SunGEMState::canReceiveCb,
    .link_status_changed = SunGEMState::setLinkStatusCb,
};

void SunGEMState::realizeWrapper(PCIDevice *pci_dev, Error **errp)
{
    SunGEMState *s = reinterpret_cast<SunGEMState *>(pci_dev);
    s->realize(pci_dev, errp);
}

void SunGEMState::realize(PCIDevice *pci_dev, Error **errp)
{
    DeviceState *dev = reinterpret_cast<DeviceState *>(pci_dev);
    uint8_t *pci_conf;

    pci_conf = pci_dev->config;

    pci_set_word(pci_conf + PCI_STATUS,
                 PCI_STATUS_FAST_BACK |
                 PCI_STATUS_DEVSEL_MEDIUM |
                 PCI_STATUS_66MHZ);

    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, 0x0);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, 0x0);

    pci_conf[PCI_INTERRUPT_PIN] = 1; /* interrupt pin A */
    pci_conf[PCI_MIN_GNT] = 0x40;
    pci_conf[PCI_MAX_LAT] = 0x40;

    resetAll(true);
    memory_region_init(&sungem, reinterpret_cast<Object *>(this), "sungem", SUNGEM_MMIO_SIZE);

    memory_region_init_io(&greg, reinterpret_cast<Object *>(this), &sungem_mmio_greg_ops, this,
                          "sungem.greg", SUNGEM_MMIO_GREG_SIZE);
    memory_region_add_subregion(&sungem, 0, &greg);

    memory_region_init_io(&txdma, reinterpret_cast<Object *>(this), &sungem_mmio_txdma_ops, this,
                          "sungem.txdma", SUNGEM_MMIO_TXDMA_SIZE);
    memory_region_add_subregion(&sungem, 0x2000, &txdma);

    memory_region_init_io(&rxdma, reinterpret_cast<Object *>(this), &sungem_mmio_rxdma_ops, this,
                          "sungem.rxdma", SUNGEM_MMIO_RXDMA_SIZE);
    memory_region_add_subregion(&sungem, 0x4000, &rxdma);

    memory_region_init_io(&wol, reinterpret_cast<Object *>(this), &sungem_mmio_wol_ops, this,
                          "sungem.wol", SUNGEM_MMIO_WOL_SIZE);
    memory_region_add_subregion(&sungem, 0x3000, &wol);

    memory_region_init_io(&mac, reinterpret_cast<Object *>(this), &sungem_mmio_mac_ops, this,
                          "sungem.mac", SUNGEM_MMIO_MAC_SIZE);
    memory_region_add_subregion(&sungem, 0x6000, &mac);

    memory_region_init_io(&mif, reinterpret_cast<Object *>(this), &sungem_mmio_mif_ops, this,
                          "sungem.mif", SUNGEM_MMIO_MIF_SIZE);
    memory_region_add_subregion(&sungem, 0x6200, &mif);

    memory_region_init_io(&pcs, reinterpret_cast<Object *>(this), &sungem_mmio_pcs_ops, this,
                          "sungem.pcs", SUNGEM_MMIO_PCS_SIZE);
    memory_region_add_subregion(&sungem, 0x9000, &pcs);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &sungem);

    qemu_macaddr_default_if_unset(&conf.macaddr);
    nic = qemu_new_nic(&net_sungem_info, &conf,
                          object_get_typename(reinterpret_cast<Object *>(dev)),
                          dev->id, &dev->mem_reentrancy_guard, this);
    qemu_format_nic_info_str(qemu_get_queue(nic),
                             conf.macaddr.a);
}

void SunGEMState::resetWrapper(DeviceState *dev)
{
    SunGEMState *s = reinterpret_cast<SunGEMState *>(dev);
    s->reset();
}

void SunGEMState::reset()
{
    resetAll(true);
}

void SunGEMState::init()
{
    device_add_bootindex_property(reinterpret_cast<Object *>(this), &conf.bootindex,
                                  "bootindex", "/ethernet-phy@0",
                                  reinterpret_cast<DeviceState *>(this));
}

static const Property sungem_properties[] = {
    DEFINE_NIC_PROPERTIES(SunGEMState, conf),
    /* Phy address should be 0 for most Apple machines except
     * for K2 in which case it's 1. Will be set by a machine
     * override.
     */
    DEFINE_PROP_UINT32("phy_addr", SunGEMState, phy_addr, 0),
};

static const VMStateField vmstate_sungem_fields[] = {
    VMSTATE_PCI_DEVICE(pdev, SunGEMState),
    VMSTATE_MACADDR(conf.macaddr, SunGEMState),
    VMSTATE_UINT32(phy_addr, SunGEMState),
    VMSTATE_UINT32_ARRAY(gregs, SunGEMState, (SUNGEM_MMIO_GREG_SIZE >> 2)),
    VMSTATE_UINT32_ARRAY(txdmaregs, SunGEMState,
                         (SUNGEM_MMIO_TXDMA_SIZE >> 2)),
    VMSTATE_UINT32_ARRAY(rxdmaregs, SunGEMState,
                         (SUNGEM_MMIO_RXDMA_SIZE >> 2)),
    VMSTATE_UINT32_ARRAY(macregs, SunGEMState, (SUNGEM_MMIO_MAC_SIZE >> 2)),
    VMSTATE_UINT32_ARRAY(mifregs, SunGEMState, (SUNGEM_MMIO_MIF_SIZE >> 2)),
    VMSTATE_UINT32_ARRAY(pcsregs, SunGEMState, (SUNGEM_MMIO_PCS_SIZE >> 2)),
    VMSTATE_UINT32(rx_mask, SunGEMState),
    VMSTATE_UINT32(tx_mask, SunGEMState),
    VMSTATE_UINT8_ARRAY(tx_data, SunGEMState, MAX_PACKET_SIZE),
    VMSTATE_UINT32(tx_size, SunGEMState),
    VMSTATE_UINT64(tx_first_ctl, SunGEMState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_sungem = {
    .name = "sungem",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = vmstate_sungem_fields,
};

void SunGEMState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->realize = SunGEMState::realizeWrapper;
    k->exit = SunGEMState::uninit;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_GMAC;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->vmsd = &vmstate_sungem;
    device_class_set_legacy_reset(dc, SunGEMState::resetWrapper);
    device_class_set_props(dc, sungem_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const InterfaceInfo sungem_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { }
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_IFACES(SunGEMState, TYPE_SUNGEM,
                             TYPE_PCI_DEVICE, sungem_interfaces)
