/*
 * QEMU e1000 emulation
 *
 * Software developer's manual:
 * http://download.intel.com/design/network/manuals/8254x_GBe_SDM.pdf
 *
 * Nir Peleg, Tutis Systems Ltd. for Qumranet Inc.
 * Copyright (c) 2008 Qumranet
 * Based on work done by:
 * Copyright (c) 2007 Dan Aloni
 * Copyright (c) 2004 Antony T Curtis
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */


#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/net/mii.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/eth.h"
#include "net/net.h"
#include "net/checksum.h"
#include "system/system.h"
#include "system/dma.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qemu/range.h"

#include "e1000_common.h"
#include "e1000x_common.h"
#include "trace.h"
#include "qom/object.h"

/* #define E1000_DEBUG */

#ifdef E1000_DEBUG
enum {
    DEBUG_GENERAL,      DEBUG_IO,       DEBUG_MMIO,     DEBUG_INTERRUPT,
    DEBUG_RX,           DEBUG_TX,       DEBUG_MDIC,     DEBUG_EEPROM,
    DEBUG_UNKNOWN,      DEBUG_TXSUM,    DEBUG_TXERR,    DEBUG_RXERR,
    DEBUG_RXFILTER,     DEBUG_PHY,      DEBUG_NOTYET,
};
#define DBGBIT(x)    (1<<DEBUG_##x)
static int debugflags = DBGBIT(TXERR) | DBGBIT(GENERAL);

#define DBGOUT(what, fmt, ...) do { \
    if (debugflags & DBGBIT(what)) \
        fprintf(stderr, "e1000: " fmt, ## __VA_ARGS__); \
    } while (0)
#else
#define DBGOUT(what, fmt, ...) do {} while (0)
#endif

#define IOPORT_SIZE       0x40
#define PNPMMIO_SIZE      0x20000

#define MAXIMUM_ETHERNET_HDR_LEN (ETH_HLEN + 4)

/*
 * HW models:
 *  E1000_DEV_ID_82540EM works with Windows, Linux, and OS X <= 10.8
 *  E1000_DEV_ID_82544GC_COPPER appears to work; not well tested
 *  E1000_DEV_ID_82545EM_COPPER works with Linux and OS X >= 10.6
 *  Others never tested
 */

struct E1000State_st {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    NICState *nic;
    NICConf conf;
    MemoryRegion mmio;
    MemoryRegion io;

    uint32_t mac_reg[0x8000];
    uint16_t phy_reg[0x20];
    uint16_t eeprom_data[64];

    uint32_t rxbuf_size;
    uint32_t rxbuf_min_shift;
    struct e1000_tx {
        unsigned char header[256];
        unsigned char vlan_header[4];
        /* Fields vlan and data must not be reordered or separated. */
        unsigned char vlan[4];
        unsigned char data[0x10000];
        uint16_t size;
        unsigned char vlan_needed;
        unsigned char sum_needed;
        bool cptse;
        e1000x_txd_props props;
        e1000x_txd_props tso_props;
        uint16_t tso_frames;
        bool busy;
    } tx;

    struct {
        uint32_t val_in;    /* shifted in from guest driver */
        uint16_t bitnum_in;
        uint16_t bitnum_out;
        uint16_t reading;
        uint32_t old_eecd;
    } eecd_state;

    QEMUTimer *autoneg_timer;

    QEMUTimer *mit_timer;      /* Mitigation timer. */
    bool mit_timer_on;         /* Mitigation timer is running. */
    bool mit_irq_level;        /* Tracks interrupt pin level. */
    uint32_t mit_ide;          /* Tracks E1000_TXD_CMD_IDE bit. */

    QEMUTimer *flush_queue_timer;

/* Compatibility flags for migration to/from qemu 1.3.0 and older */
#define E1000_FLAG_TSO_BIT 3
#define E1000_FLAG_VET_BIT 4
#define E1000_FLAG_TSO (1 << E1000_FLAG_TSO_BIT)
#define E1000_FLAG_VET (1 << E1000_FLAG_VET_BIT)

    uint32_t compat_flags;
    bool received_tx_tso;
    bool use_tso_for_migration;
    e1000x_txd_props mig_props;

    /* instance methods */
    void linkUp();
    void autonegDone();
    bool haveAutoneg();
    void resetHoldImpl(ResetType type);
    void realizeImpl(Error **errp);

    /* static callback wrappers */
    static void resetHoldStatic(Object *obj, ResetType type);
    static void realizeStatic(PCIDevice *pci_dev, Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
    static void instanceInit(Object *obj);
};
typedef struct E1000State_st E1000State;

#define chkflag(x)     (s->compat_flags & E1000_FLAG_##x)

struct E1000BaseClass {
    PCIDeviceClass parent_class;
    uint16_t phy_id2;
};
typedef struct E1000BaseClass E1000BaseClass;

#define TYPE_E1000_BASE "e1000-base"

DECLARE_OBJ_CHECKERS(E1000State, E1000BaseClass,
                     E1000, TYPE_E1000_BASE)


void E1000State_st::linkUp()
{
    e1000x_update_regs_on_link_up(mac_reg, phy_reg);

    /* E1000_STATUS_LU is tested by e1000_can_receive() */
    qemu_flush_queued_packets(qemu_get_queue(nic));
}

void E1000State_st::autonegDone()
{
    e1000x_update_regs_on_autoneg_done(mac_reg, phy_reg);

    /* E1000_STATUS_LU is tested by e1000_can_receive() */
    qemu_flush_queued_packets(qemu_get_queue(nic));
}

bool E1000State_st::haveAutoneg()
{
    return (phy_reg[MII_BMCR] & MII_BMCR_AUTOEN);
}

/* Free-function wrappers for legacy call sites */
static void
e1000_link_up(E1000State *s)
{
    s->linkUp();
}

static void
e1000_autoneg_done(E1000State *s)
{
    s->autonegDone();
}

static bool
have_autoneg(E1000State *s)
{
    return s->haveAutoneg();
}

static void
set_phy_ctrl(E1000State *s, int index, uint16_t val)
{
    /* bits 0-5 reserved; MII_BMCR_[ANRESTART,RESET] are self clearing */
    s->phy_reg[MII_BMCR] = val & ~(0x3f |
                                   MII_BMCR_RESET |
                                   MII_BMCR_ANRESTART);

    /*
     * QEMU 1.3 does not support link auto-negotiation emulation, so if we
     * migrate during auto negotiation, after migration the link will be
     * down.
     */
    if (have_autoneg(s) && (val & MII_BMCR_ANRESTART)) {
        e1000x_restart_autoneg(s->mac_reg, s->phy_reg, s->autoneg_timer);
    }
}

static void (*phyreg_writeops[])(E1000State *, int, uint16_t) = {
    [MII_BMCR] = set_phy_ctrl,
};

enum { NPHYWRITEOPS = ARRAY_SIZE(phyreg_writeops) };

enum { PHY_R = 1, PHY_W = 2, PHY_RW = PHY_R | PHY_W };
static char phy_regcap[0x20];
static uint16_t phy_reg_init[0x20];
static uint32_t mac_reg_init[0x8000];

static void __attribute__((constructor)) e1000_init_phy_mac_tables(void)
{
    /* phy_regcap */
    phy_regcap[MII_BMSR] = PHY_R;
    phy_regcap[M88E1000_EXT_PHY_SPEC_CTRL] = PHY_RW;
    phy_regcap[MII_PHYID1] = PHY_R;
    phy_regcap[M88E1000_PHY_SPEC_CTRL] = PHY_RW;
    phy_regcap[MII_BMCR] = PHY_RW;
    phy_regcap[MII_CTRL1000] = PHY_RW;
    phy_regcap[MII_ANLPAR] = PHY_R;
    phy_regcap[MII_STAT1000] = PHY_R;
    phy_regcap[MII_ANAR] = PHY_RW;
    phy_regcap[M88E1000_RX_ERR_CNTR] = PHY_R;
    phy_regcap[MII_PHYID2] = PHY_R;
    phy_regcap[M88E1000_PHY_SPEC_STATUS] = PHY_R;
    phy_regcap[MII_ANER] = PHY_R;

    /* MII_PHYID2 documented in 8254x_GBe_SDM.pdf, pp. 250 */
    /* phy_reg_init */
    phy_reg_init[MII_BMCR] = MII_BMCR_SPEED1000 |
                              MII_BMCR_FD |
                              MII_BMCR_AUTOEN;
    phy_reg_init[MII_BMSR] = MII_BMSR_EXTCAP |
                              MII_BMSR_LINK_ST |
                              MII_BMSR_AUTONEG |
                              MII_BMSR_MFPS |
                              MII_BMSR_EXTSTAT |
                              MII_BMSR_10T_HD |
                              MII_BMSR_10T_FD |
                              MII_BMSR_100TX_HD |
                              MII_BMSR_100TX_FD;
    phy_reg_init[MII_PHYID1] = 0x141;
    /* [MII_PHYID2] configured per DevId, from e1000_reset() */
    phy_reg_init[MII_ANAR] = MII_ANAR_CSMACD | MII_ANAR_10 |
                              MII_ANAR_10FD | MII_ANAR_TX |
                              MII_ANAR_TXFD | MII_ANAR_PAUSE |
                              MII_ANAR_PAUSE_ASYM;
    phy_reg_init[MII_ANLPAR] = MII_ANLPAR_10 | MII_ANLPAR_10FD |
                                MII_ANLPAR_TX | MII_ANLPAR_TXFD;
    phy_reg_init[MII_CTRL1000] = MII_CTRL1000_FULL | MII_CTRL1000_PORT |
                                  MII_CTRL1000_MASTER;
    phy_reg_init[MII_STAT1000] = MII_STAT1000_HALF | MII_STAT1000_FULL |
                                  MII_STAT1000_ROK | MII_STAT1000_LOK;
    phy_reg_init[M88E1000_PHY_SPEC_CTRL] = 0x360;
    phy_reg_init[M88E1000_PHY_SPEC_STATUS] = 0xac00;
    phy_reg_init[M88E1000_EXT_PHY_SPEC_CTRL] = 0x0d60;

    /* mac_reg_init */
    mac_reg_init[PBA] = 0x00100030;
    mac_reg_init[LEDCTL] = 0x602;
    mac_reg_init[CTRL] = E1000_CTRL_SWDPIN2 | E1000_CTRL_SWDPIN0 |
                          E1000_CTRL_SPD_1000 | E1000_CTRL_SLU;
    mac_reg_init[STATUS] = 0x80000000 | E1000_STATUS_GIO_MASTER_ENABLE |
                            E1000_STATUS_ASDV | E1000_STATUS_MTXCKOK |
                            E1000_STATUS_SPEED_1000 | E1000_STATUS_FD |
                            E1000_STATUS_LU;
    mac_reg_init[MANC] = E1000_MANC_EN_MNG2HOST | E1000_MANC_RCV_TCO_EN |
                          E1000_MANC_ARP_EN | E1000_MANC_0298_EN |
                          E1000_MANC_RMCP_EN;
}

/* Helper function, *curr == 0 means the value is not set */
static inline void
mit_update_delay(uint32_t *curr, uint32_t value)
{
    if (value && (*curr == 0 || value < *curr)) {
        *curr = value;
    }
}

static void
set_interrupt_cause(E1000State *s, int index, uint32_t val)
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(s);
    uint32_t pending_ints;
    uint32_t mit_delay;

    s->mac_reg[ICR] = val;

    /*
     * Make sure ICR and ICS registers have the same value.
     * The spec says that the ICS register is write-only.  However in practice,
     * on real hardware ICS is readable, and for reads it has the same value as
     * ICR (except that ICS does not have the clear on read behaviour of ICR).
     *
     * The VxWorks PRO/1000 driver uses this behaviour.
     */
    s->mac_reg[ICS] = val;

    pending_ints = (s->mac_reg[IMS] & s->mac_reg[ICR]);
    if (!s->mit_irq_level && pending_ints) {
        /*
         * Here we detect a potential raising edge. We postpone raising the
         * interrupt line if we are inside the mitigation delay window
         * (s->mit_timer_on == 1).
         * We provide a partial implementation of interrupt mitigation,
         * emulating only RADV, TADV and ITR (lower 16 bits, 1024ns units for
         * RADV and TADV, 256ns units for ITR). RDTR is only used to enable
         * RADV; relative timers based on TIDV and RDTR are not implemented.
         */
        if (s->mit_timer_on) {
            return;
        }

        /* Compute the next mitigation delay according to pending
         * interrupts and the current values of RADV (provided
         * RDTR!=0), TADV and ITR.
         * Then rearm the timer.
         */
        mit_delay = 0;
        if (s->mit_ide &&
                (pending_ints & (E1000_ICR_TXQE | E1000_ICR_TXDW))) {
            mit_update_delay(&mit_delay, s->mac_reg[TADV] * 4);
        }
        if (s->mac_reg[RDTR] && (pending_ints & E1000_ICS_RXT0)) {
            mit_update_delay(&mit_delay, s->mac_reg[RADV] * 4);
        }
        mit_update_delay(&mit_delay, s->mac_reg[ITR]);

        /*
         * According to e1000 SPEC, the Ethernet controller guarantees
         * a maximum observable interrupt rate of 7813 interrupts/sec.
         * Thus if mit_delay < 500 then the delay should be set to the
         * minimum delay possible which is 500.
         */
        mit_delay = (mit_delay < 500) ? 500 : mit_delay;

        s->mit_timer_on = 1;
        timer_mod(s->mit_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  mit_delay * 256);
        s->mit_ide = 0;
    }

    s->mit_irq_level = (pending_ints != 0);
    pci_set_irq(d, s->mit_irq_level);
}

static void
e1000_mit_timer(void *opaque)
{
    E1000State *s = static_cast<E1000State *>(opaque);

    s->mit_timer_on = 0;
    /* Call set_interrupt_cause to update the irq level (if necessary). */
    set_interrupt_cause(s, 0, s->mac_reg[ICR]);
}

static void
set_ics(E1000State *s, int index, uint32_t val)
{
    DBGOUT(INTERRUPT, "set_ics %x, ICR %x, IMR %x\n", val, s->mac_reg[ICR],
        s->mac_reg[IMS]);
    set_interrupt_cause(s, 0, val | s->mac_reg[ICR]);
}

static void
e1000_autoneg_timer(void *opaque)
{
    E1000State *s = static_cast<E1000State *>(opaque);
    if (!qemu_get_queue(s->nic)->link_down) {
        e1000_autoneg_done(s);
        set_ics(s, 0, E1000_ICS_LSC); /* signal link status change to guest */
    }
}

static bool e1000_vet_init_need(void *opaque)
{
    E1000State *s = static_cast<E1000State *>(opaque);

    return chkflag(VET);
}

void E1000State_st::resetHoldImpl(ResetType type)
{
    E1000BaseClass *edc = E1000_GET_CLASS(this);
    uint8_t *macaddr = conf.macaddr.a;

    timer_del(autoneg_timer);
    timer_del(mit_timer);
    timer_del(flush_queue_timer);
    mit_timer_on = 0;
    mit_irq_level = 0;
    mit_ide = 0;
    memset(phy_reg, 0, sizeof phy_reg);
    memcpy(phy_reg, phy_reg_init, sizeof phy_reg_init);
    phy_reg[MII_PHYID2] = edc->phy_id2;
    memset(mac_reg, 0, sizeof mac_reg);
    memcpy(mac_reg, mac_reg_init, sizeof mac_reg_init);
    rxbuf_min_shift = 1;
    memset(&tx, 0, sizeof tx);

    if (qemu_get_queue(nic)->link_down) {
        e1000x_update_regs_on_link_down(mac_reg, phy_reg);
    }

    e1000x_reset_mac_addr(nic, mac_reg, macaddr);

    if (e1000_vet_init_need(this)) {
        mac_reg[VET] = ETH_P_VLAN;
    }
}

/* static wrapper */
void E1000State_st::resetHoldStatic(Object *obj, ResetType type)
{
    E1000State *d = reinterpret_cast<E1000State *>(obj);
    d->resetHoldImpl(type);
}

static void
set_ctrl(E1000State *s, int index, uint32_t val)
{
    /* RST is self clearing */
    s->mac_reg[CTRL] = val & ~E1000_CTRL_RST;
}

static void
e1000_flush_queue_timer(void *opaque)
{
    E1000State *s = static_cast<E1000State *>(opaque);

    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static void
set_rx_control(E1000State *s, int index, uint32_t val)
{
    s->mac_reg[RCTL] = val;
    s->rxbuf_size = e1000x_rxbufsize(val);
    s->rxbuf_min_shift = ((val / E1000_RCTL_RDMTS_QUAT) & 3) + 1;
    DBGOUT(RX, "RCTL: %d, mac_reg[RCTL] = 0x%x\n", s->mac_reg[RDT],
           s->mac_reg[RCTL]);
    timer_mod(s->flush_queue_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void
set_mdic(E1000State *s, int index, uint32_t val)
{
    uint32_t data = val & E1000_MDIC_DATA_MASK;
    uint32_t addr = ((val & E1000_MDIC_REG_MASK) >> E1000_MDIC_REG_SHIFT);

    if ((val & E1000_MDIC_PHY_MASK) >> E1000_MDIC_PHY_SHIFT != 1) // phy #
        val = s->mac_reg[MDIC] | E1000_MDIC_ERROR;
    else if (val & E1000_MDIC_OP_READ) {
        DBGOUT(MDIC, "MDIC read reg 0x%x\n", addr);
        if (!(phy_regcap[addr] & PHY_R)) {
            DBGOUT(MDIC, "MDIC read reg %x unhandled\n", addr);
            val |= E1000_MDIC_ERROR;
        } else
            val = (val ^ data) | s->phy_reg[addr];
    } else if (val & E1000_MDIC_OP_WRITE) {
        DBGOUT(MDIC, "MDIC write reg 0x%x, value 0x%x\n", addr, data);
        if (!(phy_regcap[addr] & PHY_W)) {
            DBGOUT(MDIC, "MDIC write reg %x unhandled\n", addr);
            val |= E1000_MDIC_ERROR;
        } else {
            if (addr < NPHYWRITEOPS && phyreg_writeops[addr]) {
                phyreg_writeops[addr](s, index, data);
            } else {
                s->phy_reg[addr] = data;
            }
        }
    }
    s->mac_reg[MDIC] = val | E1000_MDIC_READY;

    if (val & E1000_MDIC_INT_EN) {
        set_ics(s, 0, E1000_ICR_MDAC);
    }
}

static uint32_t
get_eecd(E1000State *s, int index)
{
    uint32_t ret = E1000_EECD_PRES|E1000_EECD_GNT | s->eecd_state.old_eecd;

    DBGOUT(EEPROM, "reading eeprom bit %d (reading %d)\n",
           s->eecd_state.bitnum_out, s->eecd_state.reading);
    if (!s->eecd_state.reading ||
        ((s->eeprom_data[(s->eecd_state.bitnum_out >> 4) & 0x3f] >>
          ((s->eecd_state.bitnum_out & 0xf) ^ 0xf))) & 1)
        ret |= E1000_EECD_DO;
    return ret;
}

static void
set_eecd(E1000State *s, int index, uint32_t val)
{
    uint32_t oldval = s->eecd_state.old_eecd;

    s->eecd_state.old_eecd = val & (E1000_EECD_SK | E1000_EECD_CS |
            E1000_EECD_DI|E1000_EECD_FWE_MASK|E1000_EECD_REQ);
    if (!(E1000_EECD_CS & val)) {            /* CS inactive; nothing to do */
        return;
    }
    if (E1000_EECD_CS & (val ^ oldval)) {    /* CS rise edge; reset state */
        s->eecd_state.val_in = 0;
        s->eecd_state.bitnum_in = 0;
        s->eecd_state.bitnum_out = 0;
        s->eecd_state.reading = 0;
    }
    if (!(E1000_EECD_SK & (val ^ oldval))) {    /* no clock edge */
        return;
    }
    if (!(E1000_EECD_SK & val)) {               /* falling edge */
        s->eecd_state.bitnum_out++;
        return;
    }
    s->eecd_state.val_in <<= 1;
    if (val & E1000_EECD_DI)
        s->eecd_state.val_in |= 1;
    if (++s->eecd_state.bitnum_in == 9 && !s->eecd_state.reading) {
        s->eecd_state.bitnum_out = ((s->eecd_state.val_in & 0x3f)<<4)-1;
        s->eecd_state.reading = (((s->eecd_state.val_in >> 6) & 7) ==
            EEPROM_READ_OPCODE_MICROWIRE);
    }
    DBGOUT(EEPROM, "eeprom bitnum in %d out %d, reading %d\n",
           s->eecd_state.bitnum_in, s->eecd_state.bitnum_out,
           s->eecd_state.reading);
}

static uint32_t
flash_eerd_read(E1000State *s, int x)
{
    unsigned int index, r = s->mac_reg[EERD] & ~E1000_EEPROM_RW_REG_START;

    if ((s->mac_reg[EERD] & E1000_EEPROM_RW_REG_START) == 0)
        return (s->mac_reg[EERD]);

    if ((index = r >> E1000_EEPROM_RW_ADDR_SHIFT) > EEPROM_CHECKSUM_REG)
        return (E1000_EEPROM_RW_REG_DONE | r);

    return ((s->eeprom_data[index] << E1000_EEPROM_RW_REG_DATA) |
           E1000_EEPROM_RW_REG_DONE | r);
}

static void
putsum(uint8_t *data, uint32_t n, uint32_t sloc, uint32_t css, uint32_t cse)
{
    uint32_t sum;

    if (cse && cse < n)
        n = cse + 1;
    if (sloc < n-1) {
        sum = net_checksum_add(n-css, data+css);
        stw_be_p(data + sloc, net_checksum_finish_nozero(sum));
    }
}

static inline void
inc_tx_bcast_or_mcast_count(E1000State *s, const unsigned char *arr)
{
    if (is_broadcast_ether_addr(arr)) {
        e1000x_inc_reg_if_not_full(s->mac_reg, BPTC);
    } else if (is_multicast_ether_addr(arr)) {
        e1000x_inc_reg_if_not_full(s->mac_reg, MPTC);
    }
}

static void
e1000_send_packet(E1000State *s, const uint8_t *buf, int size)
{
    static const int PTCregs[6] = { PTC64, PTC127, PTC255, PTC511,
                                    PTC1023, PTC1522 };

    NetClientState *nc = qemu_get_queue(s->nic);
    if (s->phy_reg[MII_BMCR] & MII_BMCR_LOOPBACK) {
        qemu_receive_packet(nc, buf, size);
    } else {
        qemu_send_packet(nc, buf, size);
    }
    inc_tx_bcast_or_mcast_count(s, buf);
    e1000x_increase_size_stats(s->mac_reg, PTCregs, size + 4);
}

static void
xmit_seg(E1000State *s)
{
    uint16_t len;
    unsigned int frames = s->tx.tso_frames, css, sofar;
    E1000State_st::e1000_tx *tp = &s->tx;
    struct e1000x_txd_props *props = tp->cptse ? &tp->tso_props : &tp->props;

    if (tp->cptse) {
        css = props->ipcss;
        DBGOUT(TXSUM, "frames %d size %d ipcss %d\n",
               frames, tp->size, css);
        if (props->ip) {    /* IPv4 */
            stw_be_p(tp->data+css+2, tp->size - css);
            stw_be_p(tp->data+css+4,
                     lduw_be_p(tp->data + css + 4) + frames);
        } else {         /* IPv6 */
            stw_be_p(tp->data+css+4, tp->size - css);
        }
        css = props->tucss;
        len = tp->size - css;
        DBGOUT(TXSUM, "tcp %d tucss %d len %d\n", props->tcp, css, len);
        if (props->tcp) {
            sofar = frames * props->mss;
            stl_be_p(tp->data+css+4, ldl_be_p(tp->data+css+4)+sofar); /* seq */
            if (props->paylen - sofar > props->mss) {
                tp->data[css + 13] &= ~9;    /* PSH, FIN */
            } else if (frames) {
                e1000x_inc_reg_if_not_full(s->mac_reg, TSCTC);
            }
        } else {    /* UDP */
            stw_be_p(tp->data+css+4, len);
        }
        if (tp->sum_needed & E1000_TXD_POPTS_TXSM) {
            unsigned int phsum;
            // add pseudo-header length before checksum calculation
            void *sp = tp->data + props->tucso;

            phsum = lduw_be_p(sp) + len;
            phsum = (phsum >> 16) + (phsum & 0xffff);
            stw_be_p(sp, phsum);
        }
        tp->tso_frames++;
    }

    if (tp->sum_needed & E1000_TXD_POPTS_TXSM) {
        putsum(tp->data, tp->size, props->tucso, props->tucss, props->tucse);
    }
    if (tp->sum_needed & E1000_TXD_POPTS_IXSM) {
        putsum(tp->data, tp->size, props->ipcso, props->ipcss, props->ipcse);
    }
    if (tp->vlan_needed) {
        memmove(tp->vlan, tp->data, 4);
        memmove(tp->data, tp->data + 4, 8);
        memcpy(tp->data + 8, tp->vlan_header, 4);
        e1000_send_packet(s, tp->vlan, tp->size + 4);
    } else {
        e1000_send_packet(s, tp->data, tp->size);
    }

    e1000x_inc_reg_if_not_full(s->mac_reg, TPT);
    e1000x_grow_8reg_if_not_full(s->mac_reg, TOTL, s->tx.size + 4);
    e1000x_inc_reg_if_not_full(s->mac_reg, GPTC);
    e1000x_grow_8reg_if_not_full(s->mac_reg, GOTCL, s->tx.size + 4);
}

static void
process_tx_desc(E1000State *s, struct e1000_tx_desc *dp)
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(s);
    uint32_t txd_lower = le32_to_cpu(dp->lower.data);
    uint32_t dtype = txd_lower & (E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_D);
    unsigned int split_size = txd_lower & 0xffff, bytes, sz;
    unsigned int msh = 0xfffff;
    uint64_t addr;
    struct e1000_context_desc *xp = (struct e1000_context_desc *)dp;
    E1000State_st::e1000_tx *tp = &s->tx;

    s->mit_ide |= (txd_lower & E1000_TXD_CMD_IDE);
    if (dtype == E1000_TXD_CMD_DEXT) {    /* context descriptor */
        if (le32_to_cpu(xp->cmd_and_length) & E1000_TXD_CMD_TSE) {
            e1000x_read_tx_ctx_descr(xp, &tp->tso_props);
            s->use_tso_for_migration = 1;
            tp->tso_frames = 0;
        } else {
            e1000x_read_tx_ctx_descr(xp, &tp->props);
            s->use_tso_for_migration = 0;
        }
        return;
    } else if (dtype == (E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_D)) {
        // data descriptor
        if (tp->size == 0) {
            tp->sum_needed = le32_to_cpu(dp->upper.data) >> 8;
        }
        tp->cptse = (txd_lower & E1000_TXD_CMD_TSE) ? 1 : 0;
    } else {
        // legacy descriptor
        tp->cptse = 0;
    }

    if (e1000x_vlan_enabled(s->mac_reg) &&
        e1000x_is_vlan_txd(txd_lower) &&
        (tp->cptse || txd_lower & E1000_TXD_CMD_EOP)) {
        tp->vlan_needed = 1;
        stw_be_p(tp->vlan_header,
                      le16_to_cpu(s->mac_reg[VET]));
        stw_be_p(tp->vlan_header + 2,
                      le16_to_cpu(dp->upper.fields.special));
    }

    addr = le64_to_cpu(dp->buffer_addr);
    if (tp->cptse) {
        msh = tp->tso_props.hdr_len + tp->tso_props.mss;
        do {
            bytes = split_size;
            if (tp->size >= msh) {
                goto eop;
            }
            if (tp->size + bytes > msh)
                bytes = msh - tp->size;

            bytes = MIN(sizeof(tp->data) - tp->size, bytes);
            pci_dma_read(d, addr, tp->data + tp->size, bytes);
            sz = tp->size + bytes;
            if (sz >= tp->tso_props.hdr_len
                && tp->size < tp->tso_props.hdr_len) {
                memmove(tp->header, tp->data, tp->tso_props.hdr_len);
            }
            tp->size = sz;
            addr += bytes;
            if (sz == msh) {
                xmit_seg(s);
                memmove(tp->data, tp->header, tp->tso_props.hdr_len);
                tp->size = tp->tso_props.hdr_len;
            }
            split_size -= bytes;
        } while (bytes && split_size);
    } else {
        split_size = MIN(sizeof(tp->data) - tp->size, split_size);
        pci_dma_read(d, addr, tp->data + tp->size, split_size);
        tp->size += split_size;
    }

eop:
    if (!(txd_lower & E1000_TXD_CMD_EOP))
        return;
    if (!(tp->cptse && tp->size < tp->tso_props.hdr_len)) {
        xmit_seg(s);
    }
    tp->tso_frames = 0;
    tp->sum_needed = 0;
    tp->vlan_needed = 0;
    tp->size = 0;
    tp->cptse = 0;
}

static uint32_t
txdesc_writeback(E1000State *s, dma_addr_t base, struct e1000_tx_desc *dp)
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(s);
    uint32_t txd_upper, txd_lower = le32_to_cpu(dp->lower.data);

    if (!(txd_lower & (E1000_TXD_CMD_RS|E1000_TXD_CMD_RPS)))
        return 0;
    txd_upper = (le32_to_cpu(dp->upper.data) | E1000_TXD_STAT_DD) &
                ~(E1000_TXD_STAT_EC | E1000_TXD_STAT_LC | E1000_TXD_STAT_TU);
    dp->upper.data = cpu_to_le32(txd_upper);
    pci_dma_write(d, base + ((char *)&dp->upper - (char *)dp),
                  &dp->upper, sizeof(dp->upper));
    return E1000_ICR_TXDW;
}

static uint64_t tx_desc_base(E1000State *s)
{
    uint64_t bah = s->mac_reg[TDBAH];
    uint64_t bal = s->mac_reg[TDBAL] & ~0xf;

    return (bah << 32) + bal;
}

static void
start_xmit(E1000State *s)
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(s);
    dma_addr_t base;
    struct e1000_tx_desc desc;
    uint32_t tdh_start = s->mac_reg[TDH], cause = E1000_ICS_TXQE;

    if (!(s->mac_reg[TCTL] & E1000_TCTL_EN)) {
        DBGOUT(TX, "tx disabled\n");
        return;
    }

    if (s->tx.busy) {
        return;
    }
    s->tx.busy = true;

    while (s->mac_reg[TDH] != s->mac_reg[TDT]) {
        base = tx_desc_base(s) +
               sizeof(struct e1000_tx_desc) * s->mac_reg[TDH];
        pci_dma_read(d, base, &desc, sizeof(desc));

        DBGOUT(TX, "index %d: %p : %x %x\n", s->mac_reg[TDH],
               reinterpret_cast<void *>(static_cast<intptr_t>(desc.buffer_addr)), desc.lower.data,
               desc.upper.data);

        process_tx_desc(s, &desc);
        cause |= txdesc_writeback(s, base, &desc);

        if (++s->mac_reg[TDH] * sizeof(desc) >= s->mac_reg[TDLEN])
            s->mac_reg[TDH] = 0;
        /*
         * the following could happen only if guest sw assigns
         * bogus values to TDT/TDLEN.
         * there's nothing too intelligent we could do about this.
         */
        if (s->mac_reg[TDH] == tdh_start ||
            tdh_start >= s->mac_reg[TDLEN] / sizeof(desc)) {
            DBGOUT(TXERR, "TDH wraparound @%x, TDT %x, TDLEN %x\n",
                   tdh_start, s->mac_reg[TDT], s->mac_reg[TDLEN]);
            break;
        }
    }
    s->tx.busy = false;
    set_ics(s, 0, cause);
}

static int
receive_filter(E1000State *s, const void *buf)
{
    return (!e1000x_is_vlan_packet(buf, s->mac_reg[VET]) ||
            e1000x_rx_vlan_filter(s->mac_reg, PKT_GET_VLAN_HDR(buf))) &&
           e1000x_rx_group_filter(s->mac_reg, static_cast<const struct eth_header *>(buf));
}

static void
e1000_set_link_status(NetClientState *nc)
{
    E1000State *s = static_cast<E1000State *>(qemu_get_nic_opaque(nc));
    uint32_t old_status = s->mac_reg[STATUS];

    if (nc->link_down) {
        e1000x_update_regs_on_link_down(s->mac_reg, s->phy_reg);
    } else {
        if (have_autoneg(s) &&
            !(s->phy_reg[MII_BMSR] & MII_BMSR_AN_COMP)) {
            e1000x_restart_autoneg(s->mac_reg, s->phy_reg, s->autoneg_timer);
        } else {
            e1000_link_up(s);
        }
    }

    if (s->mac_reg[STATUS] != old_status)
        set_ics(s, 0, E1000_ICR_LSC);
}

static bool e1000_has_rxbufs(E1000State *s, size_t total_size)
{
    int bufs;
    /* Fast-path short packets */
    if (total_size <= s->rxbuf_size) {
        return s->mac_reg[RDH] != s->mac_reg[RDT];
    }
    if (s->mac_reg[RDH] < s->mac_reg[RDT]) {
        bufs = s->mac_reg[RDT] - s->mac_reg[RDH];
    } else if (s->mac_reg[RDH] > s->mac_reg[RDT]) {
        bufs = s->mac_reg[RDLEN] /  sizeof(struct e1000_rx_desc) +
            s->mac_reg[RDT] - s->mac_reg[RDH];
    } else {
        return false;
    }
    return total_size <= bufs * s->rxbuf_size;
}

static bool
e1000_can_receive(NetClientState *nc)
{
    E1000State *s = static_cast<E1000State *>(qemu_get_nic_opaque(nc));

    return e1000x_rx_ready(&s->parent_obj, s->mac_reg) &&
        e1000_has_rxbufs(s, 1) && !timer_pending(s->flush_queue_timer);
}

static uint64_t rx_desc_base(E1000State *s)
{
    uint64_t bah = s->mac_reg[RDBAH];
    uint64_t bal = s->mac_reg[RDBAL] & ~0xf;

    return (bah << 32) + bal;
}

static void
e1000_receiver_overrun(E1000State *s, size_t size)
{
    trace_e1000_receiver_overrun(size, s->mac_reg[RDH], s->mac_reg[RDT]);
    e1000x_inc_reg_if_not_full(s->mac_reg, RNBC);
    e1000x_inc_reg_if_not_full(s->mac_reg, MPC);
    set_ics(s, 0, E1000_ICS_RXO);
}

static ssize_t
e1000_receive_iov(NetClientState *nc, const struct iovec *iov, int iovcnt)
{
    E1000State *s = static_cast<E1000State *>(qemu_get_nic_opaque(nc));
    PCIDevice *d = reinterpret_cast<PCIDevice *>(s);
    struct e1000_rx_desc desc;
    dma_addr_t base;
    unsigned int n, rdt;
    uint32_t rdh_start;
    uint16_t vlan_special = 0;
    uint8_t vlan_status = 0;
    uint8_t min_buf[ETH_ZLEN];
    uint8_t *filter_buf = static_cast<uint8_t *>(iov->iov_base);
    size_t size = iov_size(iov, iovcnt);
    size_t iov_ofs = 0;
    size_t desc_offset;
    size_t desc_size;
    size_t total_size;
    eth_pkt_types_e pkt_type;

    if (!e1000x_hw_rx_enabled(s->mac_reg)) {
        return -1;
    }

    if (timer_pending(s->flush_queue_timer)) {
        return 0;
    }

    if (iov->iov_len < MAXIMUM_ETHERNET_HDR_LEN) {
        /* This is very unlikely, but may happen. */
        iov_to_buf(iov, iovcnt, 0, min_buf, MAXIMUM_ETHERNET_HDR_LEN);
        filter_buf = min_buf;
    }

    /* Discard oversized packets if !LPE and !SBP. */
    if (e1000x_is_oversized(s->mac_reg, size)) {
        return size;
    }

    if (!receive_filter(s, filter_buf)) {
        return size;
    }

    if (e1000x_vlan_enabled(s->mac_reg) &&
        e1000x_is_vlan_packet(filter_buf, le16_to_cpu(s->mac_reg[VET]))) {
        vlan_special = cpu_to_le16(lduw_be_p(filter_buf + 14));
        iov_ofs = 4;
        if (filter_buf == iov->iov_base) {
            memmove(filter_buf + 4, filter_buf, 12);
        } else {
            iov_from_buf(iov, iovcnt, 4, filter_buf, 12);
            while (iov->iov_len <= iov_ofs) {
                iov_ofs -= iov->iov_len;
                iov++;
            }
        }
        vlan_status = E1000_RXD_STAT_VP;
        size -= 4;
    }

    pkt_type = get_eth_packet_type(PKT_GET_ETH_HDR(filter_buf));
    rdh_start = s->mac_reg[RDH];
    desc_offset = 0;
    total_size = size + e1000x_fcs_len(s->mac_reg);
    if (!e1000_has_rxbufs(s, total_size)) {
        e1000_receiver_overrun(s, total_size);
        return -1;
    }
    do {
        desc_size = total_size - desc_offset;
        if (desc_size > s->rxbuf_size) {
            desc_size = s->rxbuf_size;
        }
        base = rx_desc_base(s) + sizeof(desc) * s->mac_reg[RDH];
        pci_dma_read(d, base, &desc, sizeof(desc));
        desc.special = vlan_special;
        desc.status &= ~E1000_RXD_STAT_DD;
        if (desc.buffer_addr) {
            if (desc_offset < size) {
                size_t iov_copy;
                hwaddr ba = le64_to_cpu(desc.buffer_addr);
                size_t copy_size = size - desc_offset;
                if (copy_size > s->rxbuf_size) {
                    copy_size = s->rxbuf_size;
                }
                do {
                    iov_copy = MIN(copy_size, iov->iov_len - iov_ofs);
                    pci_dma_write(d, ba, static_cast<const char *>(iov->iov_base) + iov_ofs, iov_copy);
                    copy_size -= iov_copy;
                    ba += iov_copy;
                    iov_ofs += iov_copy;
                    if (iov_ofs == iov->iov_len) {
                        iov++;
                        iov_ofs = 0;
                    }
                } while (copy_size);
            }
            desc_offset += desc_size;
            desc.length = cpu_to_le16(desc_size);
            if (desc_offset >= total_size) {
                desc.status |= E1000_RXD_STAT_EOP | E1000_RXD_STAT_IXSM;
            } else {
                /* Guest zeroing out status is not a hardware requirement.
                   Clear EOP in case guest didn't do it. */
                desc.status &= ~E1000_RXD_STAT_EOP;
            }
        } else { // as per intel docs; skip descriptors with null buf addr
            DBGOUT(RX, "Null RX descriptor!!\n");
        }
        pci_dma_write(d, base, &desc, sizeof(desc));
        desc.status |= (vlan_status | E1000_RXD_STAT_DD);
        pci_dma_write(d, base + offsetof(struct e1000_rx_desc, status),
                      &desc.status, sizeof(desc.status));

        if (++s->mac_reg[RDH] * sizeof(desc) >= s->mac_reg[RDLEN])
            s->mac_reg[RDH] = 0;
        /* see comment in start_xmit; same here */
        if (s->mac_reg[RDH] == rdh_start ||
            rdh_start >= s->mac_reg[RDLEN] / sizeof(desc)) {
            DBGOUT(RXERR, "RDH wraparound @%x, RDT %x, RDLEN %x\n",
                   rdh_start, s->mac_reg[RDT], s->mac_reg[RDLEN]);
            e1000_receiver_overrun(s, total_size);
            return -1;
        }
    } while (desc_offset < total_size);

    e1000x_update_rx_total_stats(s->mac_reg, pkt_type, size, total_size);

    n = E1000_ICS_RXT0;
    if ((rdt = s->mac_reg[RDT]) < s->mac_reg[RDH])
        rdt += s->mac_reg[RDLEN] / sizeof(desc);
    if (((rdt - s->mac_reg[RDH]) * sizeof(desc)) <= s->mac_reg[RDLEN] >>
        s->rxbuf_min_shift)
        n |= E1000_ICS_RXDMT0;

    set_ics(s, 0, n);

    return size;
}

static ssize_t
e1000_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    const struct iovec iov = {
        .iov_base = (uint8_t *)buf,
        .iov_len = size
    };

    return e1000_receive_iov(nc, &iov, 1);
}

static uint32_t
mac_readreg(E1000State *s, int index)
{
    return s->mac_reg[index];
}

static uint32_t
mac_icr_read(E1000State *s, int index)
{
    uint32_t ret = s->mac_reg[ICR];

    DBGOUT(INTERRUPT, "ICR read: %x\n", ret);
    set_interrupt_cause(s, 0, 0);
    return ret;
}

static uint32_t
mac_read_clr4(E1000State *s, int index)
{
    uint32_t ret = s->mac_reg[index];

    s->mac_reg[index] = 0;
    return ret;
}

static uint32_t
mac_read_clr8(E1000State *s, int index)
{
    uint32_t ret = s->mac_reg[index];

    s->mac_reg[index] = 0;
    s->mac_reg[index-1] = 0;
    return ret;
}

static void
mac_writereg(E1000State *s, int index, uint32_t val)
{
    uint32_t macaddr[2];

    s->mac_reg[index] = val;

    if (index == RA + 1) {
        macaddr[0] = cpu_to_le32(s->mac_reg[RA]);
        macaddr[1] = cpu_to_le32(s->mac_reg[RA + 1]);
        qemu_format_nic_info_str(qemu_get_queue(s->nic), (uint8_t *)macaddr);
    }
}

static void
set_rdt(E1000State *s, int index, uint32_t val)
{
    s->mac_reg[index] = val & 0xffff;
    if (e1000_has_rxbufs(s, 1)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

#define LOW_BITS_SET_FUNC(num)                             \
    static void                                            \
    set_##num##bit(E1000State *s, int index, uint32_t val) \
    {                                                      \
        s->mac_reg[index] = val & (BIT(num) - 1);          \
    }

LOW_BITS_SET_FUNC(4)
LOW_BITS_SET_FUNC(11)
LOW_BITS_SET_FUNC(13)
LOW_BITS_SET_FUNC(16)

static void
set_dlen(E1000State *s, int index, uint32_t val)
{
    s->mac_reg[index] = val & 0xfff80;
}

static void
set_tctl(E1000State *s, int index, uint32_t val)
{
    s->mac_reg[index] = val;
    s->mac_reg[TDT] &= 0xffff;
    start_xmit(s);
}

static void
set_icr(E1000State *s, int index, uint32_t val)
{
    DBGOUT(INTERRUPT, "set_icr %x\n", val);
    set_interrupt_cause(s, 0, s->mac_reg[ICR] & ~val);
}

static void
set_imc(E1000State *s, int index, uint32_t val)
{
    s->mac_reg[IMS] &= ~val;
    set_ics(s, 0, 0);
}

static void
set_ims(E1000State *s, int index, uint32_t val)
{
    s->mac_reg[IMS] |= val;
    set_ics(s, 0, 0);
}

typedef uint32_t (*readops)(E1000State *, int);
typedef void (*writeops)(E1000State *, int, uint32_t);

/*
 * The macreg_readops/writeops arrays use GNU C range designators
 * [X ... Y] which are not valid in C++. Use constructor init instead.
 */
static readops macreg_readops[0x8000];
static writeops macreg_writeops[0x8000];

enum { NREADOPS = 0x8000 };
enum { NWRITEOPS = 0x8000 };

static void __attribute__((constructor)) e1000_init_reg_ops(void)
{
    /* readops: getreg entries */
    macreg_readops[PBA] = mac_readreg;
    macreg_readops[RCTL] = mac_readreg;
    macreg_readops[TDH] = mac_readreg;
    macreg_readops[TXDCTL] = mac_readreg;
    macreg_readops[WUFC] = mac_readreg;
    macreg_readops[TDT] = mac_readreg;
    macreg_readops[CTRL] = mac_readreg;
    macreg_readops[LEDCTL] = mac_readreg;
    macreg_readops[MANC] = mac_readreg;
    macreg_readops[MDIC] = mac_readreg;
    macreg_readops[SWSM] = mac_readreg;
    macreg_readops[STATUS] = mac_readreg;
    macreg_readops[TORL] = mac_readreg;
    macreg_readops[TOTL] = mac_readreg;
    macreg_readops[IMS] = mac_readreg;
    macreg_readops[TCTL] = mac_readreg;
    macreg_readops[RDH] = mac_readreg;
    macreg_readops[RDT] = mac_readreg;
    macreg_readops[VET] = mac_readreg;
    macreg_readops[ICS] = mac_readreg;
    macreg_readops[TDBAL] = mac_readreg;
    macreg_readops[TDBAH] = mac_readreg;
    macreg_readops[RDBAH] = mac_readreg;
    macreg_readops[RDBAL] = mac_readreg;
    macreg_readops[TDLEN] = mac_readreg;
    macreg_readops[RDLEN] = mac_readreg;
    macreg_readops[RDTR] = mac_readreg;
    macreg_readops[RADV] = mac_readreg;
    macreg_readops[TADV] = mac_readreg;
    macreg_readops[ITR] = mac_readreg;
    macreg_readops[FCRUC] = mac_readreg;
    macreg_readops[IPAV] = mac_readreg;
    macreg_readops[WUC] = mac_readreg;
    macreg_readops[WUS] = mac_readreg;
    macreg_readops[SCC] = mac_readreg;
    macreg_readops[ECOL] = mac_readreg;
    macreg_readops[MCC] = mac_readreg;
    macreg_readops[LATECOL] = mac_readreg;
    macreg_readops[COLC] = mac_readreg;
    macreg_readops[DC] = mac_readreg;
    macreg_readops[TNCRS] = mac_readreg;
    macreg_readops[SEQEC] = mac_readreg;
    macreg_readops[CEXTERR] = mac_readreg;
    macreg_readops[RLEC] = mac_readreg;
    macreg_readops[XONRXC] = mac_readreg;
    macreg_readops[XONTXC] = mac_readreg;
    macreg_readops[XOFFRXC] = mac_readreg;
    macreg_readops[XOFFTXC] = mac_readreg;
    macreg_readops[RFC] = mac_readreg;
    macreg_readops[RJC] = mac_readreg;
    macreg_readops[RNBC] = mac_readreg;
    macreg_readops[TSCTFC] = mac_readreg;
    macreg_readops[MGTPRC] = mac_readreg;
    macreg_readops[MGTPDC] = mac_readreg;
    macreg_readops[MGTPTC] = mac_readreg;
    macreg_readops[GORCL] = mac_readreg;
    macreg_readops[GOTCL] = mac_readreg;
    macreg_readops[RDFH] = mac_readreg;
    macreg_readops[RDFT] = mac_readreg;
    macreg_readops[RDFHS] = mac_readreg;
    macreg_readops[RDFTS] = mac_readreg;
    macreg_readops[RDFPC] = mac_readreg;
    macreg_readops[TDFH] = mac_readreg;
    macreg_readops[TDFT] = mac_readreg;
    macreg_readops[TDFHS] = mac_readreg;
    macreg_readops[TDFTS] = mac_readreg;
    macreg_readops[TDFPC] = mac_readreg;
    macreg_readops[AIT] = mac_readreg;

    /* readops: clr8/clr4 entries */
    macreg_readops[TOTH] = mac_read_clr8;
    macreg_readops[TORH] = mac_read_clr8;
    macreg_readops[GOTCH] = mac_read_clr8;
    macreg_readops[GORCH] = mac_read_clr8;
    macreg_readops[PRC64] = mac_read_clr4;
    macreg_readops[PRC127] = mac_read_clr4;
    macreg_readops[PRC255] = mac_read_clr4;
    macreg_readops[PRC511] = mac_read_clr4;
    macreg_readops[PRC1023] = mac_read_clr4;
    macreg_readops[PRC1522] = mac_read_clr4;
    macreg_readops[PTC64] = mac_read_clr4;
    macreg_readops[PTC127] = mac_read_clr4;
    macreg_readops[PTC255] = mac_read_clr4;
    macreg_readops[PTC511] = mac_read_clr4;
    macreg_readops[PTC1023] = mac_read_clr4;
    macreg_readops[PTC1522] = mac_read_clr4;
    macreg_readops[GPRC] = mac_read_clr4;
    macreg_readops[GPTC] = mac_read_clr4;
    macreg_readops[TPT] = mac_read_clr4;
    macreg_readops[TPR] = mac_read_clr4;
    macreg_readops[RUC] = mac_read_clr4;
    macreg_readops[ROC] = mac_read_clr4;
    macreg_readops[BPRC] = mac_read_clr4;
    macreg_readops[MPRC] = mac_read_clr4;
    macreg_readops[TSCTC] = mac_read_clr4;
    macreg_readops[BPTC] = mac_read_clr4;
    macreg_readops[MPTC] = mac_read_clr4;

    /* readops: special entries */
    macreg_readops[ICR] = mac_icr_read;
    macreg_readops[EECD] = get_eecd;
    macreg_readops[EERD] = flash_eerd_read;

    /* readops: range entries */
    for (int i = CRCERRS; i <= MPC; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = IP6AT; i <= IP6AT + 3; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = IP4AT; i <= IP4AT + 6; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = FFLT; i <= FFLT + 6; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = RA; i <= RA + 31; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = WUPM; i <= WUPM + 31; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = MTA; i <= MTA + E1000_MC_TBL_SIZE - 1; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = VFTA; i <= VFTA + E1000_VLAN_FILTER_TBL_SIZE - 1; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = FFMT; i <= FFMT + 254; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = FFVT; i <= FFVT + 254; i++)
        macreg_readops[i] = &mac_readreg;
    for (int i = PBM; i <= PBM + 16383; i++)
        macreg_readops[i] = &mac_readreg;

    /* writeops: putreg entries */
    macreg_writeops[PBA] = mac_writereg;
    macreg_writeops[EERD] = mac_writereg;
    macreg_writeops[SWSM] = mac_writereg;
    macreg_writeops[WUFC] = mac_writereg;
    macreg_writeops[TDBAL] = mac_writereg;
    macreg_writeops[TDBAH] = mac_writereg;
    macreg_writeops[TXDCTL] = mac_writereg;
    macreg_writeops[RDBAH] = mac_writereg;
    macreg_writeops[RDBAL] = mac_writereg;
    macreg_writeops[LEDCTL] = mac_writereg;
    macreg_writeops[VET] = mac_writereg;
    macreg_writeops[FCRUC] = mac_writereg;
    macreg_writeops[IPAV] = mac_writereg;
    macreg_writeops[WUC] = mac_writereg;
    macreg_writeops[WUS] = mac_writereg;

    /* writeops: special entries */
    macreg_writeops[TDLEN] = set_dlen;
    macreg_writeops[RDLEN] = set_dlen;
    macreg_writeops[TCTL] = set_tctl;
    macreg_writeops[TDT] = set_tctl;
    macreg_writeops[MDIC] = set_mdic;
    macreg_writeops[ICS] = set_ics;
    macreg_writeops[TDH] = set_16bit;
    macreg_writeops[RDH] = set_16bit;
    macreg_writeops[RDT] = set_rdt;
    macreg_writeops[IMC] = set_imc;
    macreg_writeops[IMS] = set_ims;
    macreg_writeops[ICR] = set_icr;
    macreg_writeops[EECD] = set_eecd;
    macreg_writeops[RCTL] = set_rx_control;
    macreg_writeops[CTRL] = set_ctrl;
    macreg_writeops[RDTR] = set_16bit;
    macreg_writeops[RADV] = set_16bit;
    macreg_writeops[TADV] = set_16bit;
    macreg_writeops[ITR] = set_16bit;
    macreg_writeops[TDFH] = set_11bit;
    macreg_writeops[TDFT] = set_11bit;
    macreg_writeops[TDFHS] = set_13bit;
    macreg_writeops[TDFTS] = set_13bit;
    macreg_writeops[TDFPC] = set_13bit;
    macreg_writeops[RDFH] = set_13bit;
    macreg_writeops[RDFT] = set_13bit;
    macreg_writeops[RDFHS] = set_13bit;
    macreg_writeops[RDFTS] = set_13bit;
    macreg_writeops[RDFPC] = set_13bit;
    macreg_writeops[AIT] = set_16bit;

    /* writeops: range entries */
    for (int i = IP6AT; i <= IP6AT + 3; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = IP4AT; i <= IP4AT + 6; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = FFLT; i <= FFLT + 6; i++)
        macreg_writeops[i] = &set_11bit;
    for (int i = RA; i <= RA + 31; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = WUPM; i <= WUPM + 31; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = MTA; i <= MTA + E1000_MC_TBL_SIZE - 1; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = VFTA; i <= VFTA + E1000_VLAN_FILTER_TBL_SIZE - 1; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = FFMT; i <= FFMT + 254; i++)
        macreg_writeops[i] = &set_4bit;
    for (int i = FFVT; i <= FFVT + 254; i++)
        macreg_writeops[i] = &mac_writereg;
    for (int i = PBM; i <= PBM + 16383; i++)
        macreg_writeops[i] = &mac_writereg;
}

enum { MAC_ACCESS_PARTIAL = 1, MAC_ACCESS_FLAG_NEEDED = 2 };

/* In the array below the meaning of the bits is: [f|f|f|f|f|f|n|p]
 * f - flag bits (up to 6 possible flags)
 * n - flag needed
 * p - partially implemented */
static uint8_t mac_reg_access[0x8000];

static void __attribute__((constructor)) e1000_init_mac_reg_access(void)
{
    mac_reg_access[IPAV]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[WUC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[IP6AT]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[IP4AT]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[FFVT]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[WUPM]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[ECOL]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[MCC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[DC]      = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[TNCRS]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[RLEC]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[XONRXC]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[XOFFTXC] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[RFC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[TSCTFC]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[MGTPRC]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[WUS]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[AIT]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[FFLT]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[FFMT]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[SCC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[FCRUC]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[LATECOL] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[COLC]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[SEQEC]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[CEXTERR] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[XONTXC]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[XOFFRXC] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[RJC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[RNBC]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[MGTPDC]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[MGTPTC]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[RUC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[ROC]     = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[GORCL]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[GORCH]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[GOTCL]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[GOTCH]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[BPRC]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[MPRC]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[TSCTC]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PRC64]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PRC127]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PRC255]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PRC511]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PRC1023] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PRC1522] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PTC64]   = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PTC127]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PTC255]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PTC511]  = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PTC1023] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[PTC1522] = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[MPTC]    = MAC_ACCESS_FLAG_NEEDED;
    mac_reg_access[BPTC]    = MAC_ACCESS_FLAG_NEEDED;

    mac_reg_access[TDFH]    = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFT]    = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFHS]   = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFTS]   = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFPC]   = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFH]    = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFT]    = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFHS]   = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFTS]   = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFPC]   = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
    mac_reg_access[PBM]     = MAC_ACCESS_FLAG_NEEDED | MAC_ACCESS_PARTIAL;
}

static void
e1000_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                 unsigned size)
{
    E1000State *s = static_cast<E1000State *>(opaque);
    unsigned int index = (addr & 0x1ffff) >> 2;

    if (index < NWRITEOPS && macreg_writeops[index]) {
        if (!(mac_reg_access[index] & MAC_ACCESS_FLAG_NEEDED)
            || (s->compat_flags & (mac_reg_access[index] >> 2))) {
            if (mac_reg_access[index] & MAC_ACCESS_PARTIAL) {
                DBGOUT(GENERAL, "Writing to register at offset: 0x%08x. "
                       "It is not fully implemented.\n", index<<2);
            }
            macreg_writeops[index](s, index, val);
        } else {    /* "flag needed" bit is set, but the flag is not active */
            DBGOUT(MMIO, "MMIO write attempt to disabled reg. addr=0x%08x\n",
                   index<<2);
        }
    } else if (index < NREADOPS && macreg_readops[index]) {
        DBGOUT(MMIO, "e1000_mmio_writel RO %x: 0x%04" PRIx64 "\n",
               index<<2, val);
    } else {
        DBGOUT(UNKNOWN, "MMIO unknown write addr=0x%08x,val=0x%08" PRIx64 "\n",
               index<<2, val);
    }
}

static uint64_t
e1000_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    E1000State *s = static_cast<E1000State *>(opaque);
    unsigned int index = (addr & 0x1ffff) >> 2;

    if (index < NREADOPS && macreg_readops[index]) {
        if (!(mac_reg_access[index] & MAC_ACCESS_FLAG_NEEDED)
            || (s->compat_flags & (mac_reg_access[index] >> 2))) {
            if (mac_reg_access[index] & MAC_ACCESS_PARTIAL) {
                DBGOUT(GENERAL, "Reading register at offset: 0x%08x. "
                       "It is not fully implemented.\n", index<<2);
            }
            return macreg_readops[index](s, index);
        } else {    /* "flag needed" bit is set, but the flag is not active */
            DBGOUT(MMIO, "MMIO read attempt of disabled reg. addr=0x%08x\n",
                   index<<2);
        }
    } else {
        DBGOUT(UNKNOWN, "MMIO unknown read addr=0x%08x\n", index<<2);
    }
    return 0;
}

static const MemoryRegionOps e1000_mmio_ops = {
    .read = e1000_mmio_read,
    .write = e1000_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t e1000_io_read(void *opaque, hwaddr addr,
                              unsigned size)
{
    E1000State *s = static_cast<E1000State *>(opaque);

    (void)s;
    return 0;
}

static void e1000_io_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    E1000State *s = static_cast<E1000State *>(opaque);

    (void)s;
}

static const MemoryRegionOps e1000_io_ops = {
    .read = e1000_io_read,
    .write = e1000_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool is_version_1(void *opaque, int version_id)
{
    return version_id == 1;
}

static int e1000_pre_save(void *opaque)
{
    E1000State *s = static_cast<E1000State *>(opaque);
    NetClientState *nc = qemu_get_queue(s->nic);

    /*
     * If link is down and auto-negotiation is supported and ongoing,
     * complete auto-negotiation immediately. This allows us to look
     * at MII_BMSR_AN_COMP to infer link status on load.
     */
    if (nc->link_down && have_autoneg(s)) {
        s->phy_reg[MII_BMSR] |= MII_BMSR_AN_COMP;
    }

    /* Decide which set of props to migrate in the main structure */
    if (chkflag(TSO) || !s->use_tso_for_migration) {
        /* Either we're migrating with the extra subsection, in which
         * case the mig_props is always 'props' OR
         * we've not got the subsection, but 'props' was the last
         * updated.
         */
        s->mig_props = s->tx.props;
    } else {
        /* We're not using the subsection, and 'tso_props' was
         * the last updated.
         */
        s->mig_props = s->tx.tso_props;
    }
    return 0;
}

static int e1000_post_load(void *opaque, int version_id)
{
    E1000State *s = static_cast<E1000State *>(opaque);
    NetClientState *nc = qemu_get_queue(s->nic);

    s->mit_ide = 0;
    s->mit_timer_on = true;
    timer_mod(s->mit_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);

    /* nc.link_down can't be migrated, so infer link_down according
     * to link status bit in mac_reg[STATUS].
     * Alternatively, restart link negotiation if it was in progress. */
    nc->link_down = (s->mac_reg[STATUS] & E1000_STATUS_LU) == 0;

    if (have_autoneg(s) && !(s->phy_reg[MII_BMSR] & MII_BMSR_AN_COMP)) {
        nc->link_down = false;
        timer_mod(s->autoneg_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);
    }

    s->tx.props = s->mig_props;
    if (!s->received_tx_tso) {
        /* We received only one set of offload data (tx.props)
         * and haven't got tx.tso_props.  The best we can do
         * is dupe the data.
         */
        s->tx.tso_props = s->mig_props;
    }
    return 0;
}

static int e1000_tx_tso_post_load(void *opaque, int version_id)
{
    E1000State *s = static_cast<E1000State *>(opaque);
    s->received_tx_tso = true;
    return 0;
}

static bool e1000_tso_state_needed(void *opaque)
{
    E1000State *s = static_cast<E1000State *>(opaque);

    return chkflag(TSO);
}

static const VMStateField vmstate_e1000_mit_state_fields[] = {
    VMSTATE_UINT32(mac_reg[RDTR], E1000State),
    VMSTATE_UINT32(mac_reg[RADV], E1000State),
    VMSTATE_UINT32(mac_reg[TADV], E1000State),
    VMSTATE_UINT32(mac_reg[ITR], E1000State),
    VMSTATE_BOOL(mit_irq_level, E1000State),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_e1000_mit_state = {
    .name = "e1000/mit_state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_e1000_mit_state_fields,
};

static const VMStateField vmstate_e1000_full_mac_state_fields[] = {
    VMSTATE_UINT32_ARRAY(mac_reg, E1000State, 0x8000),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_e1000_full_mac_state = {
    .name = "e1000/full_mac_state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_e1000_full_mac_state_fields,
};

static const VMStateField vmstate_e1000_tx_tso_state_fields[] = {
    VMSTATE_UINT8(tx.tso_props.ipcss, E1000State),
    VMSTATE_UINT8(tx.tso_props.ipcso, E1000State),
    VMSTATE_UINT16(tx.tso_props.ipcse, E1000State),
    VMSTATE_UINT8(tx.tso_props.tucss, E1000State),
    VMSTATE_UINT8(tx.tso_props.tucso, E1000State),
    VMSTATE_UINT16(tx.tso_props.tucse, E1000State),
    VMSTATE_UINT32(tx.tso_props.paylen, E1000State),
    VMSTATE_UINT8(tx.tso_props.hdr_len, E1000State),
    VMSTATE_UINT16(tx.tso_props.mss, E1000State),
    VMSTATE_INT8(tx.tso_props.ip, E1000State),
    VMSTATE_INT8(tx.tso_props.tcp, E1000State),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_e1000_tx_tso_state = {
    .name = "e1000/tx_tso_state",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = e1000_tx_tso_post_load,
    .needed = e1000_tso_state_needed,
    .fields = vmstate_e1000_tx_tso_state_fields,
};

static const VMStateField vmstate_e1000_fields[] = {
    VMSTATE_PCI_DEVICE(parent_obj, E1000State),
    VMSTATE_UNUSED_TEST(is_version_1, 4), /* was instance id */
    VMSTATE_UNUSED(4), /* Was mmio_base.  */
    VMSTATE_UINT32(rxbuf_size, E1000State),
    VMSTATE_UINT32(rxbuf_min_shift, E1000State),
    VMSTATE_UINT32(eecd_state.val_in, E1000State),
    VMSTATE_UINT16(eecd_state.bitnum_in, E1000State),
    VMSTATE_UINT16(eecd_state.bitnum_out, E1000State),
    VMSTATE_UINT16(eecd_state.reading, E1000State),
    VMSTATE_UINT32(eecd_state.old_eecd, E1000State),
    VMSTATE_UINT8(mig_props.ipcss, E1000State),
    VMSTATE_UINT8(mig_props.ipcso, E1000State),
    VMSTATE_UINT16(mig_props.ipcse, E1000State),
    VMSTATE_UINT8(mig_props.tucss, E1000State),
    VMSTATE_UINT8(mig_props.tucso, E1000State),
    VMSTATE_UINT16(mig_props.tucse, E1000State),
    VMSTATE_UINT32(mig_props.paylen, E1000State),
    VMSTATE_UINT8(mig_props.hdr_len, E1000State),
    VMSTATE_UINT16(mig_props.mss, E1000State),
    VMSTATE_UINT16(tx.size, E1000State),
    VMSTATE_UINT16(tx.tso_frames, E1000State),
    VMSTATE_UINT8(tx.sum_needed, E1000State),
    VMSTATE_INT8(mig_props.ip, E1000State),
    VMSTATE_INT8(mig_props.tcp, E1000State),
    VMSTATE_BUFFER(tx.header, E1000State),
    VMSTATE_BUFFER(tx.data, E1000State),
    VMSTATE_UINT16_ARRAY(eeprom_data, E1000State, 64),
    VMSTATE_UINT16_ARRAY(phy_reg, E1000State, 0x20),
    VMSTATE_UINT32(mac_reg[CTRL], E1000State),
    VMSTATE_UINT32(mac_reg[EECD], E1000State),
    VMSTATE_UINT32(mac_reg[EERD], E1000State),
    VMSTATE_UINT32(mac_reg[GPRC], E1000State),
    VMSTATE_UINT32(mac_reg[GPTC], E1000State),
    VMSTATE_UINT32(mac_reg[ICR], E1000State),
    VMSTATE_UINT32(mac_reg[ICS], E1000State),
    VMSTATE_UINT32(mac_reg[IMC], E1000State),
    VMSTATE_UINT32(mac_reg[IMS], E1000State),
    VMSTATE_UINT32(mac_reg[LEDCTL], E1000State),
    VMSTATE_UINT32(mac_reg[MANC], E1000State),
    VMSTATE_UINT32(mac_reg[MDIC], E1000State),
    VMSTATE_UINT32(mac_reg[MPC], E1000State),
    VMSTATE_UINT32(mac_reg[PBA], E1000State),
    VMSTATE_UINT32(mac_reg[RCTL], E1000State),
    VMSTATE_UINT32(mac_reg[RDBAH], E1000State),
    VMSTATE_UINT32(mac_reg[RDBAL], E1000State),
    VMSTATE_UINT32(mac_reg[RDH], E1000State),
    VMSTATE_UINT32(mac_reg[RDLEN], E1000State),
    VMSTATE_UINT32(mac_reg[RDT], E1000State),
    VMSTATE_UINT32(mac_reg[STATUS], E1000State),
    VMSTATE_UINT32(mac_reg[SWSM], E1000State),
    VMSTATE_UINT32(mac_reg[TCTL], E1000State),
    VMSTATE_UINT32(mac_reg[TDBAH], E1000State),
    VMSTATE_UINT32(mac_reg[TDBAL], E1000State),
    VMSTATE_UINT32(mac_reg[TDH], E1000State),
    VMSTATE_UINT32(mac_reg[TDLEN], E1000State),
    VMSTATE_UINT32(mac_reg[TDT], E1000State),
    VMSTATE_UINT32(mac_reg[TORH], E1000State),
    VMSTATE_UINT32(mac_reg[TORL], E1000State),
    VMSTATE_UINT32(mac_reg[TOTH], E1000State),
    VMSTATE_UINT32(mac_reg[TOTL], E1000State),
    VMSTATE_UINT32(mac_reg[TPR], E1000State),
    VMSTATE_UINT32(mac_reg[TPT], E1000State),
    VMSTATE_UINT32(mac_reg[TXDCTL], E1000State),
    VMSTATE_UINT32(mac_reg[WUFC], E1000State),
    VMSTATE_UINT32(mac_reg[VET], E1000State),
    VMSTATE_UINT32_SUB_ARRAY(mac_reg, E1000State, RA, 32),
    VMSTATE_UINT32_SUB_ARRAY(mac_reg, E1000State, MTA, E1000_MC_TBL_SIZE),
    VMSTATE_UINT32_SUB_ARRAY(mac_reg, E1000State, VFTA,
                             E1000_VLAN_FILTER_TBL_SIZE),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription * const vmstate_e1000_subsections[] = {
    &vmstate_e1000_mit_state,
    &vmstate_e1000_full_mac_state,
    &vmstate_e1000_tx_tso_state,
    NULL
};

static const VMStateDescription vmstate_e1000 = {
    .name = "e1000",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = e1000_post_load,
    .pre_save = e1000_pre_save,
    .fields = vmstate_e1000_fields,
    .subsections = vmstate_e1000_subsections,
};

/*
 * EEPROM contents documented in Tables 5-2 and 5-3, pp. 98-102.
 * Note: A valid DevId will be inserted during pci_e1000_realize().
 */
static const uint16_t e1000_eeprom_template[64] = {
    0x0000, 0x0000, 0x0000, 0x0000,      0xffff, 0x0000,      0x0000, 0x0000,
    0x3000, 0x1000, 0x6403, 0 /*DevId*/, 0x8086, 0 /*DevId*/, 0x8086, 0x3040,
    0x0008, 0x2000, 0x7e14, 0x0048,      0x1000, 0x00d8,      0x0000, 0x2700,
    0x6cc9, 0x3150, 0x0722, 0x040b,      0x0984, 0x0000,      0xc000, 0x0706,
    0x1008, 0x0000, 0x0f04, 0x7fff,      0x4d01, 0xffff,      0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff,      0xffff, 0xffff,      0xffff, 0xffff,
    0x0100, 0x4000, 0x121c, 0xffff,      0xffff, 0xffff,      0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff,      0xffff, 0xffff,      0xffff, 0x0000,
};

/* PCI interface */

static void
e1000_mmio_setup(E1000State *d)
{
    int i;
    const uint32_t excluded_regs[] = {
        E1000_MDIC, E1000_ICR, E1000_ICS, E1000_IMS,
        E1000_IMC, E1000_TCTL, E1000_TDT, PNPMMIO_SIZE
    };

    memory_region_init_io(&d->mmio, reinterpret_cast<Object *>(d), &e1000_mmio_ops, d,
                          "e1000-mmio", PNPMMIO_SIZE);
    memory_region_add_coalescing(&d->mmio, 0, excluded_regs[0]);
    for (i = 0; excluded_regs[i] != PNPMMIO_SIZE; i++)
        memory_region_add_coalescing(&d->mmio, excluded_regs[i] + 4,
                                     excluded_regs[i+1] - excluded_regs[i] - 4);
    memory_region_init_io(&d->io, reinterpret_cast<Object *>(d), &e1000_io_ops, d, "e1000-io", IOPORT_SIZE);
}

static void
pci_e1000_uninit(PCIDevice *dev)
{
    E1000State *d = reinterpret_cast<E1000State *>(dev);

    timer_free(d->autoneg_timer);
    timer_free(d->mit_timer);
    timer_free(d->flush_queue_timer);
    qemu_del_nic(d->nic);
}

static NetClientInfo net_e1000_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = e1000_receive,
    .receive_iov = e1000_receive_iov,
    .can_receive = e1000_can_receive,
    .link_status_changed = e1000_set_link_status,
};

static void e1000_write_config(PCIDevice *pci_dev, uint32_t address,
                                uint32_t val, int len)
{
    E1000State *s = reinterpret_cast<E1000State *>(pci_dev);

    pci_default_write_config(pci_dev, address, val, len);

    if (range_covers_byte(address, len, PCI_COMMAND) &&
        (pci_dev->config[PCI_COMMAND] & PCI_COMMAND_MASTER)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

void E1000State_st::realizeImpl(Error **errp)
{
    PCIDevice *pci_dev = reinterpret_cast<PCIDevice *>(this);
    DeviceState *dev = reinterpret_cast<DeviceState *>(pci_dev);
    uint8_t *pci_conf;
    uint8_t *macaddr;

    pci_dev->config_write = e1000_write_config;

    pci_conf = pci_dev->config;

    /* TODO: RST# value should be 0, PCI spec 6.2.4 */
    pci_conf[PCI_CACHE_LINE_SIZE] = 0x10;

    pci_conf[PCI_INTERRUPT_PIN] = 1; /* interrupt pin A */

    e1000_mmio_setup(this);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &mmio);

    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &io);

    qemu_macaddr_default_if_unset(&conf.macaddr);
    macaddr = conf.macaddr.a;

    e1000x_core_prepare_eeprom(eeprom_data,
                               e1000_eeprom_template,
                               sizeof(e1000_eeprom_template),
                               PCI_DEVICE_GET_CLASS(pci_dev)->device_id,
                               macaddr);

    nic = qemu_new_nic(&net_e1000_info, &conf,
                       object_get_typename(reinterpret_cast<Object *>(this)), dev->id,
                       &dev->mem_reentrancy_guard, this);

    qemu_format_nic_info_str(qemu_get_queue(nic), macaddr);

    autoneg_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, e1000_autoneg_timer, this);
    mit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, e1000_mit_timer, this);
    flush_queue_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     e1000_flush_queue_timer, this);
}

/* static wrapper */
void E1000State_st::realizeStatic(PCIDevice *pci_dev, Error **errp)
{
    E1000State *d = reinterpret_cast<E1000State *>(pci_dev);
    d->realizeImpl(errp);
}

static const Property e1000_properties[] = {
    DEFINE_NIC_PROPERTIES(E1000State, conf),
    DEFINE_PROP_BIT("migrate_tso_props", E1000State,
                    compat_flags, E1000_FLAG_TSO_BIT, true),
    DEFINE_PROP_BIT("init-vet", E1000State,
                    compat_flags, E1000_FLAG_VET_BIT, true),
};

typedef struct E1000Info {
    const char *name;
    uint16_t   device_id;
    uint8_t    revision;
    uint16_t   phy_id2;
} E1000Info;

/* static */ void E1000State_st::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    ResettableClass *rc = reinterpret_cast<ResettableClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);
    E1000BaseClass *e = reinterpret_cast<E1000BaseClass *>(klass);
    const E1000Info *info = static_cast<const E1000Info *>(data);

    k->realize = E1000State_st::realizeStatic;
    k->exit = pci_e1000_uninit;
    k->romfile = "efi-e1000.rom";
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = info->device_id;
    k->revision = info->revision;
    e->phy_id2 = info->phy_id2;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    rc->phases.hold = E1000State_st::resetHoldStatic;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "Intel Gigabit Ethernet";
    dc->vmsd = &vmstate_e1000;
    device_class_set_props(dc, e1000_properties);
}

/* static */ void E1000State_st::instanceInit(Object *obj)
{
    E1000State *n = reinterpret_cast<E1000State *>(obj);
    device_add_bootindex_property(obj, &n->conf.bootindex,
                                  "bootindex", "/ethernet-phy@0",
                                  reinterpret_cast<DeviceState *>(n));
}

static const InterfaceInfo e1000_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo e1000_base_info = {
    .name          = TYPE_E1000_BASE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(E1000State),
    .instance_init = E1000State_st::instanceInit,
    .is_abstract   = true,
    .class_size    = sizeof(E1000BaseClass),
    .interfaces    = e1000_interfaces,
};

static const E1000Info e1000_devices[] = {
    {
        .name      = "e1000",
        .device_id = E1000_DEV_ID_82540EM,
        .revision  = 0x03,
        .phy_id2   = E1000_PHY_ID2_8254xx_DEFAULT,
    },
    {
        .name      = "e1000-82544gc",
        .device_id = E1000_DEV_ID_82544GC_COPPER,
        .revision  = 0x03,
        .phy_id2   = E1000_PHY_ID2_82544x,
    },
    {
        .name      = "e1000-82545em",
        .device_id = E1000_DEV_ID_82545EM_COPPER,
        .revision  = 0x03,
        .phy_id2   = E1000_PHY_ID2_8254xx_DEFAULT,
    },
};

static void e1000_register_types(void)
{
    int i;

    type_register_static(&e1000_base_info);
    for (i = 0; i < ARRAY_SIZE(e1000_devices); i++) {
        const E1000Info *info = &e1000_devices[i];
        TypeInfo type_info = {};

        type_info.name = info->name;
        type_info.parent = TYPE_E1000_BASE;
        type_info.class_data = info;
        type_info.class_init = E1000State_st::classInit;

        type_register_static(&type_info);
    }
}

type_init(e1000_register_types)
