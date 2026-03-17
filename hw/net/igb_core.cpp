/*
 * Core code for QEMU igb emulation
 *
 * Datasheet:
 * https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82576eg-gbe-datasheet.pdf
 *
 * Copyright (c) 2020-2023 Red Hat, Inc.
 * Copyright (c) 2015 Ravello Systems LTD (http://ravellosystems.com)
 * Developed by Daynix Computing LTD (http://www.daynix.com)
 *
 * Authors:
 * Akihiko Odaki <akihiko.odaki@daynix.com>
 * Gal Hammmer <gal.hammer@sap.com>
 * Marcel Apfelbaum <marcel.apfelbaum@gmail.com>
 * Dmitry Fleytman <dmitry@daynix.com>
 * Leonid Bloch <leonid@daynix.com>
 * Yan Vugenfirer <yan@daynix.com>
 *
 * Based on work done by:
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
#include "qemu/log.h"
#include "net/net.h"
#include "net/tap.h"
#include "hw/net/mii.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "system/runstate.h"

#include "net_tx_pkt.h"
#include "net_rx_pkt.h"

#include "igb_common.h"
#include "e1000x_common.h"
#include "igb_core.h"

#include "trace.h"

#define E1000E_MAX_TX_FRAGS (64)

union e1000_rx_desc_union {
    struct e1000_rx_desc legacy;
    union e1000_adv_rx_desc adv;
};

typedef struct IGBTxPktVmdqCallbackContext {
    IGBCore *core;
    NetClientState *nc;
} IGBTxPktVmdqCallbackContext;

typedef struct L2Header {
    struct eth_header eth;
    struct vlan_header vlan[2];
} L2Header;

typedef struct PTP2 {
    uint8_t message_id_transport_specific;
    uint8_t version_ptp;
    uint16_t message_length;
    uint8_t subdomain_number;
    uint8_t reserved0;
    uint16_t flags;
    uint64_t correction;
    uint8_t reserved1[5];
    uint8_t source_communication_technology;
    uint32_t source_uuid_lo;
    uint16_t source_uuid_hi;
    uint16_t source_port_id;
    uint16_t sequence_id;
    uint8_t control;
    uint8_t log_message_period;
} PTP2;

static ssize_t
igb_receive_internal(IGBCore *core, const struct iovec *iov, int iovcnt,
                     bool has_vnet, bool *external_tx);

static void igb_raise_interrupts(IGBCore *core, size_t index, uint32_t causes);
static void igb_reset(IGBCore *core, bool sw);

static inline void
igb_raise_legacy_irq(IGBCore *core)
{
    trace_e1000e_irq_legacy_notify(true);
    e1000x_inc_reg_if_not_full(core->mac, IAC);
    pci_set_irq(core->owner, 1);
}

static inline void
igb_lower_legacy_irq(IGBCore *core)
{
    trace_e1000e_irq_legacy_notify(false);
    pci_set_irq(core->owner, 0);
}

static void igb_msix_notify(IGBCore *core, unsigned int cause)
{
    PCIDevice *dev = core->owner;
    uint16_t vfn;
    uint32_t effective_eiac;
    unsigned int vector;

    vfn = 8 - (cause + 2) / IGBVF_MSIX_VEC_NUM;
    if (vfn < pcie_sriov_num_vfs(core->owner)) {
        dev = pcie_sriov_get_vf_at_index(core->owner, vfn);
        assert(dev);
        vector = (cause + 2) % IGBVF_MSIX_VEC_NUM;
    } else if (cause >= IGB_MSIX_VEC_NUM) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "igb: Tried to use vector unavailable for PF");
        return;
    } else {
        vector = cause;
    }

    msix_notify(dev, vector);

    trace_e1000e_irq_icr_clear_eiac(core->mac[EICR], core->mac[EIAC]);
    effective_eiac = core->mac[EIAC] & BIT(cause);
    core->mac[EICR] &= ~effective_eiac;
}

static inline void
igb_intrmgr_rearm_timer(IGBIntrDelayTimer *timer)
{
    int64_t delay_ns = (int64_t) timer->core->mac[timer->delay_reg] *
                                 timer->delay_resolution_ns;

    trace_e1000e_irq_rearm_timer(timer->delay_reg << 2, delay_ns);

    timer_mod(timer->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);

    timer->running = true;
}

static void
igb_intmgr_timer_resume(IGBIntrDelayTimer *timer)
{
    if (timer->running) {
        igb_intrmgr_rearm_timer(timer);
    }
}

static void
igb_intrmgr_on_msix_throttling_timer(void *opaque)
{
    IGBIntrDelayTimer *timer = static_cast<IGBIntrDelayTimer *>(opaque);
    int idx = timer - &timer->core->eitr[0];

    timer->running = false;

    trace_e1000e_irq_msix_notify_postponed_vec(idx);
    igb_msix_notify(timer->core, idx);
}

static void
igb_intrmgr_initialize_all_timers(IGBCore *core, bool create)
{
    int i;

    for (i = 0; i < IGB_INTR_NUM; i++) {
        core->eitr[i].core = core;
        core->eitr[i].delay_reg = EITR0 + i;
        core->eitr[i].delay_resolution_ns = E1000_INTR_DELAY_NS_RES;
    }

    if (!create) {
        return;
    }

    for (i = 0; i < IGB_INTR_NUM; i++) {
        core->eitr[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           igb_intrmgr_on_msix_throttling_timer,
                                           &core->eitr[i]);
    }
}

static void
igb_intrmgr_resume(IGBCore *core)
{
    int i;

    for (i = 0; i < IGB_INTR_NUM; i++) {
        igb_intmgr_timer_resume(&core->eitr[i]);
    }
}

static void
igb_intrmgr_reset(IGBCore *core)
{
    int i;

    for (i = 0; i < IGB_INTR_NUM; i++) {
        if (core->eitr[i].running) {
            timer_del(core->eitr[i].timer);
            igb_intrmgr_on_msix_throttling_timer(&core->eitr[i]);
        }
    }
}

static void
igb_intrmgr_pci_unint(IGBCore *core)
{
    int i;

    for (i = 0; i < IGB_INTR_NUM; i++) {
        timer_free(core->eitr[i].timer);
    }
}

static void
igb_intrmgr_pci_realize(IGBCore *core)
{
    igb_intrmgr_initialize_all_timers(core, true);
}

static inline bool
igb_rx_csum_enabled(IGBCore *core)
{
    return (core->mac[RXCSUM] & E1000_RXCSUM_PCSD) ? false : true;
}

static inline bool
igb_rx_use_legacy_descriptor(IGBCore *core)
{
    /*
     * TODO: If SRRCTL[n],DESCTYPE = 000b, the 82576 uses the legacy Rx
     * descriptor.
     */
    return false;
}

typedef struct E1000ERingInfo {
    int dbah;
    int dbal;
    int dlen;
    int dh;
    int dt;
    int idx;
} E1000ERingInfo;

static uint32_t
igb_rx_queue_desctyp_get(IGBCore *core, const E1000ERingInfo *r)
{
    return core->mac[E1000_SRRCTL(r->idx) >> 2] & E1000_SRRCTL_DESCTYPE_MASK;
}

static bool
igb_rx_use_ps_descriptor(IGBCore *core, const E1000ERingInfo *r)
{
    uint32_t desctyp = igb_rx_queue_desctyp_get(core, r);
    return desctyp == E1000_SRRCTL_DESCTYPE_HDR_SPLIT ||
           desctyp == E1000_SRRCTL_DESCTYPE_HDR_SPLIT_ALWAYS;
}

static inline bool
igb_rss_enabled(IGBCore *core)
{
    return (core->mac[MRQC] & 3) == E1000_MRQC_ENABLE_RSS_MQ &&
           !igb_rx_csum_enabled(core) &&
           !igb_rx_use_legacy_descriptor(core);
}

typedef struct E1000E_RSSInfo_st {
    bool enabled;
    uint32_t hash;
    uint32_t queue;
    uint32_t type;
} E1000E_RSSInfo;

static uint32_t
igb_rss_get_hash_type(IGBCore *core, struct NetRxPkt *pkt)
{
    bool hasip4, hasip6;
    EthL4HdrProto l4hdr_proto;

    assert(igb_rss_enabled(core));

    net_rx_pkt_get_protocols(pkt, &hasip4, &hasip6, &l4hdr_proto);

    if (hasip4) {
        trace_e1000e_rx_rss_ip4(l4hdr_proto, core->mac[MRQC],
                                E1000_MRQC_EN_TCPIPV4(core->mac[MRQC]),
                                E1000_MRQC_EN_IPV4(core->mac[MRQC]));

        if (l4hdr_proto == ETH_L4_HDR_PROTO_TCP &&
            E1000_MRQC_EN_TCPIPV4(core->mac[MRQC])) {
            return E1000_MRQ_RSS_TYPE_IPV4TCP;
        }

        if (l4hdr_proto == ETH_L4_HDR_PROTO_UDP &&
            (core->mac[MRQC] & E1000_MRQC_RSS_FIELD_IPV4_UDP)) {
            return E1000_MRQ_RSS_TYPE_IPV4UDP;
        }

        if (E1000_MRQC_EN_IPV4(core->mac[MRQC])) {
            return E1000_MRQ_RSS_TYPE_IPV4;
        }
    } else if (hasip6) {
        eth_ip6_hdr_info *ip6info = net_rx_pkt_get_ip6_info(pkt);

        bool ex_dis = core->mac[RFCTL] & E1000_RFCTL_IPV6_EX_DIS;
        bool new_ex_dis = core->mac[RFCTL] & E1000_RFCTL_NEW_IPV6_EXT_DIS;

        /*
         * Following two traces must not be combined because resulting
         * event will have 11 arguments totally and some trace backends
         * (at least "ust") have limitation of maximum 10 arguments per
         * event. Events with more arguments fail to compile for
         * backends like these.
         */
        trace_e1000e_rx_rss_ip6_rfctl(core->mac[RFCTL]);
        trace_e1000e_rx_rss_ip6(ex_dis, new_ex_dis, l4hdr_proto,
                                ip6info->has_ext_hdrs,
                                ip6info->rss_ex_dst_valid,
                                ip6info->rss_ex_src_valid,
                                core->mac[MRQC],
                                E1000_MRQC_EN_TCPIPV6EX(core->mac[MRQC]),
                                E1000_MRQC_EN_IPV6EX(core->mac[MRQC]),
                                E1000_MRQC_EN_IPV6(core->mac[MRQC]));

        if ((!ex_dis || !ip6info->has_ext_hdrs) &&
            (!new_ex_dis || !(ip6info->rss_ex_dst_valid ||
                              ip6info->rss_ex_src_valid))) {

            if (l4hdr_proto == ETH_L4_HDR_PROTO_TCP &&
                E1000_MRQC_EN_TCPIPV6EX(core->mac[MRQC])) {
                return E1000_MRQ_RSS_TYPE_IPV6TCPEX;
            }

            if (l4hdr_proto == ETH_L4_HDR_PROTO_UDP &&
                (core->mac[MRQC] & E1000_MRQC_RSS_FIELD_IPV6_UDP)) {
                return E1000_MRQ_RSS_TYPE_IPV6UDP;
            }

            if (E1000_MRQC_EN_IPV6EX(core->mac[MRQC])) {
                return E1000_MRQ_RSS_TYPE_IPV6EX;
            }

        }

        if (E1000_MRQC_EN_IPV6(core->mac[MRQC])) {
            return E1000_MRQ_RSS_TYPE_IPV6;
        }

    }

    return E1000_MRQ_RSS_TYPE_NONE;
}

static uint32_t
igb_rss_calc_hash(IGBCore *core, struct NetRxPkt *pkt, E1000E_RSSInfo *info)
{
    NetRxPktRssType type;

    assert(igb_rss_enabled(core));

    switch (info->type) {
    case E1000_MRQ_RSS_TYPE_IPV4:
        type = NetPktRssIpV4;
        break;
    case E1000_MRQ_RSS_TYPE_IPV4TCP:
        type = NetPktRssIpV4Tcp;
        break;
    case E1000_MRQ_RSS_TYPE_IPV6TCPEX:
        type = NetPktRssIpV6TcpEx;
        break;
    case E1000_MRQ_RSS_TYPE_IPV6:
        type = NetPktRssIpV6;
        break;
    case E1000_MRQ_RSS_TYPE_IPV6EX:
        type = NetPktRssIpV6Ex;
        break;
    case E1000_MRQ_RSS_TYPE_IPV4UDP:
        type = NetPktRssIpV4Udp;
        break;
    case E1000_MRQ_RSS_TYPE_IPV6UDP:
        type = NetPktRssIpV6Udp;
        break;
    default:
        g_assert_not_reached();
    }

    return net_rx_pkt_calc_rss_hash(pkt, type, (uint8_t *) &core->mac[RSSRK]);
}

static void
igb_rss_parse_packet(IGBCore *core, struct NetRxPkt *pkt, bool tx,
                     E1000E_RSSInfo *info)
{
    trace_e1000e_rx_rss_started();

    if (tx || !igb_rss_enabled(core)) {
        info->enabled = false;
        info->hash = 0;
        info->queue = 0;
        info->type = 0;
        trace_e1000e_rx_rss_disabled();
        return;
    }

    info->enabled = true;

    info->type = igb_rss_get_hash_type(core, pkt);

    trace_e1000e_rx_rss_type(info->type);

    if (info->type == E1000_MRQ_RSS_TYPE_NONE) {
        info->hash = 0;
        info->queue = 0;
        return;
    }

    info->hash = igb_rss_calc_hash(core, pkt, info);
    info->queue = E1000_RSS_QUEUE(&core->mac[RETA], info->hash);
}

static void
igb_tx_insert_vlan(IGBCore *core, uint16_t qn, igb_tx *tx,
    uint16_t vlan, bool insert_vlan)
{
    if (core->mac[MRQC] & 1) {
        uint16_t pool = qn % IGB_NUM_VM_POOLS;

        if (core->mac[VMVIR0 + pool] & E1000_VMVIR_VLANA_DEFAULT) {
            /* always insert default VLAN */
            insert_vlan = true;
            vlan = core->mac[VMVIR0 + pool] & 0xffff;
        } else if (core->mac[VMVIR0 + pool] & E1000_VMVIR_VLANA_NEVER) {
            insert_vlan = false;
        }
    }

    if (insert_vlan) {
        net_tx_pkt_setup_vlan_header_ex(tx->tx_pkt, vlan,
            core->mac[VET] & 0xffff);
    }
}

static bool
igb_setup_tx_offloads(IGBCore *core, igb_tx *tx)
{
    uint32_t idx = (tx->first_olinfo_status >> 4) & 1;

    if (tx->first_cmd_type_len & E1000_ADVTXD_DCMD_TSE) {
        uint32_t mss = tx->ctx[idx].mss_l4len_idx >> E1000_ADVTXD_MSS_SHIFT;
        if (!net_tx_pkt_build_vheader(tx->tx_pkt, true, true, mss)) {
            return false;
        }

        net_tx_pkt_update_ip_checksums(tx->tx_pkt);
        e1000x_inc_reg_if_not_full(core->mac, TSCTC);
        return true;
    }

    if ((tx->first_olinfo_status & E1000_ADVTXD_POTS_TXSM) &&
        !((tx->ctx[idx].type_tucmd_mlhl & E1000_ADVTXD_TUCMD_L4T_SCTP) ?
          net_tx_pkt_update_sctp_checksum(tx->tx_pkt) :
          net_tx_pkt_build_vheader(tx->tx_pkt, false, true, 0))) {
        return false;
    }

    if (tx->first_olinfo_status & E1000_ADVTXD_POTS_IXSM) {
        net_tx_pkt_update_ip_hdr_checksum(tx->tx_pkt);
    }

    return true;
}

static void igb_tx_pkt_mac_callback(void *core,
                                    const struct iovec *iov,
                                    int iovcnt,
                                    const struct iovec *virt_iov,
                                    int virt_iovcnt)
{
    igb_receive_internal(static_cast<IGBCore *>(core), virt_iov, virt_iovcnt, true, NULL);
}

static void igb_tx_pkt_vmdq_callback(void *opaque,
                                     const struct iovec *iov,
                                     int iovcnt,
                                     const struct iovec *virt_iov,
                                     int virt_iovcnt)
{
    IGBTxPktVmdqCallbackContext *context = static_cast<IGBTxPktVmdqCallbackContext *>(opaque);
    bool external_tx;

    igb_receive_internal(context->core, virt_iov, virt_iovcnt, true,
                         &external_tx);

    if (external_tx) {
        if (context->core->has_vnet) {
            qemu_sendv_packet(context->nc, virt_iov, virt_iovcnt);
        } else {
            qemu_sendv_packet(context->nc, iov, iovcnt);
        }
    }
}

/* TX Packets Switching (7.10.3.6) */
static bool igb_tx_pkt_switch(IGBCore *core, igb_tx *tx,
                              NetClientState *nc)
{
    IGBTxPktVmdqCallbackContext context;

    /* TX switching is only used to serve VM to VM traffic. */
    if (!(core->mac[MRQC] & 1)) {
        goto send_out;
    }

    /* TX switching requires DTXSWC.Loopback_en bit enabled. */
    if (!(core->mac[DTXSWC] & E1000_DTXSWC_VMDQ_LOOPBACK_EN)) {
        goto send_out;
    }

    context.core = core;
    context.nc = nc;

    return net_tx_pkt_send_custom(tx->tx_pkt, false,
                                  igb_tx_pkt_vmdq_callback, &context);

send_out:
    return net_tx_pkt_send(tx->tx_pkt, nc);
}

static bool
igb_tx_pkt_send(IGBCore *core, igb_tx *tx, int queue_index)
{
    int target_queue = MIN(core->max_queue_num, queue_index);
    NetClientState *queue = qemu_get_subqueue(core->owner_nic, target_queue);

    if (!igb_setup_tx_offloads(core, tx)) {
        return false;
    }

    net_tx_pkt_dump(tx->tx_pkt);

    if ((core->phy[MII_BMCR] & MII_BMCR_LOOPBACK) ||
        ((core->mac[RCTL] & E1000_RCTL_LBM_MAC) == E1000_RCTL_LBM_MAC)) {
        return net_tx_pkt_send_custom(tx->tx_pkt, false,
                                      igb_tx_pkt_mac_callback, core);
    } else {
        return igb_tx_pkt_switch(core, tx, queue);
    }
}

static void
igb_on_tx_done_update_stats(IGBCore *core, struct NetTxPkt *tx_pkt, int qn)
{
    static const int PTCregs[6] = { PTC64, PTC127, PTC255, PTC511,
                                    PTC1023, PTC1522 };

    size_t tot_len = net_tx_pkt_get_total_len(tx_pkt) + 4;

    e1000x_increase_size_stats(core->mac, PTCregs, tot_len);
    e1000x_inc_reg_if_not_full(core->mac, TPT);
    e1000x_grow_8reg_if_not_full(core->mac, TOTL, tot_len);

    switch (net_tx_pkt_get_packet_type(tx_pkt)) {
    case ETH_PKT_BCAST:
        e1000x_inc_reg_if_not_full(core->mac, BPTC);
        break;
    case ETH_PKT_MCAST:
        e1000x_inc_reg_if_not_full(core->mac, MPTC);
        break;
    case ETH_PKT_UCAST:
        break;
    default:
        g_assert_not_reached();
    }

    e1000x_inc_reg_if_not_full(core->mac, GPTC);
    e1000x_grow_8reg_if_not_full(core->mac, GOTCL, tot_len);

    if (core->mac[MRQC] & 1) {
        uint16_t pool = qn % IGB_NUM_VM_POOLS;

        core->mac[PVFGOTC0 + (pool * 64)] += tot_len;
        core->mac[PVFGPTC0 + (pool * 64)]++;
    }
}

static void
igb_process_tx_desc(IGBCore *core,
                    PCIDevice *dev,
                    igb_tx *tx,
                    union e1000_adv_tx_desc *tx_desc,
                    int queue_index)
{
    struct e1000_adv_tx_context_desc *tx_ctx_desc;
    uint32_t cmd_type_len;
    uint32_t idx;
    uint64_t buffer_addr;
    uint16_t length;

    cmd_type_len = le32_to_cpu(tx_desc->read.cmd_type_len);

    if (cmd_type_len & E1000_ADVTXD_DCMD_DEXT) {
        if ((cmd_type_len & E1000_ADVTXD_DTYP_DATA) ==
            E1000_ADVTXD_DTYP_DATA) {
            /* advanced transmit data descriptor */
            if (tx->first) {
                tx->first_cmd_type_len = cmd_type_len;
                tx->first_olinfo_status = le32_to_cpu(tx_desc->read.olinfo_status);
                tx->first = false;
            }
        } else if ((cmd_type_len & E1000_ADVTXD_DTYP_CTXT) ==
                   E1000_ADVTXD_DTYP_CTXT) {
            /* advanced transmit context descriptor */
            tx_ctx_desc = (struct e1000_adv_tx_context_desc *)tx_desc;
            idx = (le32_to_cpu(tx_ctx_desc->mss_l4len_idx) >> 4) & 1;
            tx->ctx[idx].vlan_macip_lens = le32_to_cpu(tx_ctx_desc->vlan_macip_lens);
            tx->ctx[idx].seqnum_seed = le32_to_cpu(tx_ctx_desc->seqnum_seed);
            tx->ctx[idx].type_tucmd_mlhl = le32_to_cpu(tx_ctx_desc->type_tucmd_mlhl);
            tx->ctx[idx].mss_l4len_idx = le32_to_cpu(tx_ctx_desc->mss_l4len_idx);
            return;
        } else {
            /* unknown descriptor type */
            return;
        }
    } else {
        /* legacy descriptor */

        /* TODO: Implement a support for legacy descriptors (7.2.2.1). */
    }

    buffer_addr = le64_to_cpu(tx_desc->read.buffer_addr);
    length = cmd_type_len & 0xFFFF;

    if (!tx->skip_cp) {
        if (!net_tx_pkt_add_raw_fragment_pci(tx->tx_pkt, dev,
                                             buffer_addr, length)) {
            tx->skip_cp = true;
        }
    }

    if (cmd_type_len & E1000_TXD_CMD_EOP) {
        if (!tx->skip_cp && net_tx_pkt_parse(tx->tx_pkt)) {
            idx = (tx->first_olinfo_status >> 4) & 1;
            igb_tx_insert_vlan(core, queue_index, tx,
                tx->ctx[idx].vlan_macip_lens >> IGB_TX_FLAGS_VLAN_SHIFT,
                !!(tx->first_cmd_type_len & E1000_TXD_CMD_VLE));

            if ((tx->first_cmd_type_len & E1000_ADVTXD_MAC_TSTAMP) &&
                (core->mac[TSYNCTXCTL] & E1000_TSYNCTXCTL_ENABLED) &&
                !(core->mac[TSYNCTXCTL] & E1000_TSYNCTXCTL_VALID)) {
                core->mac[TSYNCTXCTL] |= E1000_TSYNCTXCTL_VALID;
                e1000x_timestamp(core->mac, core->timadj, TXSTMPL, TXSTMPH);
            }

            if (igb_tx_pkt_send(core, tx, queue_index)) {
                igb_on_tx_done_update_stats(core, tx->tx_pkt, queue_index);
            }
        }

        tx->first = true;
        tx->skip_cp = false;
        net_tx_pkt_reset(tx->tx_pkt, net_tx_pkt_unmap_frag_pci, dev);
    }
}

static uint32_t igb_tx_wb_eic(IGBCore *core, int queue_idx)
{
    uint32_t n, ent = 0;

    n = igb_ivar_entry_tx(queue_idx);
    ent = (core->mac[IVAR0 + n / 4] >> (8 * (n % 4))) & 0xff;

    return (ent & E1000_IVAR_VALID) ? BIT(ent & 0x1f) : 0;
}

static uint32_t igb_rx_wb_eic(IGBCore *core, int queue_idx)
{
    uint32_t n, ent = 0;

    n = igb_ivar_entry_rx(queue_idx);
    ent = (core->mac[IVAR0 + n / 4] >> (8 * (n % 4))) & 0xff;

    return (ent & E1000_IVAR_VALID) ? BIT(ent & 0x1f) : 0;
}

static inline bool
igb_ring_empty(IGBCore *core, const E1000ERingInfo *r)
{
    return core->mac[r->dh] == core->mac[r->dt] ||
                core->mac[r->dt] >= core->mac[r->dlen] / E1000_RING_DESC_LEN;
}

static inline uint64_t
igb_ring_base(IGBCore *core, const E1000ERingInfo *r)
{
    uint64_t bah = core->mac[r->dbah];
    uint64_t bal = core->mac[r->dbal];

    return (bah << 32) + bal;
}

static inline uint64_t
igb_ring_head_descr(IGBCore *core, const E1000ERingInfo *r)
{
    return igb_ring_base(core, r) + E1000_RING_DESC_LEN * core->mac[r->dh];
}

static inline void
igb_ring_advance(IGBCore *core, const E1000ERingInfo *r, uint32_t count)
{
    core->mac[r->dh] += count;

    if (core->mac[r->dh] * E1000_RING_DESC_LEN >= core->mac[r->dlen]) {
        core->mac[r->dh] = 0;
    }
}

static inline uint32_t
igb_ring_free_descr_num(IGBCore *core, const E1000ERingInfo *r)
{
    trace_e1000e_ring_free_space(r->idx, core->mac[r->dlen],
                                 core->mac[r->dh],  core->mac[r->dt]);

    if (core->mac[r->dh] <= core->mac[r->dt]) {
        return core->mac[r->dt] - core->mac[r->dh];
    }

    if (core->mac[r->dh] > core->mac[r->dt]) {
        return core->mac[r->dlen] / E1000_RING_DESC_LEN +
               core->mac[r->dt] - core->mac[r->dh];
    }

    g_assert_not_reached();
}

static inline bool
igb_ring_enabled(IGBCore *core, const E1000ERingInfo *r)
{
    return core->mac[r->dlen] > 0;
}

typedef struct IGB_TxRing_st {
    const E1000ERingInfo *i;
    igb_tx *tx;
} IGB_TxRing;

static inline int
igb_mq_queue_idx(int base_reg_idx, int reg_idx)
{
    return (reg_idx - base_reg_idx) / 16;
}

static inline void
igb_tx_ring_init(IGBCore *core, IGB_TxRing *txr, int idx)
{
    static const E1000ERingInfo i[IGB_NUM_QUEUES] = {
        { TDBAH0, TDBAL0, TDLEN0, TDH0, TDT0, 0 },
        { TDBAH1, TDBAL1, TDLEN1, TDH1, TDT1, 1 },
        { TDBAH2, TDBAL2, TDLEN2, TDH2, TDT2, 2 },
        { TDBAH3, TDBAL3, TDLEN3, TDH3, TDT3, 3 },
        { TDBAH4, TDBAL4, TDLEN4, TDH4, TDT4, 4 },
        { TDBAH5, TDBAL5, TDLEN5, TDH5, TDT5, 5 },
        { TDBAH6, TDBAL6, TDLEN6, TDH6, TDT6, 6 },
        { TDBAH7, TDBAL7, TDLEN7, TDH7, TDT7, 7 },
        { TDBAH8, TDBAL8, TDLEN8, TDH8, TDT8, 8 },
        { TDBAH9, TDBAL9, TDLEN9, TDH9, TDT9, 9 },
        { TDBAH10, TDBAL10, TDLEN10, TDH10, TDT10, 10 },
        { TDBAH11, TDBAL11, TDLEN11, TDH11, TDT11, 11 },
        { TDBAH12, TDBAL12, TDLEN12, TDH12, TDT12, 12 },
        { TDBAH13, TDBAL13, TDLEN13, TDH13, TDT13, 13 },
        { TDBAH14, TDBAL14, TDLEN14, TDH14, TDT14, 14 },
        { TDBAH15, TDBAL15, TDLEN15, TDH15, TDT15, 15 }
    };

    assert(idx < ARRAY_SIZE(i));

    txr->i     = &i[idx];
    txr->tx    = &core->tx[idx];
}

typedef struct E1000E_RxRing_st {
    const E1000ERingInfo *i;
} E1000E_RxRing;

static inline void
igb_rx_ring_init(IGBCore *core, E1000E_RxRing *rxr, int idx)
{
    static const E1000ERingInfo i[IGB_NUM_QUEUES] = {
        { RDBAH0, RDBAL0, RDLEN0, RDH0, RDT0, 0 },
        { RDBAH1, RDBAL1, RDLEN1, RDH1, RDT1, 1 },
        { RDBAH2, RDBAL2, RDLEN2, RDH2, RDT2, 2 },
        { RDBAH3, RDBAL3, RDLEN3, RDH3, RDT3, 3 },
        { RDBAH4, RDBAL4, RDLEN4, RDH4, RDT4, 4 },
        { RDBAH5, RDBAL5, RDLEN5, RDH5, RDT5, 5 },
        { RDBAH6, RDBAL6, RDLEN6, RDH6, RDT6, 6 },
        { RDBAH7, RDBAL7, RDLEN7, RDH7, RDT7, 7 },
        { RDBAH8, RDBAL8, RDLEN8, RDH8, RDT8, 8 },
        { RDBAH9, RDBAL9, RDLEN9, RDH9, RDT9, 9 },
        { RDBAH10, RDBAL10, RDLEN10, RDH10, RDT10, 10 },
        { RDBAH11, RDBAL11, RDLEN11, RDH11, RDT11, 11 },
        { RDBAH12, RDBAL12, RDLEN12, RDH12, RDT12, 12 },
        { RDBAH13, RDBAL13, RDLEN13, RDH13, RDT13, 13 },
        { RDBAH14, RDBAL14, RDLEN14, RDH14, RDT14, 14 },
        { RDBAH15, RDBAL15, RDLEN15, RDH15, RDT15, 15 }
    };

    assert(idx < ARRAY_SIZE(i));

    rxr->i      = &i[idx];
}

static uint32_t
igb_txdesc_writeback(IGBCore *core, dma_addr_t base,
                     union e1000_adv_tx_desc *tx_desc,
                     const E1000ERingInfo *txi)
{
    PCIDevice *d;
    uint32_t cmd_type_len = le32_to_cpu(tx_desc->read.cmd_type_len);
    uint64_t tdwba;

    tdwba = core->mac[E1000_TDWBAL(txi->idx) >> 2];
    tdwba |= (uint64_t)core->mac[E1000_TDWBAH(txi->idx) >> 2] << 32;

    if (!(cmd_type_len & E1000_TXD_CMD_RS)) {
        return 0;
    }

    d = pcie_sriov_get_vf_at_index(core->owner, txi->idx % 8);
    if (!d) {
        d = core->owner;
    }

    if (tdwba & 1) {
        uint32_t buffer = cpu_to_le32(core->mac[txi->dh]);
        pci_dma_write(d, tdwba & ~3, &buffer, sizeof(buffer));
    } else {
        uint32_t status = le32_to_cpu(tx_desc->wb.status) | E1000_TXD_STAT_DD;

        tx_desc->wb.status = cpu_to_le32(status);
        pci_dma_write(d, base + offsetof(union e1000_adv_tx_desc, wb),
            &tx_desc->wb, sizeof(tx_desc->wb));
    }

    return igb_tx_wb_eic(core, txi->idx);
}

static inline bool
igb_tx_enabled(IGBCore *core, const E1000ERingInfo *txi)
{
    bool vmdq = core->mac[MRQC] & 1;
    uint16_t qn = txi->idx;
    uint16_t pool = qn % IGB_NUM_VM_POOLS;

    return (core->mac[TCTL] & E1000_TCTL_EN) &&
        (!vmdq || core->mac[VFTE] & BIT(pool)) &&
        (core->mac[TXDCTL0 + (qn * 16)] & E1000_TXDCTL_QUEUE_ENABLE);
}

static void
igb_start_xmit(IGBCore *core, const IGB_TxRing *txr)
{
    PCIDevice *d;
    dma_addr_t base;
    union e1000_adv_tx_desc desc;
    const E1000ERingInfo *txi = txr->i;
    uint32_t eic = 0;

    if (!igb_tx_enabled(core, txi)) {
        trace_e1000e_tx_disabled();
        return;
    }

    d = pcie_sriov_get_vf_at_index(core->owner, txi->idx % 8);
    if (!d) {
        d = core->owner;
    }

    while (!igb_ring_empty(core, txi)) {
        base = igb_ring_head_descr(core, txi);

        pci_dma_read(d, base, &desc, sizeof(desc));

        trace_e1000e_tx_descr((void *)(intptr_t)desc.read.buffer_addr,
                              desc.read.cmd_type_len, desc.wb.status);

        igb_process_tx_desc(core, d, txr->tx, &desc, txi->idx);
        igb_ring_advance(core, txi, 1);
        eic |= igb_txdesc_writeback(core, base, &desc, txi);
    }

    if (eic) {
        igb_raise_interrupts(core, EICR, eic);
        igb_raise_interrupts(core, ICR, E1000_ICR_TXDW);
    }

    net_tx_pkt_reset(txr->tx->tx_pkt, net_tx_pkt_unmap_frag_pci, d);
}

static uint32_t
igb_rxbufsize(IGBCore *core, const E1000ERingInfo *r)
{
    uint32_t srrctl = core->mac[E1000_SRRCTL(r->idx) >> 2];
    uint32_t bsizepkt = srrctl & E1000_SRRCTL_BSIZEPKT_MASK;
    if (bsizepkt) {
        return bsizepkt << E1000_SRRCTL_BSIZEPKT_SHIFT;
    }

    return e1000x_rxbufsize(core->mac[RCTL]);
}

static bool
igb_has_rxbufs(IGBCore *core, const E1000ERingInfo *r, size_t total_size)
{
    uint32_t bufs = igb_ring_free_descr_num(core, r);
    uint32_t bufsize = igb_rxbufsize(core, r);

    trace_e1000e_rx_has_buffers(r->idx, bufs, total_size, bufsize);

    return total_size <= bufs / (core->rx_desc_len / E1000_MIN_RX_DESC_LEN) *
                         bufsize;
}

static uint32_t
igb_rxhdrbufsize(IGBCore *core, const E1000ERingInfo *r)
{
    uint32_t srrctl = core->mac[E1000_SRRCTL(r->idx) >> 2];
    return (srrctl & E1000_SRRCTL_BSIZEHDRSIZE_MASK) >>
           E1000_SRRCTL_BSIZEHDRSIZE_SHIFT;
}

void
igb_start_recv(IGBCore *core)
{
    int i;

    trace_e1000e_rx_start_recv();

    for (i = 0; i <= core->max_queue_num; i++) {
        qemu_flush_queued_packets(qemu_get_subqueue(core->owner_nic, i));
    }
}

bool
igb_can_receive(IGBCore *core)
{
    int i;

    if (!e1000x_rx_ready(core->owner, core->mac)) {
        return false;
    }

    for (i = 0; i < IGB_NUM_QUEUES; i++) {
        E1000E_RxRing rxr;
        if (!(core->mac[RXDCTL0 + (i * 16)] & E1000_RXDCTL_QUEUE_ENABLE)) {
            continue;
        }

        igb_rx_ring_init(core, &rxr, i);
        if (igb_ring_enabled(core, rxr.i) && igb_has_rxbufs(core, rxr.i, 1)) {
            trace_e1000e_rx_can_recv();
            return true;
        }
    }

    trace_e1000e_rx_can_recv_rings_full();
    return false;
}

ssize_t
igb_receive(IGBCore *core, const uint8_t *buf, size_t size)
{
    const struct iovec iov = {
        .iov_base = (uint8_t *)buf,
        .iov_len = size
    };

    return igb_receive_iov(core, &iov, 1);
}

static inline bool
igb_rx_l3_cso_enabled(IGBCore *core)
{
    return !!(core->mac[RXCSUM] & E1000_RXCSUM_IPOFLD);
}

static inline bool
igb_rx_l4_cso_enabled(IGBCore *core)
{
    return !!(core->mac[RXCSUM] & E1000_RXCSUM_TUOFLD);
}

static bool igb_rx_is_oversized(IGBCore *core, const struct eth_header *ehdr,
                                size_t size, size_t vlan_num,
                                bool lpe, uint16_t rlpml)
{
    size_t vlan_header_size = sizeof(struct vlan_header) * vlan_num;
    size_t header_size = sizeof(struct eth_header) + vlan_header_size;
    return lpe ? size + ETH_FCS_LEN > rlpml : size > header_size + ETH_MTU;
}

static uint16_t igb_receive_assign(IGBCore *core, const struct iovec *iov,
                                   size_t iovcnt, size_t iov_ofs,
                                   const L2Header *l2_header, size_t size,
                                   E1000E_RSSInfo *rss_info,
                                   uint16_t *etqf, bool *ts, bool *external_tx)
{
    static const int ta_shift[] = { 4, 3, 2, 0 };
    const struct eth_header *ehdr = &l2_header->eth;
    uint32_t f, ra[2], *macp, rctl = core->mac[RCTL];
    uint16_t queues = 0;
    uint16_t oversized = 0;
    size_t vlan_num = 0;
    PTP2 ptp2;
    bool lpe;
    uint16_t rlpml;
    int i;

    memset(rss_info, 0, sizeof(E1000E_RSSInfo));
    *ts = false;

    if (external_tx) {
        *external_tx = true;
    }

    if (core->mac[CTRL_EXT] & BIT(26)) {
        if (be16_to_cpu(ehdr->h_proto) == core->mac[VET] >> 16 &&
            be16_to_cpu(l2_header->vlan[0].h_proto) == (core->mac[VET] & 0xffff)) {
            vlan_num = 2;
        }
    } else {
        if (be16_to_cpu(ehdr->h_proto) == (core->mac[VET] & 0xffff)) {
            vlan_num = 1;
        }
    }

    lpe = !!(core->mac[RCTL] & E1000_RCTL_LPE);
    rlpml = core->mac[RLPML];
    if (!(core->mac[RCTL] & E1000_RCTL_SBP) &&
        igb_rx_is_oversized(core, ehdr, size, vlan_num, lpe, rlpml)) {
        trace_e1000x_rx_oversized(size);
        return queues;
    }

    for (*etqf = 0; *etqf < 8; (*etqf)++) {
        if ((core->mac[ETQF0 + *etqf] & E1000_ETQF_FILTER_ENABLE) &&
            be16_to_cpu(ehdr->h_proto) == (core->mac[ETQF0 + *etqf] & E1000_ETQF_ETYPE_MASK)) {
            if ((core->mac[ETQF0 + *etqf] & E1000_ETQF_1588) &&
                (core->mac[TSYNCRXCTL] & E1000_TSYNCRXCTL_ENABLED) &&
                !(core->mac[TSYNCRXCTL] & E1000_TSYNCRXCTL_VALID) &&
                iov_to_buf(iov, iovcnt, iov_ofs + ETH_HLEN, &ptp2, sizeof(ptp2)) >= sizeof(ptp2) &&
                (ptp2.version_ptp & 15) == 2 &&
                ptp2.message_id_transport_specific == ((core->mac[TSYNCRXCFG] >> 8) & 255)) {
                e1000x_timestamp(core->mac, core->timadj, RXSTMPL, RXSTMPH);
                *ts = true;
                core->mac[TSYNCRXCTL] |= E1000_TSYNCRXCTL_VALID;
                core->mac[RXSATRL] = le32_to_cpu(ptp2.source_uuid_lo);
                core->mac[RXSATRH] = le16_to_cpu(ptp2.source_uuid_hi) |
                                     (le16_to_cpu(ptp2.sequence_id) << 16);
            }
            break;
        }
    }

    if (vlan_num &&
        !e1000x_rx_vlan_filter(core->mac, l2_header->vlan + vlan_num - 1)) {
        return queues;
    }

    if (core->mac[MRQC] & 1) {
        if (is_broadcast_ether_addr(ehdr->h_dest)) {
            for (i = 0; i < IGB_NUM_VM_POOLS; i++) {
                if (core->mac[VMOLR0 + i] & E1000_VMOLR_BAM) {
                    queues |= BIT(i);
                }
            }
        } else {
            for (macp = core->mac + RA; macp < core->mac + RA + 32; macp += 2) {
                if (!(macp[1] & E1000_RAH_AV)) {
                    continue;
                }
                ra[0] = cpu_to_le32(macp[0]);
                ra[1] = cpu_to_le32(macp[1]);
                if (!memcmp(ehdr->h_dest, (uint8_t *)ra, ETH_ALEN)) {
                    queues |= (macp[1] & E1000_RAH_POOL_MASK) / E1000_RAH_POOL_1;
                }
            }

            for (macp = core->mac + RA2; macp < core->mac + RA2 + 16; macp += 2) {
                if (!(macp[1] & E1000_RAH_AV)) {
                    continue;
                }
                ra[0] = cpu_to_le32(macp[0]);
                ra[1] = cpu_to_le32(macp[1]);
                if (!memcmp(ehdr->h_dest, (uint8_t *)ra, ETH_ALEN)) {
                    queues |= (macp[1] & E1000_RAH_POOL_MASK) / E1000_RAH_POOL_1;
                }
            }

            if (!queues) {
                macp = core->mac + (is_multicast_ether_addr(ehdr->h_dest) ? MTA : UTA);

                f = ta_shift[(rctl >> E1000_RCTL_MO_SHIFT) & 3];
                f = (((ehdr->h_dest[5] << 8) | ehdr->h_dest[4]) >> f) & 0xfff;
                if (macp[f >> 5] & (1 << (f & 0x1f))) {
                    for (i = 0; i < IGB_NUM_VM_POOLS; i++) {
                        if (core->mac[VMOLR0 + i] & E1000_VMOLR_ROMPE) {
                            queues |= BIT(i);
                        }
                    }
                }
            } else if (is_unicast_ether_addr(ehdr->h_dest) && external_tx) {
                *external_tx = false;
            }
        }

        if (e1000x_vlan_rx_filter_enabled(core->mac)) {
            uint16_t mask = 0;

            if (vlan_num) {
                uint16_t vid = be16_to_cpu(l2_header->vlan[vlan_num - 1].h_tci) & VLAN_VID_MASK;

                for (i = 0; i < E1000_VLVF_ARRAY_SIZE; i++) {
                    if ((core->mac[VLVF0 + i] & E1000_VLVF_VLANID_MASK) == vid &&
                        (core->mac[VLVF0 + i] & E1000_VLVF_VLANID_ENABLE)) {
                        uint32_t poolsel = core->mac[VLVF0 + i] & E1000_VLVF_POOLSEL_MASK;
                        mask |= poolsel >> E1000_VLVF_POOLSEL_SHIFT;
                    }
                }
            } else {
                for (i = 0; i < IGB_NUM_VM_POOLS; i++) {
                    if (core->mac[VMOLR0 + i] & E1000_VMOLR_AUPE) {
                        mask |= BIT(i);
                    }
                }
            }

            queues &= mask;
        }

        if (is_unicast_ether_addr(ehdr->h_dest) && !queues && !external_tx &&
            !(core->mac[VT_CTL] & E1000_VT_CTL_DISABLE_DEF_POOL)) {
            uint32_t def_pl = core->mac[VT_CTL] & E1000_VT_CTL_DEFAULT_POOL_MASK;
            queues = BIT(def_pl >> E1000_VT_CTL_DEFAULT_POOL_SHIFT);
        }

        queues &= core->mac[VFRE];
        if (queues) {
            for (i = 0; i < IGB_NUM_VM_POOLS; i++) {
                lpe = !!(core->mac[VMOLR0 + i] & E1000_VMOLR_LPE);
                rlpml = core->mac[VMOLR0 + i] & E1000_VMOLR_RLPML_MASK;
                if ((queues & BIT(i)) &&
                    igb_rx_is_oversized(core, ehdr, size, vlan_num,
                                        lpe, rlpml)) {
                    oversized |= BIT(i);
                }
            }
            /* 8.19.37 increment ROC if packet is oversized for all queues */
            if (oversized == queues) {
                trace_e1000x_rx_oversized(size);
                e1000x_inc_reg_if_not_full(core->mac, ROC);
            }
            queues &= ~oversized;
        }

        if (queues) {
            igb_rss_parse_packet(core, core->rx_pkt,
                                 external_tx != NULL, rss_info);
            /* Sec 8.26.1: PQn = VFn + VQn*8 */
            if (rss_info->queue & 1) {
                for (i = 0; i < IGB_NUM_VM_POOLS; i++) {
                    if ((queues & BIT(i)) &&
                        (core->mac[VMOLR0 + i] & E1000_VMOLR_RSSE)) {
                        queues |= BIT(i + IGB_NUM_VM_POOLS);
                        queues &= ~BIT(i);
                    }
                }
            }
        }
    } else {
        bool accepted = e1000x_rx_group_filter(core->mac, ehdr);
        if (!accepted) {
            for (macp = core->mac + RA2; macp < core->mac + RA2 + 16; macp += 2) {
                if (!(macp[1] & E1000_RAH_AV)) {
                    continue;
                }
                ra[0] = cpu_to_le32(macp[0]);
                ra[1] = cpu_to_le32(macp[1]);
                if (!memcmp(ehdr->h_dest, (uint8_t *)ra, ETH_ALEN)) {
                    trace_e1000x_rx_flt_ucast_match((int)(macp - core->mac - RA2) / 2,
                                                    MAC_ARG(ehdr->h_dest));

                    accepted = true;
                    break;
                }
            }
        }

        if (accepted) {
            igb_rss_parse_packet(core, core->rx_pkt, false, rss_info);
            queues = BIT(rss_info->queue);
        }
    }

    return queues;
}

static inline void
igb_read_lgcy_rx_descr(IGBCore *core, struct e1000_rx_desc *desc,
                       hwaddr *buff_addr)
{
    *buff_addr = le64_to_cpu(desc->buffer_addr);
}

static inline void
igb_read_adv_rx_single_buf_descr(IGBCore *core, union e1000_adv_rx_desc *desc,
                                 hwaddr *buff_addr)
{
    *buff_addr = le64_to_cpu(desc->read.pkt_addr);
}

static inline void
igb_read_adv_rx_split_buf_descr(IGBCore *core, union e1000_adv_rx_desc *desc,
                                hwaddr *buff_addr)
{
    buff_addr[0] = le64_to_cpu(desc->read.hdr_addr);
    buff_addr[1] = le64_to_cpu(desc->read.pkt_addr);
}

typedef struct IGBBAState {
    uint16_t written[IGB_MAX_PS_BUFFERS];
    uint8_t cur_idx;
} IGBBAState;

typedef struct IGBSplitDescriptorData {
    bool sph;
    bool hbo;
    size_t hdr_len;
} IGBSplitDescriptorData;

typedef struct IGBPacketRxDMAState {
    size_t size;
    size_t total_size;
    size_t ps_hdr_len;
    size_t desc_size;
    size_t desc_offset;
    uint32_t rx_desc_packet_buf_size;
    uint32_t rx_desc_header_buf_size;
    struct iovec *iov;
    size_t iov_ofs;
    bool do_ps;
    bool is_first;
    IGBBAState bastate;
    hwaddr ba[IGB_MAX_PS_BUFFERS];
    IGBSplitDescriptorData ps_desc_data;
} IGBPacketRxDMAState;

static inline void
igb_read_rx_descr(IGBCore *core,
                  union e1000_rx_desc_union *desc,
                  IGBPacketRxDMAState *pdma_st,
                  const E1000ERingInfo *r)
{
    uint32_t desc_type;

    if (igb_rx_use_legacy_descriptor(core)) {
        igb_read_lgcy_rx_descr(core, &desc->legacy, &pdma_st->ba[1]);
        pdma_st->ba[0] = 0;
        return;
    }

    /* advanced header split descriptor */
    if (igb_rx_use_ps_descriptor(core, r)) {
        igb_read_adv_rx_split_buf_descr(core, &desc->adv, &pdma_st->ba[0]);
        return;
    }

    /* descriptor replication modes not supported */
    desc_type = igb_rx_queue_desctyp_get(core, r);
    if (desc_type != E1000_SRRCTL_DESCTYPE_ADV_ONEBUF) {
        trace_igb_wrn_rx_desc_modes_not_supp(desc_type);
    }

    /* advanced single buffer descriptor */
    igb_read_adv_rx_single_buf_descr(core, &desc->adv, &pdma_st->ba[1]);
    pdma_st->ba[0] = 0;
}

static void
igb_verify_csum_in_sw(IGBCore *core,
                      struct NetRxPkt *pkt,
                      uint32_t *status_flags,
                      EthL4HdrProto l4hdr_proto)
{
    bool csum_valid;
    uint32_t csum_error;

    if (igb_rx_l3_cso_enabled(core)) {
        if (!net_rx_pkt_validate_l3_csum(pkt, &csum_valid)) {
            trace_e1000e_rx_metadata_l3_csum_validation_failed();
        } else {
            csum_error = csum_valid ? 0 : E1000_RXDEXT_STATERR_IPE;
            *status_flags |= E1000_RXD_STAT_IPCS | csum_error;
        }
    } else {
        trace_e1000e_rx_metadata_l3_cso_disabled();
    }

    if (!igb_rx_l4_cso_enabled(core)) {
        trace_e1000e_rx_metadata_l4_cso_disabled();
        return;
    }

    if (!net_rx_pkt_validate_l4_csum(pkt, &csum_valid)) {
        trace_e1000e_rx_metadata_l4_csum_validation_failed();
        return;
    }

    csum_error = csum_valid ? 0 : E1000_RXDEXT_STATERR_TCPE;
    *status_flags |= E1000_RXD_STAT_TCPCS | csum_error;

    if (l4hdr_proto == ETH_L4_HDR_PROTO_UDP) {
        *status_flags |= E1000_RXD_STAT_UDPCS;
    }
}

static void
igb_build_rx_metadata_common(IGBCore *core,
                             struct NetRxPkt *pkt,
                             bool is_eop,
                             uint32_t *status_flags,
                             uint16_t *vlan_tag)
{
    struct virtio_net_hdr *vhdr;
    bool hasip4, hasip6, csum_valid;
    EthL4HdrProto l4hdr_proto;

    *status_flags = E1000_RXD_STAT_DD;

    /* No additional metadata needed for non-EOP descriptors */
    if (!is_eop) {
        goto func_exit;
    }

    *status_flags |= E1000_RXD_STAT_EOP;

    net_rx_pkt_get_protocols(pkt, &hasip4, &hasip6, &l4hdr_proto);
    trace_e1000e_rx_metadata_protocols(hasip4, hasip6, l4hdr_proto);

    /* VLAN state */
    if (net_rx_pkt_is_vlan_stripped(pkt)) {
        *status_flags |= E1000_RXD_STAT_VP;
        *vlan_tag = cpu_to_le16(net_rx_pkt_get_vlan_tag(pkt));
        trace_e1000e_rx_metadata_vlan(*vlan_tag);
    }

    /* RX CSO information */
    if (hasip6 && (core->mac[RFCTL] & E1000_RFCTL_IPV6_XSUM_DIS)) {
        trace_e1000e_rx_metadata_ipv6_sum_disabled();
        goto func_exit;
    }

    vhdr = net_rx_pkt_get_vhdr(pkt);

    if (!(vhdr->flags & VIRTIO_NET_HDR_F_DATA_VALID) &&
        !(vhdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM)) {
        trace_e1000e_rx_metadata_virthdr_no_csum_info();
        igb_verify_csum_in_sw(core, pkt, status_flags, l4hdr_proto);
        goto func_exit;
    }

    if (igb_rx_l3_cso_enabled(core)) {
        *status_flags |= hasip4 ? E1000_RXD_STAT_IPCS : 0;
    } else {
        trace_e1000e_rx_metadata_l3_cso_disabled();
    }

    if (igb_rx_l4_cso_enabled(core)) {
        switch (l4hdr_proto) {
        case ETH_L4_HDR_PROTO_SCTP:
            if (!net_rx_pkt_validate_l4_csum(pkt, &csum_valid)) {
                trace_e1000e_rx_metadata_l4_csum_validation_failed();
                goto func_exit;
            }
            if (!csum_valid) {
                *status_flags |= E1000_RXDEXT_STATERR_TCPE;
            }
            /* fall through */
        case ETH_L4_HDR_PROTO_TCP:
            *status_flags |= E1000_RXD_STAT_TCPCS;
            break;

        case ETH_L4_HDR_PROTO_UDP:
            *status_flags |= E1000_RXD_STAT_TCPCS | E1000_RXD_STAT_UDPCS;
            break;

        default:
            break;
        }
    } else {
        trace_e1000e_rx_metadata_l4_cso_disabled();
    }

func_exit:
    trace_e1000e_rx_metadata_status_flags(*status_flags);
    *status_flags = cpu_to_le32(*status_flags);
}

static inline void
igb_write_lgcy_rx_descr(IGBCore *core, struct e1000_rx_desc *desc,
                        struct NetRxPkt *pkt,
                        const E1000E_RSSInfo *rss_info,
                        uint16_t length)
{
    uint32_t status_flags;

    assert(!rss_info->enabled);

    memset(desc, 0, sizeof(*desc));
    desc->length = cpu_to_le16(length);
    igb_build_rx_metadata_common(core, pkt, pkt != NULL,
                                 &status_flags,
                                 &desc->special);

    desc->errors = (uint8_t) (le32_to_cpu(status_flags) >> 24);
    desc->status = (uint8_t) le32_to_cpu(status_flags);
}

static bool
igb_rx_ps_descriptor_split_always(IGBCore *core, const E1000ERingInfo *r)
{
    uint32_t desctyp = igb_rx_queue_desctyp_get(core, r);
    return desctyp == E1000_SRRCTL_DESCTYPE_HDR_SPLIT_ALWAYS;
}

static uint16_t
igb_rx_desc_get_packet_type(IGBCore *core, struct NetRxPkt *pkt, uint16_t etqf)
{
    uint16_t pkt_type;
    bool hasip4, hasip6;
    EthL4HdrProto l4hdr_proto;

    if (etqf < 8) {
        pkt_type = BIT(11) | etqf;
        return pkt_type;
    }

    net_rx_pkt_get_protocols(pkt, &hasip4, &hasip6, &l4hdr_proto);

    if (hasip6 && !(core->mac[RFCTL] & E1000_RFCTL_IPV6_DIS)) {
        eth_ip6_hdr_info *ip6hdr_info = net_rx_pkt_get_ip6_info(pkt);
        pkt_type = ip6hdr_info->has_ext_hdrs ? E1000_ADVRXD_PKT_IP6E :
                                               E1000_ADVRXD_PKT_IP6;
    } else if (hasip4) {
        pkt_type = E1000_ADVRXD_PKT_IP4;
    } else {
        pkt_type = 0;
    }

    switch (l4hdr_proto) {
    case ETH_L4_HDR_PROTO_TCP:
        pkt_type |= E1000_ADVRXD_PKT_TCP;
        break;
    case ETH_L4_HDR_PROTO_UDP:
        pkt_type |= E1000_ADVRXD_PKT_UDP;
        break;
    case ETH_L4_HDR_PROTO_SCTP:
        pkt_type |= E1000_ADVRXD_PKT_SCTP;
        break;
    default:
        break;
    }

    return pkt_type;
}

static inline void
igb_write_adv_rx_descr(IGBCore *core, union e1000_adv_rx_desc *desc,
                       struct NetRxPkt *pkt,
                       const E1000E_RSSInfo *rss_info, uint16_t etqf, bool ts,
                       uint16_t length)
{
    bool hasip4, hasip6;
    EthL4HdrProto l4hdr_proto;
    uint16_t rss_type = 0, pkt_type;
    bool eop = (pkt != NULL);
    uint32_t adv_desc_status_error = 0;
    memset(&desc->wb, 0, sizeof(desc->wb));

    desc->wb.upper.length = cpu_to_le16(length);
    igb_build_rx_metadata_common(core, pkt, eop,
                                 &desc->wb.upper.status_error,
                                 &desc->wb.upper.vlan);

    if (!eop) {
        return;
    }

    net_rx_pkt_get_protocols(pkt, &hasip4, &hasip6, &l4hdr_proto);

    if ((core->mac[RXCSUM] & E1000_RXCSUM_PCSD) != 0) {
        if (rss_info->enabled) {
            desc->wb.lower.hi_dword.rss = cpu_to_le32(rss_info->hash);
            rss_type = rss_info->type;
            trace_igb_rx_metadata_rss(desc->wb.lower.hi_dword.rss, rss_type);
        }
    } else if (hasip4) {
            adv_desc_status_error |= E1000_RXD_STAT_IPIDV;
            desc->wb.lower.hi_dword.csum_ip.ip_id =
                cpu_to_le16(net_rx_pkt_get_ip_id(pkt));
            trace_e1000e_rx_metadata_ip_id(
                desc->wb.lower.hi_dword.csum_ip.ip_id);
    }

    if (ts) {
        adv_desc_status_error |= BIT(16);
    }

    pkt_type = igb_rx_desc_get_packet_type(core, pkt, etqf);
    trace_e1000e_rx_metadata_pkt_type(pkt_type);
    desc->wb.lower.lo_dword.pkt_info = cpu_to_le16(rss_type | (pkt_type << 4));
    desc->wb.upper.status_error |= cpu_to_le32(adv_desc_status_error);
}

static inline void
igb_write_adv_ps_rx_descr(IGBCore *core,
                          union e1000_adv_rx_desc *desc,
                          struct NetRxPkt *pkt,
                          const E1000E_RSSInfo *rss_info,
                          const E1000ERingInfo *r,
                          uint16_t etqf,
                          bool ts,
                          IGBPacketRxDMAState *pdma_st)
{
    size_t pkt_len;
    uint16_t hdr_info = 0;

    if (pdma_st->do_ps) {
        pkt_len = pdma_st->bastate.written[1];
    } else {
        pkt_len = pdma_st->bastate.written[0] + pdma_st->bastate.written[1];
    }

    igb_write_adv_rx_descr(core, desc, pkt, rss_info, etqf, ts, pkt_len);

    hdr_info = (pdma_st->ps_desc_data.hdr_len << E1000_ADVRXD_HDR_LEN_OFFSET) &
               E1000_ADVRXD_ADV_HDR_LEN_MASK;
    hdr_info |= pdma_st->ps_desc_data.sph ? E1000_ADVRXD_HDR_SPH : 0;
    desc->wb.lower.lo_dword.hdr_info = cpu_to_le16(hdr_info);

    desc->wb.upper.status_error |= cpu_to_le32(
        pdma_st->ps_desc_data.hbo ? E1000_ADVRXD_ST_ERR_HBO_OFFSET : 0);
}

static inline void
igb_write_rx_descr(IGBCore *core,
                   union e1000_rx_desc_union *desc,
                   struct NetRxPkt *pkt,
                   const E1000E_RSSInfo *rss_info,
                   uint16_t etqf,
                   bool ts,
                   IGBPacketRxDMAState *pdma_st,
                   const E1000ERingInfo *r)
{
    if (igb_rx_use_legacy_descriptor(core)) {
        igb_write_lgcy_rx_descr(core, &desc->legacy, pkt, rss_info,
                                pdma_st->bastate.written[1]);
    } else if (igb_rx_use_ps_descriptor(core, r)) {
        igb_write_adv_ps_rx_descr(core, &desc->adv, pkt, rss_info, r, etqf, ts,
                                  pdma_st);
    } else {
        igb_write_adv_rx_descr(core, &desc->adv, pkt, rss_info,
                               etqf, ts, pdma_st->bastate.written[1]);
    }
}

static inline void
igb_pci_dma_write_rx_desc(IGBCore *core, PCIDevice *dev, dma_addr_t addr,
                          union e1000_rx_desc_union *desc, dma_addr_t len)
{
    if (igb_rx_use_legacy_descriptor(core)) {
        struct e1000_rx_desc *d = &desc->legacy;
        size_t offset = offsetof(struct e1000_rx_desc, status);
        uint8_t status = d->status;

        d->status &= ~E1000_RXD_STAT_DD;
        pci_dma_write(dev, addr, desc, len);

        if (status & E1000_RXD_STAT_DD) {
            d->status = status;
            pci_dma_write(dev, addr + offset, &status, sizeof(status));
        }
    } else {
        union e1000_adv_rx_desc *d = &desc->adv;
        size_t offset =
            offsetof(union e1000_adv_rx_desc, wb.upper.status_error);
        uint32_t status = d->wb.upper.status_error;

        d->wb.upper.status_error &= ~E1000_RXD_STAT_DD;
        pci_dma_write(dev, addr, desc, len);

        if (status & E1000_RXD_STAT_DD) {
            d->wb.upper.status_error = status;
            pci_dma_write(dev, addr + offset, &status, sizeof(status));
        }
    }
}

static void
igb_update_rx_stats(IGBCore *core, const E1000ERingInfo *rxi,
                    size_t pkt_size, size_t pkt_fcs_size)
{
    eth_pkt_types_e pkt_type = net_rx_pkt_get_packet_type(core->rx_pkt);
    e1000x_update_rx_total_stats(core->mac, pkt_type, pkt_size, pkt_fcs_size);

    if (core->mac[MRQC] & 1) {
        uint16_t pool = rxi->idx % IGB_NUM_VM_POOLS;

        core->mac[PVFGORC0 + (pool * 64)] += pkt_size + 4;
        core->mac[PVFGPRC0 + (pool * 64)]++;
        if (pkt_type == ETH_PKT_MCAST) {
            core->mac[PVFMPRC0 + (pool * 64)]++;
        }
    }
}

static inline bool
igb_rx_descr_threshold_hit(IGBCore *core, const E1000ERingInfo *rxi)
{
    return igb_ring_free_descr_num(core, rxi) ==
           ((core->mac[E1000_SRRCTL(rxi->idx) >> 2] >> 20) & 31) * 16;
}

static bool
igb_do_ps(IGBCore *core,
          const E1000ERingInfo *r,
          struct NetRxPkt *pkt,
          IGBPacketRxDMAState *pdma_st)
{
    bool hasip4, hasip6;
    EthL4HdrProto l4hdr_proto;
    bool fragment;
    bool split_always;
    size_t bheader_size;
    size_t total_pkt_len;

    if (!igb_rx_use_ps_descriptor(core, r)) {
        return false;
    }

    total_pkt_len = net_rx_pkt_get_total_len(pkt);
    bheader_size = igb_rxhdrbufsize(core, r);
    split_always = igb_rx_ps_descriptor_split_always(core, r);
    if (split_always && total_pkt_len <= bheader_size) {
        pdma_st->ps_hdr_len = total_pkt_len;
        pdma_st->ps_desc_data.hdr_len = total_pkt_len;
        return true;
    }

    net_rx_pkt_get_protocols(pkt, &hasip4, &hasip6, &l4hdr_proto);

    if (hasip4) {
        fragment = net_rx_pkt_get_ip4_info(pkt)->fragment;
    } else if (hasip6) {
        fragment = net_rx_pkt_get_ip6_info(pkt)->fragment;
    } else {
        pdma_st->ps_desc_data.hdr_len = bheader_size;
        goto header_not_handled;
    }

    if (fragment && (core->mac[RFCTL] & E1000_RFCTL_IPFRSP_DIS)) {
        pdma_st->ps_desc_data.hdr_len = bheader_size;
        goto header_not_handled;
    }

    /* no header splitting for SCTP */
    if (!fragment && (l4hdr_proto == ETH_L4_HDR_PROTO_UDP ||
                      l4hdr_proto == ETH_L4_HDR_PROTO_TCP)) {
        pdma_st->ps_hdr_len = net_rx_pkt_get_l5_hdr_offset(pkt);
    } else {
        pdma_st->ps_hdr_len = net_rx_pkt_get_l4_hdr_offset(pkt);
    }

    pdma_st->ps_desc_data.sph = true;
    pdma_st->ps_desc_data.hdr_len = pdma_st->ps_hdr_len;

    if (pdma_st->ps_hdr_len > bheader_size) {
        pdma_st->ps_desc_data.hbo = true;
        goto header_not_handled;
    }

    return true;

header_not_handled:
    if (split_always) {
        pdma_st->ps_hdr_len = bheader_size;
        return true;
    }

    return false;
}

static void
igb_truncate_to_descriptor_size(IGBPacketRxDMAState *pdma_st, size_t *size)
{
    if (pdma_st->do_ps && pdma_st->is_first) {
        if (*size > pdma_st->rx_desc_packet_buf_size + pdma_st->ps_hdr_len) {
            *size = pdma_st->rx_desc_packet_buf_size + pdma_st->ps_hdr_len;
        }
    } else {
        if (*size > pdma_st->rx_desc_packet_buf_size) {
            *size = pdma_st->rx_desc_packet_buf_size;
        }
    }
}

static inline void
igb_write_hdr_frag_to_rx_buffers(IGBCore *core,
                                 PCIDevice *d,
                                 IGBPacketRxDMAState *pdma_st,
                                 const char *data,
                                 dma_addr_t data_len)
{
    assert(data_len <= pdma_st->rx_desc_header_buf_size -
                       pdma_st->bastate.written[0]);
    pci_dma_write(d,
                  pdma_st->ba[0] + pdma_st->bastate.written[0],
                  data, data_len);
    pdma_st->bastate.written[0] += data_len;
    pdma_st->bastate.cur_idx = 1;
}

static void
igb_write_header_to_rx_buffers(IGBCore *core,
                               struct NetRxPkt *pkt,
                               PCIDevice *d,
                               IGBPacketRxDMAState *pdma_st,
                               size_t *copy_size)
{
    size_t iov_copy;
    size_t ps_hdr_copied = 0;

    if (!pdma_st->is_first) {
        /* Leave buffer 0 of each descriptor except first */
        /* empty                                          */
        pdma_st->bastate.cur_idx = 1;
        return;
    }

    do {
        iov_copy = MIN(pdma_st->ps_hdr_len - ps_hdr_copied,
                       pdma_st->iov->iov_len - pdma_st->iov_ofs);

        igb_write_hdr_frag_to_rx_buffers(core, d, pdma_st,
                                         static_cast<const char *>(pdma_st->iov->iov_base),
                                         iov_copy);

        *copy_size -= iov_copy;
        ps_hdr_copied += iov_copy;

        pdma_st->iov_ofs += iov_copy;
        if (pdma_st->iov_ofs == pdma_st->iov->iov_len) {
            pdma_st->iov++;
            pdma_st->iov_ofs = 0;
        }
    } while (ps_hdr_copied < pdma_st->ps_hdr_len);

    pdma_st->is_first = false;
}

static void
igb_write_payload_frag_to_rx_buffers(IGBCore *core,
                                     PCIDevice *d,
                                     IGBPacketRxDMAState *pdma_st,
                                     const char *data,
                                     dma_addr_t data_len)
{
    while (data_len > 0) {
        assert(pdma_st->bastate.cur_idx < IGB_MAX_PS_BUFFERS);

        uint32_t cur_buf_bytes_left =
            pdma_st->rx_desc_packet_buf_size -
            pdma_st->bastate.written[pdma_st->bastate.cur_idx];
        uint32_t bytes_to_write = MIN(data_len, cur_buf_bytes_left);

        trace_igb_rx_desc_buff_write(
            pdma_st->bastate.cur_idx,
            pdma_st->ba[pdma_st->bastate.cur_idx],
            pdma_st->bastate.written[pdma_st->bastate.cur_idx],
            data,
            bytes_to_write);

        pci_dma_write(d,
                      pdma_st->ba[pdma_st->bastate.cur_idx] +
                      pdma_st->bastate.written[pdma_st->bastate.cur_idx],
                      data, bytes_to_write);

        pdma_st->bastate.written[pdma_st->bastate.cur_idx] += bytes_to_write;
        data += bytes_to_write;
        data_len -= bytes_to_write;

        if (pdma_st->bastate.written[pdma_st->bastate.cur_idx] ==
            pdma_st->rx_desc_packet_buf_size) {
            pdma_st->bastate.cur_idx++;
        }
    }
}

static void
igb_write_payload_to_rx_buffers(IGBCore *core,
                                struct NetRxPkt *pkt,
                                PCIDevice *d,
                                IGBPacketRxDMAState *pdma_st,
                                size_t *copy_size)
{
    static const uint32_t fcs_pad = 0;
    size_t iov_copy;

    /* Copy packet payload */
    while (*copy_size) {
        iov_copy = MIN(*copy_size, pdma_st->iov->iov_len - pdma_st->iov_ofs);
        igb_write_payload_frag_to_rx_buffers(core, d,
                                             pdma_st,
                                             static_cast<const char *>(pdma_st->iov->iov_base) +
                                             pdma_st->iov_ofs,
                                             iov_copy);

        *copy_size -= iov_copy;
        pdma_st->iov_ofs += iov_copy;
        if (pdma_st->iov_ofs == pdma_st->iov->iov_len) {
            pdma_st->iov++;
            pdma_st->iov_ofs = 0;
        }
    }

    if (pdma_st->desc_offset + pdma_st->desc_size >= pdma_st->total_size) {
        /* Simulate FCS checksum presence in the last descriptor */
        igb_write_payload_frag_to_rx_buffers(core, d,
                                             pdma_st,
                                             (const char *) &fcs_pad,
                                             e1000x_fcs_len(core->mac));
    }
}

static void
igb_write_to_rx_buffers(IGBCore *core,
                        struct NetRxPkt *pkt,
                        PCIDevice *d,
                        IGBPacketRxDMAState *pdma_st)
{
    size_t copy_size;

    if (!(pdma_st->ba)[1] || (pdma_st->do_ps && !(pdma_st->ba[0]))) {
        /* as per intel docs; skip descriptors with null buf addr */
        trace_e1000e_rx_null_descriptor();
        return;
    }

    if (pdma_st->desc_offset >= pdma_st->size) {
        return;
    }

    pdma_st->desc_size = pdma_st->total_size - pdma_st->desc_offset;
    igb_truncate_to_descriptor_size(pdma_st, &pdma_st->desc_size);
    copy_size = pdma_st->size - pdma_st->desc_offset;
    igb_truncate_to_descriptor_size(pdma_st, &copy_size);

    /* For PS mode copy the packet header first */
    if (pdma_st->do_ps) {
        igb_write_header_to_rx_buffers(core, pkt, d, pdma_st, &copy_size);
    } else {
        pdma_st->bastate.cur_idx = 1;
    }

    igb_write_payload_to_rx_buffers(core, pkt, d, pdma_st, &copy_size);
}

static void
igb_write_packet_to_guest(IGBCore *core, struct NetRxPkt *pkt,
                          const E1000E_RxRing *rxr,
                          const E1000E_RSSInfo *rss_info,
                          uint16_t etqf, bool ts)
{
    PCIDevice *d;
    dma_addr_t base;
    union e1000_rx_desc_union desc;
    const E1000ERingInfo *rxi;
    size_t rx_desc_len;

    IGBPacketRxDMAState pdma_st = {0};
    pdma_st.is_first = true;
    pdma_st.size = net_rx_pkt_get_total_len(pkt);
    pdma_st.total_size = pdma_st.size + e1000x_fcs_len(core->mac);

    rxi = rxr->i;
    rx_desc_len = core->rx_desc_len;
    pdma_st.rx_desc_packet_buf_size = igb_rxbufsize(core, rxi);
    pdma_st.rx_desc_header_buf_size = igb_rxhdrbufsize(core, rxi);
    pdma_st.iov = net_rx_pkt_get_iovec(pkt);
    d = pcie_sriov_get_vf_at_index(core->owner, rxi->idx % 8);
    if (!d) {
        d = core->owner;
    }

    pdma_st.do_ps = igb_do_ps(core, rxi, pkt, &pdma_st);

    do {
        memset(&pdma_st.bastate, 0, sizeof(IGBBAState));
        bool is_last = false;

        if (igb_ring_empty(core, rxi)) {
            return;
        }

        base = igb_ring_head_descr(core, rxi);
        pci_dma_read(d, base, &desc, rx_desc_len);
        trace_e1000e_rx_descr(rxi->idx, base, rx_desc_len);

        igb_read_rx_descr(core, &desc, &pdma_st, rxi);

        igb_write_to_rx_buffers(core, pkt, d, &pdma_st);
        pdma_st.desc_offset += pdma_st.desc_size;
        if (pdma_st.desc_offset >= pdma_st.total_size) {
            is_last = true;
        }

        igb_write_rx_descr(core, &desc,
                           is_last ? pkt : NULL,
                           rss_info,
                           etqf, ts,
                           &pdma_st,
                           rxi);
        igb_pci_dma_write_rx_desc(core, d, base, &desc, rx_desc_len);
        igb_ring_advance(core, rxi, rx_desc_len / E1000_MIN_RX_DESC_LEN);
    } while (pdma_st.desc_offset < pdma_st.total_size);

    igb_update_rx_stats(core, rxi, pdma_st.size, pdma_st.total_size);
}

static bool
igb_rx_strip_vlan(IGBCore *core, const E1000ERingInfo *rxi)
{
    if (core->mac[MRQC] & 1) {
        uint16_t pool = rxi->idx % IGB_NUM_VM_POOLS;
        /* Sec 7.10.3.8: CTRL.VME is ignored, only VMOLR/RPLOLR is used */
        return (net_rx_pkt_get_packet_type(core->rx_pkt) == ETH_PKT_MCAST) ?
                core->mac[RPLOLR] & E1000_RPLOLR_STRVLAN :
                core->mac[VMOLR0 + pool] & E1000_VMOLR_STRVLAN;
    }

    return e1000x_vlan_enabled(core->mac);
}

static inline void
igb_rx_fix_l4_csum(IGBCore *core, struct NetRxPkt *pkt)
{
    struct virtio_net_hdr *vhdr = net_rx_pkt_get_vhdr(pkt);

    if (vhdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
        net_rx_pkt_fix_l4_csum(pkt);
    }
}

ssize_t
igb_receive_iov(IGBCore *core, const struct iovec *iov, int iovcnt)
{
    return igb_receive_internal(core, iov, iovcnt, core->has_vnet, NULL);
}

static ssize_t
igb_receive_internal(IGBCore *core, const struct iovec *iov, int iovcnt,
                     bool has_vnet, bool *external_tx)
{
    uint16_t queues = 0;
    uint32_t causes = 0;
    uint32_t ecauses = 0;
    union {
        L2Header l2_header;
        uint8_t octets[ETH_ZLEN];
    } buf;
    struct iovec min_iov;
    size_t size, orig_size;
    size_t iov_ofs = 0;
    E1000E_RxRing rxr;
    E1000E_RSSInfo rss_info;
    uint16_t etqf;
    bool ts;
    size_t total_size;
    int strip_vlan_index;
    int i;

    trace_e1000e_rx_receive_iov(iovcnt);

    if (external_tx) {
        *external_tx = true;
    }

    if (!e1000x_hw_rx_enabled(core->mac)) {
        return -1;
    }

    /* Pull virtio header in */
    if (has_vnet) {
        net_rx_pkt_set_vhdr_iovec(core->rx_pkt, iov, iovcnt);
        iov_ofs = sizeof(struct virtio_net_hdr);
    } else {
        net_rx_pkt_unset_vhdr(core->rx_pkt);
    }

    orig_size = iov_size(iov, iovcnt);
    size = orig_size - iov_ofs;

    /* Pad to minimum Ethernet frame length */
    if (size < sizeof(buf)) {
        iov_to_buf(iov, iovcnt, iov_ofs, &buf, size);
        memset(&buf.octets[size], 0, sizeof(buf) - size);
        e1000x_inc_reg_if_not_full(core->mac, RUC);
        min_iov.iov_base = &buf;
        min_iov.iov_len = size = sizeof(buf);
        iovcnt = 1;
        iov = &min_iov;
        iov_ofs = 0;
    } else {
        iov_to_buf(iov, iovcnt, iov_ofs, &buf, sizeof(buf.l2_header));
    }

    net_rx_pkt_set_packet_type(core->rx_pkt,
                               get_eth_packet_type(&buf.l2_header.eth));
    net_rx_pkt_set_protocols(core->rx_pkt, iov, iovcnt, iov_ofs);

    queues = igb_receive_assign(core, iov, iovcnt, iov_ofs,
                                &buf.l2_header, size,
                                &rss_info, &etqf, &ts, external_tx);
    if (!queues) {
        trace_e1000e_rx_flt_dropped();
        return orig_size;
    }

    for (i = 0; i < IGB_NUM_QUEUES; i++) {
        if (!(queues & BIT(i)) ||
            !(core->mac[RXDCTL0 + (i * 16)] & E1000_RXDCTL_QUEUE_ENABLE)) {
            continue;
        }

        igb_rx_ring_init(core, &rxr, i);

        if (!igb_rx_strip_vlan(core, rxr.i)) {
            strip_vlan_index = -1;
        } else if (core->mac[CTRL_EXT] & BIT(26)) {
            strip_vlan_index = 1;
        } else {
            strip_vlan_index = 0;
        }

        net_rx_pkt_attach_iovec_ex(core->rx_pkt, iov, iovcnt, iov_ofs,
                                   strip_vlan_index,
                                   core->mac[VET] & 0xffff,
                                   core->mac[VET] >> 16);

        total_size = net_rx_pkt_get_total_len(core->rx_pkt) +
            e1000x_fcs_len(core->mac);

        if (!igb_has_rxbufs(core, rxr.i, total_size)) {
            causes |= E1000_ICS_RXO;
            trace_e1000e_rx_not_written_to_guest(rxr.i->idx);
            continue;
        }

        causes |= E1000_ICR_RXDW;

        igb_rx_fix_l4_csum(core, core->rx_pkt);
        igb_write_packet_to_guest(core, core->rx_pkt, &rxr, &rss_info, etqf, ts);

        /* Check if receive descriptor minimum threshold hit */
        if (igb_rx_descr_threshold_hit(core, rxr.i)) {
            causes |= E1000_ICS_RXDMT0;
        }

        ecauses |= igb_rx_wb_eic(core, rxr.i->idx);

        trace_e1000e_rx_written_to_guest(rxr.i->idx);
    }

    trace_e1000e_rx_interrupt_set(causes);
    igb_raise_interrupts(core, EICR, ecauses);
    igb_raise_interrupts(core, ICR, causes);

    return orig_size;
}

static inline bool
igb_have_autoneg(IGBCore *core)
{
    return core->phy[MII_BMCR] & MII_BMCR_AUTOEN;
}

static void igb_update_flowctl_status(IGBCore *core)
{
    if (igb_have_autoneg(core) && core->phy[MII_BMSR] & MII_BMSR_AN_COMP) {
        trace_e1000e_link_autoneg_flowctl(true);
        core->mac[CTRL] |= E1000_CTRL_TFCE | E1000_CTRL_RFCE;
    } else {
        trace_e1000e_link_autoneg_flowctl(false);
    }
}

static inline void
igb_link_down(IGBCore *core)
{
    e1000x_update_regs_on_link_down(core->mac, core->phy);
    igb_update_flowctl_status(core);
}

static inline void
igb_set_phy_ctrl(IGBCore *core, uint16_t val)
{
    /* bits 0-5 reserved; MII_BMCR_[ANRESTART,RESET] are self clearing */
    core->phy[MII_BMCR] = val & ~(0x3f | MII_BMCR_RESET | MII_BMCR_ANRESTART);

    if ((val & MII_BMCR_ANRESTART) && igb_have_autoneg(core)) {
        e1000x_restart_autoneg(core->mac, core->phy, core->autoneg_timer);
    }
}

void igb_core_set_link_status(IGBCore *core)
{
    NetClientState *nc = qemu_get_queue(core->owner_nic);
    uint32_t old_status = core->mac[STATUS];

    trace_e1000e_link_status_changed(nc->link_down ? false : true);

    if (nc->link_down) {
        e1000x_update_regs_on_link_down(core->mac, core->phy);
    } else {
        if (igb_have_autoneg(core) &&
            !(core->phy[MII_BMSR] & MII_BMSR_AN_COMP)) {
            e1000x_restart_autoneg(core->mac, core->phy,
                                   core->autoneg_timer);
        } else {
            e1000x_update_regs_on_link_up(core->mac, core->phy);
            igb_start_recv(core);
        }
    }

    if (core->mac[STATUS] != old_status) {
        igb_raise_interrupts(core, ICR, E1000_ICR_LSC);
    }
}

static void
igb_set_ctrl(IGBCore *core, int index, uint32_t val)
{
    trace_e1000e_core_ctrl_write(index, val);

    /* RST is self clearing */
    core->mac[CTRL] = val & ~E1000_CTRL_RST;
    core->mac[CTRL_DUP] = core->mac[CTRL];

    trace_e1000e_link_set_params(
        !!(val & E1000_CTRL_ASDE),
        (val & E1000_CTRL_SPD_SEL) >> E1000_CTRL_SPD_SHIFT,
        !!(val & E1000_CTRL_FRCSPD),
        !!(val & E1000_CTRL_FRCDPX),
        !!(val & E1000_CTRL_RFCE),
        !!(val & E1000_CTRL_TFCE));

    if (val & E1000_CTRL_RST) {
        trace_e1000e_core_ctrl_sw_reset();
        igb_reset(core, true);
    }

    if (val & E1000_CTRL_PHY_RST) {
        trace_e1000e_core_ctrl_phy_reset();
        core->mac[STATUS] |= E1000_STATUS_PHYRA;
    }
}

static void
igb_set_rfctl(IGBCore *core, int index, uint32_t val)
{
    trace_e1000e_rx_set_rfctl(val);

    if (!(val & E1000_RFCTL_ISCSI_DIS)) {
        trace_e1000e_wrn_iscsi_filtering_not_supported();
    }

    if (!(val & E1000_RFCTL_NFSW_DIS)) {
        trace_e1000e_wrn_nfsw_filtering_not_supported();
    }

    if (!(val & E1000_RFCTL_NFSR_DIS)) {
        trace_e1000e_wrn_nfsr_filtering_not_supported();
    }

    core->mac[RFCTL] = val;
}

static void
igb_calc_rxdesclen(IGBCore *core)
{
    if (igb_rx_use_legacy_descriptor(core)) {
        core->rx_desc_len = sizeof(struct e1000_rx_desc);
    } else {
        core->rx_desc_len = sizeof(union e1000_adv_rx_desc);
    }
    trace_e1000e_rx_desc_len(core->rx_desc_len);
}

static void
igb_set_rx_control(IGBCore *core, int index, uint32_t val)
{
    core->mac[RCTL] = val;
    trace_e1000e_rx_set_rctl(core->mac[RCTL]);

    if (val & E1000_RCTL_DTYP_MASK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "igb: RCTL.DTYP must be zero for compatibility");
    }

    if (val & E1000_RCTL_EN) {
        igb_calc_rxdesclen(core);
        igb_start_recv(core);
    }
}

static inline bool
igb_postpone_interrupt(IGBIntrDelayTimer *timer)
{
    if (timer->running) {
        trace_e1000e_irq_postponed_by_xitr(timer->delay_reg << 2);

        return true;
    }

    if (timer->core->mac[timer->delay_reg] != 0) {
        igb_intrmgr_rearm_timer(timer);
    }

    return false;
}

static inline bool
igb_eitr_should_postpone(IGBCore *core, int idx)
{
    return igb_postpone_interrupt(&core->eitr[idx]);
}

static void igb_send_msix(IGBCore *core, uint32_t causes)
{
    int vector;

    for (vector = 0; vector < IGB_INTR_NUM; ++vector) {
        if ((causes & BIT(vector)) && !igb_eitr_should_postpone(core, vector)) {

            trace_e1000e_irq_msix_notify_vec(vector);
            igb_msix_notify(core, vector);
        }
    }
}

static inline void
igb_fix_icr_asserted(IGBCore *core)
{
    core->mac[ICR] &= ~E1000_ICR_ASSERTED;
    if (core->mac[ICR]) {
        core->mac[ICR] |= E1000_ICR_ASSERTED;
    }

    trace_e1000e_irq_fix_icr_asserted(core->mac[ICR]);
}

static void igb_raise_interrupts(IGBCore *core, size_t index, uint32_t causes)
{
    uint32_t old_causes = core->mac[ICR] & core->mac[IMS];
    uint32_t old_ecauses = core->mac[EICR] & core->mac[EIMS];
    uint32_t raised_causes;
    uint32_t raised_ecauses;
    uint32_t int_alloc;

    trace_e1000e_irq_set(index << 2,
                         core->mac[index], core->mac[index] | causes);

    core->mac[index] |= causes;

    if (core->mac[GPIE] & E1000_GPIE_MSIX_MODE) {
        raised_causes = core->mac[ICR] & core->mac[IMS] & ~old_causes;

        if (raised_causes & E1000_ICR_DRSTA) {
            int_alloc = core->mac[IVAR_MISC] & 0xff;
            if (int_alloc & E1000_IVAR_VALID) {
                core->mac[EICR] |= BIT(int_alloc & 0x1f);
            }
        }
        /* Check if other bits (excluding the TCP Timer) are enabled. */
        if (raised_causes & ~E1000_ICR_DRSTA) {
            int_alloc = (core->mac[IVAR_MISC] >> 8) & 0xff;
            if (int_alloc & E1000_IVAR_VALID) {
                core->mac[EICR] |= BIT(int_alloc & 0x1f);
            }
        }

        raised_ecauses = core->mac[EICR] & core->mac[EIMS] & ~old_ecauses;
        if (!raised_ecauses) {
            return;
        }

        igb_send_msix(core, raised_ecauses);
    } else {
        igb_fix_icr_asserted(core);

        raised_causes = core->mac[ICR] & core->mac[IMS] & ~old_causes;
        if (!raised_causes) {
            return;
        }

        core->mac[EICR] |= (raised_causes & E1000_ICR_DRSTA) | E1000_EICR_OTHER;

        if (msix_enabled(core->owner)) {
            trace_e1000e_irq_msix_notify_vec(0);
            msix_notify(core->owner, 0);
        } else if (msi_enabled(core->owner)) {
            trace_e1000e_irq_msi_notify(raised_causes);
            msi_notify(core->owner, 0);
        } else {
            igb_raise_legacy_irq(core);
        }
    }
}

static void igb_lower_interrupts(IGBCore *core, size_t index, uint32_t causes)
{
    trace_e1000e_irq_clear(index << 2,
                           core->mac[index], core->mac[index] & ~causes);

    core->mac[index] &= ~causes;

    trace_e1000e_irq_pending_interrupts(core->mac[ICR] & core->mac[IMS],
                                        core->mac[ICR], core->mac[IMS]);

    if (!(core->mac[ICR] & core->mac[IMS]) &&
        !(core->mac[GPIE] & E1000_GPIE_MSIX_MODE)) {
        core->mac[EICR] &= ~E1000_EICR_OTHER;

        if (!msix_enabled(core->owner) && !msi_enabled(core->owner)) {
            igb_lower_legacy_irq(core);
        }
    }
}

static void igb_set_eics(IGBCore *core, int index, uint32_t val)
{
    bool msix = !!(core->mac[GPIE] & E1000_GPIE_MSIX_MODE);
    uint32_t mask = msix ? E1000_EICR_MSIX_MASK : E1000_EICR_LEGACY_MASK;

    trace_igb_irq_write_eics(val, msix);
    igb_raise_interrupts(core, EICR, val & mask);
}

static void igb_set_eims(IGBCore *core, int index, uint32_t val)
{
    bool msix = !!(core->mac[GPIE] & E1000_GPIE_MSIX_MODE);
    uint32_t mask = msix ? E1000_EICR_MSIX_MASK : E1000_EICR_LEGACY_MASK;

    trace_igb_irq_write_eims(val, msix);
    igb_raise_interrupts(core, EIMS, val & mask);
}

static void mailbox_interrupt_to_vf(IGBCore *core, uint16_t vfn)
{
    uint32_t ent = core->mac[VTIVAR_MISC + vfn];
    uint32_t causes;

    if ((ent & E1000_IVAR_VALID)) {
        causes = (ent & 0x3) << (22 - vfn * IGBVF_MSIX_VEC_NUM);
        igb_raise_interrupts(core, EICR, causes);
    }
}

static void mailbox_interrupt_to_pf(IGBCore *core)
{
    igb_raise_interrupts(core, ICR, E1000_ICR_VMMB);
}

static void igb_set_pfmailbox(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = index - P2VMAILBOX0;

    trace_igb_set_pfmailbox(vfn, val);

    if (val & E1000_P2VMAILBOX_STS) {
        core->mac[V2PMAILBOX0 + vfn] |= E1000_V2PMAILBOX_PFSTS;
        mailbox_interrupt_to_vf(core, vfn);
    }

    if (val & E1000_P2VMAILBOX_ACK) {
        core->mac[V2PMAILBOX0 + vfn] |= E1000_V2PMAILBOX_PFACK;
        mailbox_interrupt_to_vf(core, vfn);
    }

    /* Buffer Taken by PF (can be set only if the VFU is cleared). */
    if (val & E1000_P2VMAILBOX_PFU) {
        if (!(core->mac[index] & E1000_P2VMAILBOX_VFU)) {
            core->mac[index] |= E1000_P2VMAILBOX_PFU;
            core->mac[V2PMAILBOX0 + vfn] |= E1000_V2PMAILBOX_PFU;
        }
    } else {
        core->mac[index] &= ~E1000_P2VMAILBOX_PFU;
        core->mac[V2PMAILBOX0 + vfn] &= ~E1000_V2PMAILBOX_PFU;
    }

    if (val & E1000_P2VMAILBOX_RVFU) {
        core->mac[V2PMAILBOX0 + vfn] &= ~E1000_V2PMAILBOX_VFU;
        core->mac[MBVFICR] &= ~((E1000_MBVFICR_VFACK_VF1 << vfn) |
                                (E1000_MBVFICR_VFREQ_VF1 << vfn));
    }
}

static void igb_set_vfmailbox(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = index - V2PMAILBOX0;

    trace_igb_set_vfmailbox(vfn, val);

    if (val & E1000_V2PMAILBOX_REQ) {
        core->mac[MBVFICR] |= E1000_MBVFICR_VFREQ_VF1 << vfn;
        mailbox_interrupt_to_pf(core);
    }

    if (val & E1000_V2PMAILBOX_ACK) {
        core->mac[MBVFICR] |= E1000_MBVFICR_VFACK_VF1 << vfn;
        mailbox_interrupt_to_pf(core);
    }

    /* Buffer Taken by VF (can be set only if the PFU is cleared). */
    if (val & E1000_V2PMAILBOX_VFU) {
        if (!(core->mac[index] & E1000_V2PMAILBOX_PFU)) {
            core->mac[index] |= E1000_V2PMAILBOX_VFU;
            core->mac[P2VMAILBOX0 + vfn] |= E1000_P2VMAILBOX_VFU;
        }
    } else {
        core->mac[index] &= ~E1000_V2PMAILBOX_VFU;
        core->mac[P2VMAILBOX0 + vfn] &= ~E1000_P2VMAILBOX_VFU;
    }
}

void igb_core_vf_reset(IGBCore *core, uint16_t vfn)
{
    uint16_t qn0 = vfn;
    uint16_t qn1 = vfn + IGB_NUM_VM_POOLS;

    trace_igb_core_vf_reset(vfn);

    /* disable Rx and Tx for the VF*/
    core->mac[RXDCTL0 + (qn0 * 16)] &= ~E1000_RXDCTL_QUEUE_ENABLE;
    core->mac[RXDCTL0 + (qn1 * 16)] &= ~E1000_RXDCTL_QUEUE_ENABLE;
    core->mac[TXDCTL0 + (qn0 * 16)] &= ~E1000_TXDCTL_QUEUE_ENABLE;
    core->mac[TXDCTL0 + (qn1 * 16)] &= ~E1000_TXDCTL_QUEUE_ENABLE;
    core->mac[VFRE] &= ~BIT(vfn);
    core->mac[VFTE] &= ~BIT(vfn);
    /* indicate VF reset to PF */
    core->mac[VFLRE] |= BIT(vfn);
    /* VFLRE and mailbox use the same interrupt cause */
    mailbox_interrupt_to_pf(core);
}

static void igb_w1c(IGBCore *core, int index, uint32_t val)
{
    core->mac[index] &= ~val;
}

static void igb_set_eimc(IGBCore *core, int index, uint32_t val)
{
    bool msix = !!(core->mac[GPIE] & E1000_GPIE_MSIX_MODE);
    uint32_t mask = msix ? E1000_EICR_MSIX_MASK : E1000_EICR_LEGACY_MASK;

    trace_igb_irq_write_eimc(val, msix);

    /* Interrupts are disabled via a write to EIMC and reflected in EIMS. */
    igb_lower_interrupts(core, EIMS, val & mask);
}

static void igb_set_eiac(IGBCore *core, int index, uint32_t val)
{
    bool msix = !!(core->mac[GPIE] & E1000_GPIE_MSIX_MODE);

    if (msix) {
        trace_igb_irq_write_eiac(val);

        /*
         * TODO: When using IOV, the bits that correspond to MSI-X vectors
         * that are assigned to a VF are read-only.
         */
        core->mac[EIAC] |= (val & E1000_EICR_MSIX_MASK);
    }
}

static void igb_set_eiam(IGBCore *core, int index, uint32_t val)
{
    bool msix = !!(core->mac[GPIE] & E1000_GPIE_MSIX_MODE);

    /*
     * TODO: When using IOV, the bits that correspond to MSI-X vectors that
     * are assigned to a VF are read-only.
     */
    core->mac[EIAM] |=
        ~(val & (msix ? E1000_EICR_MSIX_MASK : E1000_EICR_LEGACY_MASK));

    trace_igb_irq_write_eiam(val, msix);
}

static void igb_set_eicr(IGBCore *core, int index, uint32_t val)
{
    bool msix = !!(core->mac[GPIE] & E1000_GPIE_MSIX_MODE);

    /*
     * TODO: In IOV mode, only bit zero of this vector is available for the PF
     * function.
     */
    uint32_t mask = msix ? E1000_EICR_MSIX_MASK : E1000_EICR_LEGACY_MASK;

    trace_igb_irq_write_eicr(val, msix);
    igb_lower_interrupts(core, EICR, val & mask);
}

static void igb_set_vtctrl(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn;

    if (val & E1000_CTRL_RST) {
        vfn = (index - PVTCTRL0) / 0x40;
        igb_core_vf_reset(core, vfn);
    }
}

static void igb_set_vteics(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - PVTEICS0) / 0x40;

    core->mac[index] = val;
    igb_set_eics(core, EICS, (val & 0x7) << (22 - vfn * IGBVF_MSIX_VEC_NUM));
}

static void igb_set_vteims(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - PVTEIMS0) / 0x40;

    core->mac[index] = val;
    igb_set_eims(core, EIMS, (val & 0x7) << (22 - vfn * IGBVF_MSIX_VEC_NUM));
}

static void igb_set_vteimc(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - PVTEIMC0) / 0x40;

    core->mac[index] = val;
    igb_set_eimc(core, EIMC, (val & 0x7) << (22 - vfn * IGBVF_MSIX_VEC_NUM));
}

static void igb_set_vteiac(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - PVTEIAC0) / 0x40;

    core->mac[index] = val;
    igb_set_eiac(core, EIAC, (val & 0x7) << (22 - vfn * IGBVF_MSIX_VEC_NUM));
}

static void igb_set_vteiam(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - PVTEIAM0) / 0x40;

    core->mac[index] = val;
    igb_set_eiam(core, EIAM, (val & 0x7) << (22 - vfn * IGBVF_MSIX_VEC_NUM));
}

static void igb_set_vteicr(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - PVTEICR0) / 0x40;

    core->mac[index] = val;
    igb_set_eicr(core, EICR, (val & 0x7) << (22 - vfn * IGBVF_MSIX_VEC_NUM));
}

static void igb_set_vtivar(IGBCore *core, int index, uint32_t val)
{
    uint16_t vfn = (index - VTIVAR);
    uint16_t qn = vfn;
    uint8_t ent;
    int n;

    core->mac[index] = val;

    /* Get assigned vector associated with queue Rx#0. */
    if ((val & E1000_IVAR_VALID)) {
        n = igb_ivar_entry_rx(qn);
        ent = E1000_IVAR_VALID | (24 - vfn * IGBVF_MSIX_VEC_NUM - (2 - (val & 0x7)));
        core->mac[IVAR0 + n / 4] |= ent << 8 * (n % 4);
    }

    /* Get assigned vector associated with queue Tx#0 */
    ent = val >> 8;
    if ((ent & E1000_IVAR_VALID)) {
        n = igb_ivar_entry_tx(qn);
        ent = E1000_IVAR_VALID | (24 - vfn * IGBVF_MSIX_VEC_NUM - (2 - (ent & 0x7)));
        core->mac[IVAR0 + n / 4] |= ent << 8 * (n % 4);
    }

    /*
     * Ignoring assigned vectors associated with queues Rx#1 and Tx#1 for now.
     */
}

static inline void
igb_autoneg_timer(void *opaque)
{
    IGBCore *core = static_cast<IGBCore *>(opaque);
    if (!qemu_get_queue(core->owner_nic)->link_down) {
        e1000x_update_regs_on_autoneg_done(core->mac, core->phy);
        igb_start_recv(core);

        igb_update_flowctl_status(core);
        /* signal link status change to the guest */
        igb_raise_interrupts(core, ICR, E1000_ICR_LSC);
    }
}

static inline uint16_t
igb_get_reg_index_with_offset(const uint16_t *mac_reg_access, hwaddr addr)
{
    uint16_t index = (addr & 0x1ffff) >> 2;
    return index + (mac_reg_access[index] & 0xfffe);
}

static char igb_phy_regcap[MAX_PHY_REG_ADDRESS + 1];

static void __attribute__((constructor)) igb_init_phy_regcap(void)
{
    igb_phy_regcap[MII_BMCR] = PHY_RW;
    igb_phy_regcap[MII_BMSR] = PHY_R;
    igb_phy_regcap[MII_PHYID1] = PHY_R;
    igb_phy_regcap[MII_PHYID2] = PHY_R;
    igb_phy_regcap[MII_ANAR] = PHY_RW;
    igb_phy_regcap[MII_ANLPAR] = PHY_R;
    igb_phy_regcap[MII_ANER] = PHY_R;
    igb_phy_regcap[MII_ANNP] = PHY_RW;
    igb_phy_regcap[MII_ANLPRNP] = PHY_R;
    igb_phy_regcap[MII_CTRL1000] = PHY_RW;
    igb_phy_regcap[MII_STAT1000] = PHY_R;
    igb_phy_regcap[MII_EXTSTAT] = PHY_R;
    igb_phy_regcap[IGP01E1000_PHY_PORT_CONFIG] = PHY_RW;
    igb_phy_regcap[IGP01E1000_PHY_PORT_STATUS] = PHY_R;
    igb_phy_regcap[IGP01E1000_PHY_PORT_CTRL] = PHY_RW;
    igb_phy_regcap[IGP01E1000_PHY_LINK_HEALTH] = PHY_R;
    igb_phy_regcap[IGP02E1000_PHY_POWER_MGMT] = PHY_RW;
    igb_phy_regcap[IGP01E1000_PHY_PAGE_SELECT] = PHY_W;
}


static void
igb_phy_reg_write(IGBCore *core, uint32_t addr, uint16_t data)
{
    assert(addr <= MAX_PHY_REG_ADDRESS);

    if (addr == MII_BMCR) {
        igb_set_phy_ctrl(core, data);
    } else {
        core->phy[addr] = data;
    }
}

static void
igb_set_mdic(IGBCore *core, int index, uint32_t val)
{
    uint32_t data = val & E1000_MDIC_DATA_MASK;
    uint32_t addr = ((val & E1000_MDIC_REG_MASK) >> E1000_MDIC_REG_SHIFT);

    if ((val & E1000_MDIC_PHY_MASK) >> E1000_MDIC_PHY_SHIFT != 1) { /* phy # */
        val = core->mac[MDIC] | E1000_MDIC_ERROR;
    } else if (val & E1000_MDIC_OP_READ) {
        if (!(igb_phy_regcap[addr] & PHY_R)) {
            trace_igb_core_mdic_read_unhandled(addr);
            val |= E1000_MDIC_ERROR;
        } else {
            val = (val ^ data) | core->phy[addr];
            trace_igb_core_mdic_read(addr, val);
        }
    } else if (val & E1000_MDIC_OP_WRITE) {
        if (!(igb_phy_regcap[addr] & PHY_W)) {
            trace_igb_core_mdic_write_unhandled(addr);
            val |= E1000_MDIC_ERROR;
        } else {
            trace_igb_core_mdic_write(addr, data);
            igb_phy_reg_write(core, addr, data);
        }
    }
    core->mac[MDIC] = val | E1000_MDIC_READY;

    if (val & E1000_MDIC_INT_EN) {
        igb_raise_interrupts(core, ICR, E1000_ICR_MDAC);
    }
}

static void
igb_set_rdt(IGBCore *core, int index, uint32_t val)
{
    core->mac[index] = val & 0xffff;
    trace_e1000e_rx_set_rdt(igb_mq_queue_idx(RDT0, index), val);
    igb_start_recv(core);
}

static void
igb_set_status(IGBCore *core, int index, uint32_t val)
{
    if ((val & E1000_STATUS_PHYRA) == 0) {
        core->mac[index] &= ~E1000_STATUS_PHYRA;
    }
}

static void
igb_set_ctrlext(IGBCore *core, int index, uint32_t val)
{
    trace_igb_link_set_ext_params(!!(val & E1000_CTRL_EXT_ASDCHK),
                                  !!(val & E1000_CTRL_EXT_SPD_BYPS),
                                  !!(val & E1000_CTRL_EXT_PFRSTD));

    /* Zero self-clearing bits */
    val &= ~(E1000_CTRL_EXT_ASDCHK | E1000_CTRL_EXT_EE_RST);
    core->mac[CTRL_EXT] = val;

    if (core->mac[CTRL_EXT] & E1000_CTRL_EXT_PFRSTD) {
        for (int vfn = 0; vfn < IGB_MAX_VF_FUNCTIONS; vfn++) {
            core->mac[V2PMAILBOX0 + vfn] &= ~E1000_V2PMAILBOX_RSTI;
            core->mac[V2PMAILBOX0 + vfn] |= E1000_V2PMAILBOX_RSTD;
        }
    }
}

static void
igb_set_pbaclr(IGBCore *core, int index, uint32_t val)
{
    int i;

    core->mac[PBACLR] = val & E1000_PBACLR_VALID_MASK;

    if (!msix_enabled(core->owner)) {
        return;
    }

    for (i = 0; i < IGB_INTR_NUM; i++) {
        if (core->mac[PBACLR] & BIT(i)) {
            msix_clr_pending(core->owner, i);
        }
    }
}

static void
igb_set_fcrth(IGBCore *core, int index, uint32_t val)
{
    core->mac[FCRTH] = val & 0xFFF8;
}

static void
igb_set_fcrtl(IGBCore *core, int index, uint32_t val)
{
    core->mac[FCRTL] = val & 0x8000FFF8;
}

#define IGB_LOW_BITS_SET_FUNC(num)                             \
    static void                                                \
    igb_set_##num##bit(IGBCore *core, int index, uint32_t val) \
    {                                                          \
        core->mac[index] = val & (BIT(num) - 1);               \
    }

IGB_LOW_BITS_SET_FUNC(4)
IGB_LOW_BITS_SET_FUNC(13)
IGB_LOW_BITS_SET_FUNC(16)

static void
igb_set_dlen(IGBCore *core, int index, uint32_t val)
{
    core->mac[index] = val & 0xffff0;
}

static void
igb_set_dbal(IGBCore *core, int index, uint32_t val)
{
    core->mac[index] = val & E1000_XDBAL_MASK;
}

static void
igb_set_tdt(IGBCore *core, int index, uint32_t val)
{
    IGB_TxRing txr;
    int qn = igb_mq_queue_idx(TDT0, index);

    core->mac[index] = val & 0xffff;

    igb_tx_ring_init(core, &txr, qn);
    igb_start_xmit(core, &txr);
}

static void
igb_set_ics(IGBCore *core, int index, uint32_t val)
{
    trace_e1000e_irq_write_ics(val);
    igb_raise_interrupts(core, ICR, val);
}

static void
igb_set_imc(IGBCore *core, int index, uint32_t val)
{
    trace_e1000e_irq_ims_clear_set_imc(val);
    igb_lower_interrupts(core, IMS, val);
}

static void
igb_set_ims(IGBCore *core, int index, uint32_t val)
{
    igb_raise_interrupts(core, IMS, val & 0x77D4FBFD);
}

static void igb_nsicr(IGBCore *core)
{
    /*
     * If GPIE.NSICR = 0, then the clear of IMS will occur only if at
     * least one bit is set in the IMS and there is a true interrupt as
     * reflected in ICR.INTA.
     */
    if ((core->mac[GPIE] & E1000_GPIE_NSICR) ||
        (core->mac[IMS] && (core->mac[ICR] & E1000_ICR_INT_ASSERTED))) {
        igb_lower_interrupts(core, IMS, core->mac[IAM]);
    }
}

static void igb_set_icr(IGBCore *core, int index, uint32_t val)
{
    igb_nsicr(core);
    igb_lower_interrupts(core, ICR, val);
}

static uint32_t
igb_mac_readreg(IGBCore *core, int index)
{
    return core->mac[index];
}

static uint32_t
igb_mac_ics_read(IGBCore *core, int index)
{
    trace_e1000e_irq_read_ics(core->mac[ICS]);
    return core->mac[ICS];
}

static uint32_t
igb_mac_ims_read(IGBCore *core, int index)
{
    trace_e1000e_irq_read_ims(core->mac[IMS]);
    return core->mac[IMS];
}

static uint32_t
igb_mac_swsm_read(IGBCore *core, int index)
{
    uint32_t val = core->mac[SWSM];
    core->mac[SWSM] = val | E1000_SWSM_SMBI;
    return val;
}

static uint32_t
igb_mac_eitr_read(IGBCore *core, int index)
{
    return core->eitr_guest_value[index - EITR0];
}

static uint32_t igb_mac_vfmailbox_read(IGBCore *core, int index)
{
    uint32_t val = core->mac[index];

    core->mac[index] &= ~(E1000_V2PMAILBOX_PFSTS | E1000_V2PMAILBOX_PFACK |
                          E1000_V2PMAILBOX_RSTD);

    return val;
}

static uint32_t
igb_mac_icr_read(IGBCore *core, int index)
{
    uint32_t ret = core->mac[ICR];

    if (core->mac[GPIE] & E1000_GPIE_NSICR) {
        trace_igb_irq_icr_clear_gpie_nsicr();
        igb_lower_interrupts(core, ICR, 0xffffffff);
    } else if (core->mac[IMS] == 0) {
        trace_e1000e_irq_icr_clear_zero_ims();
        igb_lower_interrupts(core, ICR, 0xffffffff);
    } else if (core->mac[ICR] & E1000_ICR_INT_ASSERTED) {
        igb_lower_interrupts(core, ICR, 0xffffffff);
    } else if (!msix_enabled(core->owner)) {
        trace_e1000e_irq_icr_clear_nonmsix_icr_read();
        igb_lower_interrupts(core, ICR, 0xffffffff);
    }

    igb_nsicr(core);
    return ret;
}

static uint32_t
igb_mac_read_clr4(IGBCore *core, int index)
{
    uint32_t ret = core->mac[index];

    core->mac[index] = 0;
    return ret;
}

static uint32_t
igb_mac_read_clr8(IGBCore *core, int index)
{
    uint32_t ret = core->mac[index];

    core->mac[index] = 0;
    core->mac[index - 1] = 0;
    return ret;
}

static uint32_t
igb_get_ctrl(IGBCore *core, int index)
{
    uint32_t val = core->mac[CTRL];

    trace_e1000e_link_read_params(
        !!(val & E1000_CTRL_ASDE),
        (val & E1000_CTRL_SPD_SEL) >> E1000_CTRL_SPD_SHIFT,
        !!(val & E1000_CTRL_FRCSPD),
        !!(val & E1000_CTRL_FRCDPX),
        !!(val & E1000_CTRL_RFCE),
        !!(val & E1000_CTRL_TFCE));

    return val;
}

static uint32_t igb_get_status(IGBCore *core, int index)
{
    uint32_t res = core->mac[STATUS];
    uint16_t num_vfs = pcie_sriov_num_vfs(core->owner);

    if (core->mac[CTRL] & E1000_CTRL_FRCDPX) {
        res |= (core->mac[CTRL] & E1000_CTRL_FD) ? E1000_STATUS_FD : 0;
    } else {
        res |= E1000_STATUS_FD;
    }

    if ((core->mac[CTRL] & E1000_CTRL_FRCSPD) ||
        (core->mac[CTRL_EXT] & E1000_CTRL_EXT_SPD_BYPS)) {
        switch (core->mac[CTRL] & E1000_CTRL_SPD_SEL) {
        case E1000_CTRL_SPD_10:
            res |= E1000_STATUS_SPEED_10;
            break;
        case E1000_CTRL_SPD_100:
            res |= E1000_STATUS_SPEED_100;
            break;
        case E1000_CTRL_SPD_1000:
        default:
            res |= E1000_STATUS_SPEED_1000;
            break;
        }
    } else {
        res |= E1000_STATUS_SPEED_1000;
    }

    if (num_vfs) {
        res |= num_vfs << E1000_STATUS_NUM_VFS_SHIFT;
        res |= E1000_STATUS_IOV_MODE;
    }

    if (!(core->mac[CTRL] & E1000_CTRL_GIO_MASTER_DISABLE)) {
        res |= E1000_STATUS_GIO_MASTER_ENABLE;
    }

    return res;
}

static void
igb_mac_writereg(IGBCore *core, int index, uint32_t val)
{
    core->mac[index] = val;
}

static void
igb_mac_setmacaddr(IGBCore *core, int index, uint32_t val)
{
    uint32_t macaddr[2];

    core->mac[index] = val;

    macaddr[0] = cpu_to_le32(core->mac[RA]);
    macaddr[1] = cpu_to_le32(core->mac[RA + 1]);
    qemu_format_nic_info_str(qemu_get_queue(core->owner_nic),
        (uint8_t *) macaddr);

    trace_e1000e_mac_set_sw(MAC_ARG(macaddr));
}

static void
igb_set_eecd(IGBCore *core, int index, uint32_t val)
{
    static const uint32_t ro_bits = E1000_EECD_PRES          |
                                    E1000_EECD_AUTO_RD       |
                                    E1000_EECD_SIZE_EX_MASK;

    core->mac[EECD] = (core->mac[EECD] & ro_bits) | (val & ~ro_bits);
}

static void
igb_set_eerd(IGBCore *core, int index, uint32_t val)
{
    uint32_t addr = (val >> E1000_EERW_ADDR_SHIFT) & E1000_EERW_ADDR_MASK;
    uint32_t flags = 0;
    uint32_t data = 0;

    if ((addr < IGB_EEPROM_SIZE) && (val & E1000_EERW_START)) {
        data = core->eeprom[addr];
        flags = E1000_EERW_DONE;
    }

    core->mac[EERD] = flags                           |
                      (addr << E1000_EERW_ADDR_SHIFT) |
                      (data << E1000_EERW_DATA_SHIFT);
}

static void
igb_set_eitr(IGBCore *core, int index, uint32_t val)
{
    uint32_t eitr_num = index - EITR0;

    trace_igb_irq_eitr_set(eitr_num, val);

    core->eitr_guest_value[eitr_num] = val & ~E1000_EITR_CNT_IGNR;
    core->mac[index] = val & 0x7FFE;
}

static void
igb_update_rx_offloads(IGBCore *core)
{
    int cso_state = igb_rx_l4_cso_enabled(core);

    trace_e1000e_rx_set_cso(cso_state);

    if (core->has_vnet) {
        NetOffloads ol = {.csum = cso_state };

        qemu_set_offload(qemu_get_queue(core->owner_nic)->peer, &ol);
    }
}

static void
igb_set_rxcsum(IGBCore *core, int index, uint32_t val)
{
    core->mac[RXCSUM] = val;
    igb_update_rx_offloads(core);
}

static void
igb_set_gcr(IGBCore *core, int index, uint32_t val)
{
    uint32_t ro_bits = core->mac[GCR] & E1000_GCR_RO_BITS;
    core->mac[GCR] = (val & ~E1000_GCR_RO_BITS) | ro_bits;
}

static uint32_t igb_get_systiml(IGBCore *core, int index)
{
    e1000x_timestamp(core->mac, core->timadj, SYSTIML, SYSTIMH);
    return core->mac[SYSTIML];
}

static uint32_t igb_get_rxsatrh(IGBCore *core, int index)
{
    core->mac[TSYNCRXCTL] &= ~E1000_TSYNCRXCTL_VALID;
    return core->mac[RXSATRH];
}

static uint32_t igb_get_txstmph(IGBCore *core, int index)
{
    core->mac[TSYNCTXCTL] &= ~E1000_TSYNCTXCTL_VALID;
    return core->mac[TXSTMPH];
}

static void igb_set_timinca(IGBCore *core, int index, uint32_t val)
{
    e1000x_set_timinca(core->mac, &core->timadj, val);
}

static void igb_set_timadjh(IGBCore *core, int index, uint32_t val)
{
    core->mac[TIMADJH] = val;
    core->timadj += core->mac[TIMADJL] | ((int64_t)core->mac[TIMADJH] << 32);
}

typedef uint32_t (*readops)(IGBCore *, int);
static readops igb_macreg_readops[E1000E_MAC_SIZE];
enum { IGB_NREADOPS = E1000E_MAC_SIZE };

static void __attribute__((constructor)) igb_init_readops(void)
{
    igb_macreg_readops[WUFC] = igb_mac_readreg;
    igb_macreg_readops[MANC] = igb_mac_readreg;
    igb_macreg_readops[TOTL] = igb_mac_readreg;
    igb_macreg_readops[RDT0] = igb_mac_readreg;
    igb_macreg_readops[RDT1] = igb_mac_readreg;
    igb_macreg_readops[RDT2] = igb_mac_readreg;
    igb_macreg_readops[RDT3] = igb_mac_readreg;
    igb_macreg_readops[RDT4] = igb_mac_readreg;
    igb_macreg_readops[RDT5] = igb_mac_readreg;
    igb_macreg_readops[RDT6] = igb_mac_readreg;
    igb_macreg_readops[RDT7] = igb_mac_readreg;
    igb_macreg_readops[RDT8] = igb_mac_readreg;
    igb_macreg_readops[RDT9] = igb_mac_readreg;
    igb_macreg_readops[RDT10] = igb_mac_readreg;
    igb_macreg_readops[RDT11] = igb_mac_readreg;
    igb_macreg_readops[RDT12] = igb_mac_readreg;
    igb_macreg_readops[RDT13] = igb_mac_readreg;
    igb_macreg_readops[RDT14] = igb_mac_readreg;
    igb_macreg_readops[RDT15] = igb_mac_readreg;
    igb_macreg_readops[RDBAH0] = igb_mac_readreg;
    igb_macreg_readops[RDBAH1] = igb_mac_readreg;
    igb_macreg_readops[RDBAH2] = igb_mac_readreg;
    igb_macreg_readops[RDBAH3] = igb_mac_readreg;
    igb_macreg_readops[RDBAH4] = igb_mac_readreg;
    igb_macreg_readops[RDBAH5] = igb_mac_readreg;
    igb_macreg_readops[RDBAH6] = igb_mac_readreg;
    igb_macreg_readops[RDBAH7] = igb_mac_readreg;
    igb_macreg_readops[RDBAH8] = igb_mac_readreg;
    igb_macreg_readops[RDBAH9] = igb_mac_readreg;
    igb_macreg_readops[RDBAH10] = igb_mac_readreg;
    igb_macreg_readops[RDBAH11] = igb_mac_readreg;
    igb_macreg_readops[RDBAH12] = igb_mac_readreg;
    igb_macreg_readops[RDBAH13] = igb_mac_readreg;
    igb_macreg_readops[RDBAH14] = igb_mac_readreg;
    igb_macreg_readops[RDBAH15] = igb_mac_readreg;
    igb_macreg_readops[TDBAL0] = igb_mac_readreg;
    igb_macreg_readops[TDBAL1] = igb_mac_readreg;
    igb_macreg_readops[TDBAL2] = igb_mac_readreg;
    igb_macreg_readops[TDBAL3] = igb_mac_readreg;
    igb_macreg_readops[TDBAL4] = igb_mac_readreg;
    igb_macreg_readops[TDBAL5] = igb_mac_readreg;
    igb_macreg_readops[TDBAL6] = igb_mac_readreg;
    igb_macreg_readops[TDBAL7] = igb_mac_readreg;
    igb_macreg_readops[TDBAL8] = igb_mac_readreg;
    igb_macreg_readops[TDBAL9] = igb_mac_readreg;
    igb_macreg_readops[TDBAL10] = igb_mac_readreg;
    igb_macreg_readops[TDBAL11] = igb_mac_readreg;
    igb_macreg_readops[TDBAL12] = igb_mac_readreg;
    igb_macreg_readops[TDBAL13] = igb_mac_readreg;
    igb_macreg_readops[TDBAL14] = igb_mac_readreg;
    igb_macreg_readops[TDBAL15] = igb_mac_readreg;
    igb_macreg_readops[RDLEN0] = igb_mac_readreg;
    igb_macreg_readops[RDLEN1] = igb_mac_readreg;
    igb_macreg_readops[RDLEN2] = igb_mac_readreg;
    igb_macreg_readops[RDLEN3] = igb_mac_readreg;
    igb_macreg_readops[RDLEN4] = igb_mac_readreg;
    igb_macreg_readops[RDLEN5] = igb_mac_readreg;
    igb_macreg_readops[RDLEN6] = igb_mac_readreg;
    igb_macreg_readops[RDLEN7] = igb_mac_readreg;
    igb_macreg_readops[RDLEN8] = igb_mac_readreg;
    igb_macreg_readops[RDLEN9] = igb_mac_readreg;
    igb_macreg_readops[RDLEN10] = igb_mac_readreg;
    igb_macreg_readops[RDLEN11] = igb_mac_readreg;
    igb_macreg_readops[RDLEN12] = igb_mac_readreg;
    igb_macreg_readops[RDLEN13] = igb_mac_readreg;
    igb_macreg_readops[RDLEN14] = igb_mac_readreg;
    igb_macreg_readops[RDLEN15] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL0] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL1] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL2] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL3] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL4] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL5] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL6] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL7] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL8] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL9] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL10] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL11] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL12] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL13] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL14] = igb_mac_readreg;
    igb_macreg_readops[SRRCTL15] = igb_mac_readreg;
    igb_macreg_readops[LATECOL] = igb_mac_readreg;
    igb_macreg_readops[XONTXC] = igb_mac_readreg;
    igb_macreg_readops[TDFH] = igb_mac_readreg;
    igb_macreg_readops[TDFT] = igb_mac_readreg;
    igb_macreg_readops[TDFHS] = igb_mac_readreg;
    igb_macreg_readops[TDFTS] = igb_mac_readreg;
    igb_macreg_readops[TDFPC] = igb_mac_readreg;
    igb_macreg_readops[WUS] = igb_mac_readreg;
    igb_macreg_readops[RDFH] = igb_mac_readreg;
    igb_macreg_readops[RDFT] = igb_mac_readreg;
    igb_macreg_readops[RDFHS] = igb_mac_readreg;
    igb_macreg_readops[RDFTS] = igb_mac_readreg;
    igb_macreg_readops[RDFPC] = igb_mac_readreg;
    igb_macreg_readops[GORCL] = igb_mac_readreg;
    igb_macreg_readops[MGTPRC] = igb_mac_readreg;
    igb_macreg_readops[EERD] = igb_mac_readreg;
    igb_macreg_readops[EIAC] = igb_mac_readreg;
    igb_macreg_readops[MANC2H] = igb_mac_readreg;
    igb_macreg_readops[RXCSUM] = igb_mac_readreg;
    igb_macreg_readops[GSCL_3] = igb_mac_readreg;
    igb_macreg_readops[GSCN_2] = igb_mac_readreg;
    igb_macreg_readops[FCAH] = igb_mac_readreg;
    igb_macreg_readops[FCRTH] = igb_mac_readreg;
    igb_macreg_readops[FLOP] = igb_mac_readreg;
    igb_macreg_readops[RXSTMPH] = igb_mac_readreg;
    igb_macreg_readops[TXSTMPL] = igb_mac_readreg;
    igb_macreg_readops[TIMADJL] = igb_mac_readreg;
    igb_macreg_readops[RDH0] = igb_mac_readreg;
    igb_macreg_readops[RDH1] = igb_mac_readreg;
    igb_macreg_readops[RDH2] = igb_mac_readreg;
    igb_macreg_readops[RDH3] = igb_mac_readreg;
    igb_macreg_readops[RDH4] = igb_mac_readreg;
    igb_macreg_readops[RDH5] = igb_mac_readreg;
    igb_macreg_readops[RDH6] = igb_mac_readreg;
    igb_macreg_readops[RDH7] = igb_mac_readreg;
    igb_macreg_readops[RDH8] = igb_mac_readreg;
    igb_macreg_readops[RDH9] = igb_mac_readreg;
    igb_macreg_readops[RDH10] = igb_mac_readreg;
    igb_macreg_readops[RDH11] = igb_mac_readreg;
    igb_macreg_readops[RDH12] = igb_mac_readreg;
    igb_macreg_readops[RDH13] = igb_mac_readreg;
    igb_macreg_readops[RDH14] = igb_mac_readreg;
    igb_macreg_readops[RDH15] = igb_mac_readreg;
    igb_macreg_readops[TDT0] = igb_mac_readreg;
    igb_macreg_readops[TDT1] = igb_mac_readreg;
    igb_macreg_readops[TDT2] = igb_mac_readreg;
    igb_macreg_readops[TDT3] = igb_mac_readreg;
    igb_macreg_readops[TDT4] = igb_mac_readreg;
    igb_macreg_readops[TDT5] = igb_mac_readreg;
    igb_macreg_readops[TDT6] = igb_mac_readreg;
    igb_macreg_readops[TDT7] = igb_mac_readreg;
    igb_macreg_readops[TDT8] = igb_mac_readreg;
    igb_macreg_readops[TDT9] = igb_mac_readreg;
    igb_macreg_readops[TDT10] = igb_mac_readreg;
    igb_macreg_readops[TDT11] = igb_mac_readreg;
    igb_macreg_readops[TDT12] = igb_mac_readreg;
    igb_macreg_readops[TDT13] = igb_mac_readreg;
    igb_macreg_readops[TDT14] = igb_mac_readreg;
    igb_macreg_readops[TDT15] = igb_mac_readreg;
    igb_macreg_readops[TNCRS] = igb_mac_readreg;
    igb_macreg_readops[RJC] = igb_mac_readreg;
    igb_macreg_readops[IAM] = igb_mac_readreg;
    igb_macreg_readops[GSCL_2] = igb_mac_readreg;
    igb_macreg_readops[TIPG] = igb_mac_readreg;
    igb_macreg_readops[FLMNGCTL] = igb_mac_readreg;
    igb_macreg_readops[FLMNGCNT] = igb_mac_readreg;
    igb_macreg_readops[TSYNCTXCTL] = igb_mac_readreg;
    igb_macreg_readops[EEMNGDATA] = igb_mac_readreg;
    igb_macreg_readops[CTRL_EXT] = igb_mac_readreg;
    igb_macreg_readops[SYSTIMH] = igb_mac_readreg;
    igb_macreg_readops[EEMNGCTL] = igb_mac_readreg;
    igb_macreg_readops[FLMNGDATA] = igb_mac_readreg;
    igb_macreg_readops[TSYNCRXCTL] = igb_mac_readreg;
    igb_macreg_readops[LEDCTL] = igb_mac_readreg;
    igb_macreg_readops[TCTL] = igb_mac_readreg;
    igb_macreg_readops[TCTL_EXT] = igb_mac_readreg;
    igb_macreg_readops[DTXCTL] = igb_mac_readreg;
    igb_macreg_readops[RXPBS] = igb_mac_readreg;
    igb_macreg_readops[TDH0] = igb_mac_readreg;
    igb_macreg_readops[TDH1] = igb_mac_readreg;
    igb_macreg_readops[TDH2] = igb_mac_readreg;
    igb_macreg_readops[TDH3] = igb_mac_readreg;
    igb_macreg_readops[TDH4] = igb_mac_readreg;
    igb_macreg_readops[TDH5] = igb_mac_readreg;
    igb_macreg_readops[TDH6] = igb_mac_readreg;
    igb_macreg_readops[TDH7] = igb_mac_readreg;
    igb_macreg_readops[TDH8] = igb_mac_readreg;
    igb_macreg_readops[TDH9] = igb_mac_readreg;
    igb_macreg_readops[TDH10] = igb_mac_readreg;
    igb_macreg_readops[TDH11] = igb_mac_readreg;
    igb_macreg_readops[TDH12] = igb_mac_readreg;
    igb_macreg_readops[TDH13] = igb_mac_readreg;
    igb_macreg_readops[TDH14] = igb_mac_readreg;
    igb_macreg_readops[TDH15] = igb_mac_readreg;
    igb_macreg_readops[ECOL] = igb_mac_readreg;
    igb_macreg_readops[DC] = igb_mac_readreg;
    igb_macreg_readops[RLEC] = igb_mac_readreg;
    igb_macreg_readops[XOFFTXC] = igb_mac_readreg;
    igb_macreg_readops[RFC] = igb_mac_readreg;
    igb_macreg_readops[RNBC] = igb_mac_readreg;
    igb_macreg_readops[MGTPTC] = igb_mac_readreg;
    igb_macreg_readops[TIMINCA] = igb_mac_readreg;
    igb_macreg_readops[FACTPS] = igb_mac_readreg;
    igb_macreg_readops[GSCL_1] = igb_mac_readreg;
    igb_macreg_readops[GSCN_0] = igb_mac_readreg;
    igb_macreg_readops[PBACLR] = igb_mac_readreg;
    igb_macreg_readops[FCTTV] = igb_mac_readreg;
    igb_macreg_readops[RXSATRL] = igb_mac_readreg;
    igb_macreg_readops[TORL] = igb_mac_readreg;
    igb_macreg_readops[TDLEN0] = igb_mac_readreg;
    igb_macreg_readops[TDLEN1] = igb_mac_readreg;
    igb_macreg_readops[TDLEN2] = igb_mac_readreg;
    igb_macreg_readops[TDLEN3] = igb_mac_readreg;
    igb_macreg_readops[TDLEN4] = igb_mac_readreg;
    igb_macreg_readops[TDLEN5] = igb_mac_readreg;
    igb_macreg_readops[TDLEN6] = igb_mac_readreg;
    igb_macreg_readops[TDLEN7] = igb_mac_readreg;
    igb_macreg_readops[TDLEN8] = igb_mac_readreg;
    igb_macreg_readops[TDLEN9] = igb_mac_readreg;
    igb_macreg_readops[TDLEN10] = igb_mac_readreg;
    igb_macreg_readops[TDLEN11] = igb_mac_readreg;
    igb_macreg_readops[TDLEN12] = igb_mac_readreg;
    igb_macreg_readops[TDLEN13] = igb_mac_readreg;
    igb_macreg_readops[TDLEN14] = igb_mac_readreg;
    igb_macreg_readops[TDLEN15] = igb_mac_readreg;
    igb_macreg_readops[MCC] = igb_mac_readreg;
    igb_macreg_readops[WUC] = igb_mac_readreg;
    igb_macreg_readops[EECD] = igb_mac_readreg;
    igb_macreg_readops[FCRTV] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL0] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL1] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL2] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL3] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL4] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL5] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL6] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL7] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL8] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL9] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL10] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL11] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL12] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL13] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL14] = igb_mac_readreg;
    igb_macreg_readops[TXDCTL15] = igb_mac_readreg;
    igb_macreg_readops[TXCTL0] = igb_mac_readreg;
    igb_macreg_readops[TXCTL1] = igb_mac_readreg;
    igb_macreg_readops[TXCTL2] = igb_mac_readreg;
    igb_macreg_readops[TXCTL3] = igb_mac_readreg;
    igb_macreg_readops[TXCTL4] = igb_mac_readreg;
    igb_macreg_readops[TXCTL5] = igb_mac_readreg;
    igb_macreg_readops[TXCTL6] = igb_mac_readreg;
    igb_macreg_readops[TXCTL7] = igb_mac_readreg;
    igb_macreg_readops[TXCTL8] = igb_mac_readreg;
    igb_macreg_readops[TXCTL9] = igb_mac_readreg;
    igb_macreg_readops[TXCTL10] = igb_mac_readreg;
    igb_macreg_readops[TXCTL11] = igb_mac_readreg;
    igb_macreg_readops[TXCTL12] = igb_mac_readreg;
    igb_macreg_readops[TXCTL13] = igb_mac_readreg;
    igb_macreg_readops[TXCTL14] = igb_mac_readreg;
    igb_macreg_readops[TXCTL15] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL0] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL1] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL2] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL3] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL4] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL5] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL6] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL7] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL8] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL9] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL10] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL11] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL12] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL13] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL14] = igb_mac_readreg;
    igb_macreg_readops[TDWBAL15] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH0] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH1] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH2] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH3] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH4] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH5] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH6] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH7] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH8] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH9] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH10] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH11] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH12] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH13] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH14] = igb_mac_readreg;
    igb_macreg_readops[TDWBAH15] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL0] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL1] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL2] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL3] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL4] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL5] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL6] = igb_mac_readreg;
    igb_macreg_readops[PVTCTRL7] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS0] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS1] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS2] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS3] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS4] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS5] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS6] = igb_mac_readreg;
    igb_macreg_readops[PVTEIMS7] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC0] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC1] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC2] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC3] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC4] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC5] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC6] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAC7] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM0] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM1] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM2] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM3] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM4] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM5] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM6] = igb_mac_readreg;
    igb_macreg_readops[PVTEIAM7] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGORC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTC7] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC0] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC1] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC2] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC3] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC4] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC5] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC6] = igb_mac_readreg;
    igb_macreg_readops[PVFMPRC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGPRLBC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGPTLBC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGORLBC7] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC0] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC1] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC2] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC3] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC4] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC5] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC6] = igb_mac_readreg;
    igb_macreg_readops[PVFGOTLBC7] = igb_mac_readreg;
    igb_macreg_readops[RCTL] = igb_mac_readreg;
    igb_macreg_readops[MDIC] = igb_mac_readreg;
    igb_macreg_readops[FCRUC] = igb_mac_readreg;
    igb_macreg_readops[VET] = igb_mac_readreg;
    igb_macreg_readops[RDBAL0] = igb_mac_readreg;
    igb_macreg_readops[RDBAL1] = igb_mac_readreg;
    igb_macreg_readops[RDBAL2] = igb_mac_readreg;
    igb_macreg_readops[RDBAL3] = igb_mac_readreg;
    igb_macreg_readops[RDBAL4] = igb_mac_readreg;
    igb_macreg_readops[RDBAL5] = igb_mac_readreg;
    igb_macreg_readops[RDBAL6] = igb_mac_readreg;
    igb_macreg_readops[RDBAL7] = igb_mac_readreg;
    igb_macreg_readops[RDBAL8] = igb_mac_readreg;
    igb_macreg_readops[RDBAL9] = igb_mac_readreg;
    igb_macreg_readops[RDBAL10] = igb_mac_readreg;
    igb_macreg_readops[RDBAL11] = igb_mac_readreg;
    igb_macreg_readops[RDBAL12] = igb_mac_readreg;
    igb_macreg_readops[RDBAL13] = igb_mac_readreg;
    igb_macreg_readops[RDBAL14] = igb_mac_readreg;
    igb_macreg_readops[RDBAL15] = igb_mac_readreg;
    igb_macreg_readops[TDBAH0] = igb_mac_readreg;
    igb_macreg_readops[TDBAH1] = igb_mac_readreg;
    igb_macreg_readops[TDBAH2] = igb_mac_readreg;
    igb_macreg_readops[TDBAH3] = igb_mac_readreg;
    igb_macreg_readops[TDBAH4] = igb_mac_readreg;
    igb_macreg_readops[TDBAH5] = igb_mac_readreg;
    igb_macreg_readops[TDBAH6] = igb_mac_readreg;
    igb_macreg_readops[TDBAH7] = igb_mac_readreg;
    igb_macreg_readops[TDBAH8] = igb_mac_readreg;
    igb_macreg_readops[TDBAH9] = igb_mac_readreg;
    igb_macreg_readops[TDBAH10] = igb_mac_readreg;
    igb_macreg_readops[TDBAH11] = igb_mac_readreg;
    igb_macreg_readops[TDBAH12] = igb_mac_readreg;
    igb_macreg_readops[TDBAH13] = igb_mac_readreg;
    igb_macreg_readops[TDBAH14] = igb_mac_readreg;
    igb_macreg_readops[TDBAH15] = igb_mac_readreg;
    igb_macreg_readops[SCC] = igb_mac_readreg;
    igb_macreg_readops[COLC] = igb_mac_readreg;
    igb_macreg_readops[XOFFRXC] = igb_mac_readreg;
    igb_macreg_readops[IPAV] = igb_mac_readreg;
    igb_macreg_readops[GOTCL] = igb_mac_readreg;
    igb_macreg_readops[MGTPDC] = igb_mac_readreg;
    igb_macreg_readops[GCR] = igb_mac_readreg;
    igb_macreg_readops[MFVAL] = igb_mac_readreg;
    igb_macreg_readops[FUNCTAG] = igb_mac_readreg;
    igb_macreg_readops[GSCL_4] = igb_mac_readreg;
    igb_macreg_readops[GSCN_3] = igb_mac_readreg;
    igb_macreg_readops[MRQC] = igb_mac_readreg;
    igb_macreg_readops[FCT] = igb_mac_readreg;
    igb_macreg_readops[FLA] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL0] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL1] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL2] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL3] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL4] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL5] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL6] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL7] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL8] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL9] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL10] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL11] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL12] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL13] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL14] = igb_mac_readreg;
    igb_macreg_readops[RXDCTL15] = igb_mac_readreg;
    igb_macreg_readops[RXSTMPL] = igb_mac_readreg;
    igb_macreg_readops[TIMADJH] = igb_mac_readreg;
    igb_macreg_readops[FCRTL] = igb_mac_readreg;
    igb_macreg_readops[XONRXC] = igb_mac_readreg;
    igb_macreg_readops[RFCTL] = igb_mac_readreg;
    igb_macreg_readops[GSCN_1] = igb_mac_readreg;
    igb_macreg_readops[FCAL] = igb_mac_readreg;
    igb_macreg_readops[GPIE] = igb_mac_readreg;
    igb_macreg_readops[TXPBS] = igb_mac_readreg;
    igb_macreg_readops[RLPML] = igb_mac_readreg;
    igb_macreg_readops[TOTH] = igb_mac_read_clr8;
    igb_macreg_readops[GOTCH] = igb_mac_read_clr8;
    igb_macreg_readops[PRC64] = igb_mac_read_clr4;
    igb_macreg_readops[PRC255] = igb_mac_read_clr4;
    igb_macreg_readops[PRC1023] = igb_mac_read_clr4;
    igb_macreg_readops[PTC64] = igb_mac_read_clr4;
    igb_macreg_readops[PTC255] = igb_mac_read_clr4;
    igb_macreg_readops[PTC1023] = igb_mac_read_clr4;
    igb_macreg_readops[GPRC] = igb_mac_read_clr4;
    igb_macreg_readops[TPT] = igb_mac_read_clr4;
    igb_macreg_readops[RUC] = igb_mac_read_clr4;
    igb_macreg_readops[BPRC] = igb_mac_read_clr4;
    igb_macreg_readops[MPTC] = igb_mac_read_clr4;
    igb_macreg_readops[IAC] = igb_mac_read_clr4;
    igb_macreg_readops[ICR] = igb_mac_icr_read;
    igb_macreg_readops[STATUS] = igb_get_status;
    igb_macreg_readops[ICS] = igb_mac_ics_read;
    /*
    * 8.8.10: Reading the IMC register returns the value of the IMS register.
    */
    igb_macreg_readops[IMC] = igb_mac_ims_read;
    igb_macreg_readops[TORH] = igb_mac_read_clr8;
    igb_macreg_readops[GORCH] = igb_mac_read_clr8;
    igb_macreg_readops[PRC127] = igb_mac_read_clr4;
    igb_macreg_readops[PRC511] = igb_mac_read_clr4;
    igb_macreg_readops[PRC1522] = igb_mac_read_clr4;
    igb_macreg_readops[PTC127] = igb_mac_read_clr4;
    igb_macreg_readops[PTC511] = igb_mac_read_clr4;
    igb_macreg_readops[PTC1522] = igb_mac_read_clr4;
    igb_macreg_readops[GPTC] = igb_mac_read_clr4;
    igb_macreg_readops[TPR] = igb_mac_read_clr4;
    igb_macreg_readops[ROC] = igb_mac_read_clr4;
    igb_macreg_readops[MPRC] = igb_mac_read_clr4;
    igb_macreg_readops[BPTC] = igb_mac_read_clr4;
    igb_macreg_readops[TSCTC] = igb_mac_read_clr4;
    igb_macreg_readops[CTRL] = igb_get_ctrl;
    igb_macreg_readops[SWSM] = igb_mac_swsm_read;
    igb_macreg_readops[IMS] = igb_mac_ims_read;
    igb_macreg_readops[SYSTIML] = igb_get_systiml;
    igb_macreg_readops[RXSATRH] = igb_get_rxsatrh;
    igb_macreg_readops[TXSTMPH] = igb_get_txstmph;
    for (int i = CRCERRS; i <= MPC; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = IP6AT; i <= IP6AT + 3; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = IP4AT; i <= IP4AT + 6; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = RA; i <= RA + 31; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = RA2; i <= RA2 + 31; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = WUPM; i <= WUPM + 31; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = MTA; i <= MTA + E1000_MC_TBL_SIZE - 1; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = VFTA; i <= VFTA + E1000_VLAN_FILTER_TBL_SIZE - 1; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = FFMT; i <= FFMT + 254; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = MDEF; i <= MDEF + 7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = FTFT; i <= FTFT + 254; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = RETA; i <= RETA + 31; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = RSSRK; i <= RSSRK + 9; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = MAVTV0; i <= MAVTV3; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = EITR0; i <= EITR0 + IGB_INTR_NUM - 1; i++)
        igb_macreg_readops[i] = igb_mac_eitr_read;
    igb_macreg_readops[PVTEICR0] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR1] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR2] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR3] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR4] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR5] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR6] = igb_mac_read_clr4;
    igb_macreg_readops[PVTEICR7] = igb_mac_read_clr4;
    /* IGB specific: */
    igb_macreg_readops[FWSM] = igb_mac_readreg;
    igb_macreg_readops[SW_FW_SYNC] = igb_mac_readreg;
    igb_macreg_readops[HTCBDPC] = igb_mac_read_clr4;
    igb_macreg_readops[EICR] = igb_mac_read_clr4;
    igb_macreg_readops[EIMS] = igb_mac_readreg;
    igb_macreg_readops[EIAM] = igb_mac_readreg;
    for (int i = IVAR0; i <= IVAR0 + 7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    igb_macreg_readops[IVAR_MISC] = igb_mac_readreg;
    igb_macreg_readops[TSYNCRXCFG] = igb_mac_readreg;
    for (int i = ETQF0; i <= ETQF0 + 7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    igb_macreg_readops[VT_CTL] = igb_mac_readreg;
    for (int i = P2VMAILBOX0; i <= P2VMAILBOX7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = V2PMAILBOX0; i <= V2PMAILBOX7; i++)
        igb_macreg_readops[i] = igb_mac_vfmailbox_read;
    igb_macreg_readops[MBVFICR] = igb_mac_readreg;
    for (int i = VMBMEM0; i <= VMBMEM0 + 127; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    igb_macreg_readops[MBVFIMR] = igb_mac_readreg;
    igb_macreg_readops[VFLRE] = igb_mac_readreg;
    igb_macreg_readops[VFRE] = igb_mac_readreg;
    igb_macreg_readops[VFTE] = igb_mac_readreg;
    igb_macreg_readops[QDE] = igb_mac_readreg;
    igb_macreg_readops[DTXSWC] = igb_mac_readreg;
    igb_macreg_readops[RPLOLR] = igb_mac_readreg;
    for (int i = VLVF0; i <= VLVF0 + E1000_VLVF_ARRAY_SIZE - 1; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = VMVIR0; i <= VMVIR7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = VMOLR0; i <= VMOLR7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    igb_macreg_readops[WVBR] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC0] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC1] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC2] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC3] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC4] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC5] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC6] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC7] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC8] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC9] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC10] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC11] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC12] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC13] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC14] = igb_mac_read_clr4;
    igb_macreg_readops[RQDPC15] = igb_mac_read_clr4;
    for (int i = VTIVAR; i <= VTIVAR + 7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
    for (int i = VTIVAR_MISC; i <= VTIVAR_MISC + 7; i++)
        igb_macreg_readops[i] = igb_mac_readreg;
}



typedef void (*writeops)(IGBCore *, int, uint32_t);
static writeops igb_macreg_writeops[E1000E_MAC_SIZE];
enum { IGB_NWRITEOPS = E1000E_MAC_SIZE };

static void __attribute__((constructor)) igb_init_writeops(void)
{
    igb_macreg_writeops[SWSM] = igb_mac_writereg;
    igb_macreg_writeops[WUFC] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH0] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH1] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH2] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH3] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH4] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH5] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH6] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH7] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH8] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH9] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH10] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH11] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH12] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH13] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH14] = igb_mac_writereg;
    igb_macreg_writeops[RDBAH15] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL0] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL1] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL2] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL3] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL4] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL5] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL6] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL7] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL8] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL9] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL10] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL11] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL12] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL13] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL14] = igb_mac_writereg;
    igb_macreg_writeops[SRRCTL15] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL0] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL1] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL2] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL3] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL4] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL5] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL6] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL7] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL8] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL9] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL10] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL11] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL12] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL13] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL14] = igb_mac_writereg;
    igb_macreg_writeops[RXDCTL15] = igb_mac_writereg;
    igb_macreg_writeops[LEDCTL] = igb_mac_writereg;
    igb_macreg_writeops[TCTL] = igb_mac_writereg;
    igb_macreg_writeops[TCTL_EXT] = igb_mac_writereg;
    igb_macreg_writeops[DTXCTL] = igb_mac_writereg;
    igb_macreg_writeops[RXPBS] = igb_mac_writereg;
    igb_macreg_writeops[RQDPC0] = igb_mac_writereg;
    igb_macreg_writeops[FCAL] = igb_mac_writereg;
    igb_macreg_writeops[FCRUC] = igb_mac_writereg;
    igb_macreg_writeops[WUC] = igb_mac_writereg;
    igb_macreg_writeops[WUS] = igb_mac_writereg;
    igb_macreg_writeops[IPAV] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH0] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH1] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH2] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH3] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH4] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH5] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH6] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH7] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH8] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH9] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH10] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH11] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH12] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH13] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH14] = igb_mac_writereg;
    igb_macreg_writeops[TDBAH15] = igb_mac_writereg;
    igb_macreg_writeops[IAM] = igb_mac_writereg;
    igb_macreg_writeops[MANC] = igb_mac_writereg;
    igb_macreg_writeops[MANC2H] = igb_mac_writereg;
    igb_macreg_writeops[MFVAL] = igb_mac_writereg;
    igb_macreg_writeops[FACTPS] = igb_mac_writereg;
    igb_macreg_writeops[FUNCTAG] = igb_mac_writereg;
    igb_macreg_writeops[GSCL_1] = igb_mac_writereg;
    igb_macreg_writeops[GSCL_2] = igb_mac_writereg;
    igb_macreg_writeops[GSCL_3] = igb_mac_writereg;
    igb_macreg_writeops[GSCL_4] = igb_mac_writereg;
    igb_macreg_writeops[GSCN_0] = igb_mac_writereg;
    igb_macreg_writeops[GSCN_1] = igb_mac_writereg;
    igb_macreg_writeops[GSCN_2] = igb_mac_writereg;
    igb_macreg_writeops[GSCN_3] = igb_mac_writereg;
    igb_macreg_writeops[MRQC] = igb_mac_writereg;
    igb_macreg_writeops[FLOP] = igb_mac_writereg;
    igb_macreg_writeops[FLA] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL0] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL1] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL2] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL3] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL4] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL5] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL6] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL7] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL8] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL9] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL10] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL11] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL12] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL13] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL14] = igb_mac_writereg;
    igb_macreg_writeops[TXDCTL15] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL0] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL1] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL2] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL3] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL4] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL5] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL6] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL7] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL8] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL9] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL10] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL11] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL12] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL13] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL14] = igb_mac_writereg;
    igb_macreg_writeops[TXCTL15] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL0] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL1] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL2] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL3] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL4] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL5] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL6] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL7] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL8] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL9] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL10] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL11] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL12] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL13] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL14] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAL15] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH0] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH1] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH2] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH3] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH4] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH5] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH6] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH7] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH8] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH9] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH10] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH11] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH12] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH13] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH14] = igb_mac_writereg;
    igb_macreg_writeops[TDWBAH15] = igb_mac_writereg;
    igb_macreg_writeops[TIPG] = igb_mac_writereg;
    igb_macreg_writeops[RXSTMPH] = igb_mac_writereg;
    igb_macreg_writeops[RXSTMPL] = igb_mac_writereg;
    igb_macreg_writeops[RXSATRL] = igb_mac_writereg;
    igb_macreg_writeops[RXSATRH] = igb_mac_writereg;
    igb_macreg_writeops[TXSTMPL] = igb_mac_writereg;
    igb_macreg_writeops[TXSTMPH] = igb_mac_writereg;
    igb_macreg_writeops[SYSTIML] = igb_mac_writereg;
    igb_macreg_writeops[SYSTIMH] = igb_mac_writereg;
    igb_macreg_writeops[TIMADJL] = igb_mac_writereg;
    igb_macreg_writeops[TSYNCRXCTL] = igb_mac_writereg;
    igb_macreg_writeops[TSYNCTXCTL] = igb_mac_writereg;
    igb_macreg_writeops[EEMNGCTL] = igb_mac_writereg;
    igb_macreg_writeops[GPIE] = igb_mac_writereg;
    igb_macreg_writeops[TXPBS] = igb_mac_writereg;
    igb_macreg_writeops[RLPML] = igb_mac_writereg;
    igb_macreg_writeops[VET] = igb_mac_writereg;
    igb_macreg_writeops[TDH0] = igb_set_16bit;
    igb_macreg_writeops[TDH1] = igb_set_16bit;
    igb_macreg_writeops[TDH2] = igb_set_16bit;
    igb_macreg_writeops[TDH3] = igb_set_16bit;
    igb_macreg_writeops[TDH4] = igb_set_16bit;
    igb_macreg_writeops[TDH5] = igb_set_16bit;
    igb_macreg_writeops[TDH6] = igb_set_16bit;
    igb_macreg_writeops[TDH7] = igb_set_16bit;
    igb_macreg_writeops[TDH8] = igb_set_16bit;
    igb_macreg_writeops[TDH9] = igb_set_16bit;
    igb_macreg_writeops[TDH10] = igb_set_16bit;
    igb_macreg_writeops[TDH11] = igb_set_16bit;
    igb_macreg_writeops[TDH12] = igb_set_16bit;
    igb_macreg_writeops[TDH13] = igb_set_16bit;
    igb_macreg_writeops[TDH14] = igb_set_16bit;
    igb_macreg_writeops[TDH15] = igb_set_16bit;
    igb_macreg_writeops[TDT0] = igb_set_tdt;
    igb_macreg_writeops[TDT1] = igb_set_tdt;
    igb_macreg_writeops[TDT2] = igb_set_tdt;
    igb_macreg_writeops[TDT3] = igb_set_tdt;
    igb_macreg_writeops[TDT4] = igb_set_tdt;
    igb_macreg_writeops[TDT5] = igb_set_tdt;
    igb_macreg_writeops[TDT6] = igb_set_tdt;
    igb_macreg_writeops[TDT7] = igb_set_tdt;
    igb_macreg_writeops[TDT8] = igb_set_tdt;
    igb_macreg_writeops[TDT9] = igb_set_tdt;
    igb_macreg_writeops[TDT10] = igb_set_tdt;
    igb_macreg_writeops[TDT11] = igb_set_tdt;
    igb_macreg_writeops[TDT12] = igb_set_tdt;
    igb_macreg_writeops[TDT13] = igb_set_tdt;
    igb_macreg_writeops[TDT14] = igb_set_tdt;
    igb_macreg_writeops[TDT15] = igb_set_tdt;
    igb_macreg_writeops[MDIC] = igb_set_mdic;
    igb_macreg_writeops[ICS] = igb_set_ics;
    igb_macreg_writeops[RDH0] = igb_set_16bit;
    igb_macreg_writeops[RDH1] = igb_set_16bit;
    igb_macreg_writeops[RDH2] = igb_set_16bit;
    igb_macreg_writeops[RDH3] = igb_set_16bit;
    igb_macreg_writeops[RDH4] = igb_set_16bit;
    igb_macreg_writeops[RDH5] = igb_set_16bit;
    igb_macreg_writeops[RDH6] = igb_set_16bit;
    igb_macreg_writeops[RDH7] = igb_set_16bit;
    igb_macreg_writeops[RDH8] = igb_set_16bit;
    igb_macreg_writeops[RDH9] = igb_set_16bit;
    igb_macreg_writeops[RDH10] = igb_set_16bit;
    igb_macreg_writeops[RDH11] = igb_set_16bit;
    igb_macreg_writeops[RDH12] = igb_set_16bit;
    igb_macreg_writeops[RDH13] = igb_set_16bit;
    igb_macreg_writeops[RDH14] = igb_set_16bit;
    igb_macreg_writeops[RDH15] = igb_set_16bit;
    igb_macreg_writeops[RDT0] = igb_set_rdt;
    igb_macreg_writeops[RDT1] = igb_set_rdt;
    igb_macreg_writeops[RDT2] = igb_set_rdt;
    igb_macreg_writeops[RDT3] = igb_set_rdt;
    igb_macreg_writeops[RDT4] = igb_set_rdt;
    igb_macreg_writeops[RDT5] = igb_set_rdt;
    igb_macreg_writeops[RDT6] = igb_set_rdt;
    igb_macreg_writeops[RDT7] = igb_set_rdt;
    igb_macreg_writeops[RDT8] = igb_set_rdt;
    igb_macreg_writeops[RDT9] = igb_set_rdt;
    igb_macreg_writeops[RDT10] = igb_set_rdt;
    igb_macreg_writeops[RDT11] = igb_set_rdt;
    igb_macreg_writeops[RDT12] = igb_set_rdt;
    igb_macreg_writeops[RDT13] = igb_set_rdt;
    igb_macreg_writeops[RDT14] = igb_set_rdt;
    igb_macreg_writeops[RDT15] = igb_set_rdt;
    igb_macreg_writeops[IMC] = igb_set_imc;
    igb_macreg_writeops[IMS] = igb_set_ims;
    igb_macreg_writeops[ICR] = igb_set_icr;
    igb_macreg_writeops[EECD] = igb_set_eecd;
    igb_macreg_writeops[RCTL] = igb_set_rx_control;
    igb_macreg_writeops[CTRL] = igb_set_ctrl;
    igb_macreg_writeops[EERD] = igb_set_eerd;
    igb_macreg_writeops[TDFH] = igb_set_13bit;
    igb_macreg_writeops[TDFT] = igb_set_13bit;
    igb_macreg_writeops[TDFHS] = igb_set_13bit;
    igb_macreg_writeops[TDFTS] = igb_set_13bit;
    igb_macreg_writeops[TDFPC] = igb_set_13bit;
    igb_macreg_writeops[RDFH] = igb_set_13bit;
    igb_macreg_writeops[RDFT] = igb_set_13bit;
    igb_macreg_writeops[RDFHS] = igb_set_13bit;
    igb_macreg_writeops[RDFTS] = igb_set_13bit;
    igb_macreg_writeops[RDFPC] = igb_set_13bit;
    igb_macreg_writeops[GCR] = igb_set_gcr;
    igb_macreg_writeops[RXCSUM] = igb_set_rxcsum;
    igb_macreg_writeops[TDLEN0] = igb_set_dlen;
    igb_macreg_writeops[TDLEN1] = igb_set_dlen;
    igb_macreg_writeops[TDLEN2] = igb_set_dlen;
    igb_macreg_writeops[TDLEN3] = igb_set_dlen;
    igb_macreg_writeops[TDLEN4] = igb_set_dlen;
    igb_macreg_writeops[TDLEN5] = igb_set_dlen;
    igb_macreg_writeops[TDLEN6] = igb_set_dlen;
    igb_macreg_writeops[TDLEN7] = igb_set_dlen;
    igb_macreg_writeops[TDLEN8] = igb_set_dlen;
    igb_macreg_writeops[TDLEN9] = igb_set_dlen;
    igb_macreg_writeops[TDLEN10] = igb_set_dlen;
    igb_macreg_writeops[TDLEN11] = igb_set_dlen;
    igb_macreg_writeops[TDLEN12] = igb_set_dlen;
    igb_macreg_writeops[TDLEN13] = igb_set_dlen;
    igb_macreg_writeops[TDLEN14] = igb_set_dlen;
    igb_macreg_writeops[TDLEN15] = igb_set_dlen;
    igb_macreg_writeops[RDLEN0] = igb_set_dlen;
    igb_macreg_writeops[RDLEN1] = igb_set_dlen;
    igb_macreg_writeops[RDLEN2] = igb_set_dlen;
    igb_macreg_writeops[RDLEN3] = igb_set_dlen;
    igb_macreg_writeops[RDLEN4] = igb_set_dlen;
    igb_macreg_writeops[RDLEN5] = igb_set_dlen;
    igb_macreg_writeops[RDLEN6] = igb_set_dlen;
    igb_macreg_writeops[RDLEN7] = igb_set_dlen;
    igb_macreg_writeops[RDLEN8] = igb_set_dlen;
    igb_macreg_writeops[RDLEN9] = igb_set_dlen;
    igb_macreg_writeops[RDLEN10] = igb_set_dlen;
    igb_macreg_writeops[RDLEN11] = igb_set_dlen;
    igb_macreg_writeops[RDLEN12] = igb_set_dlen;
    igb_macreg_writeops[RDLEN13] = igb_set_dlen;
    igb_macreg_writeops[RDLEN14] = igb_set_dlen;
    igb_macreg_writeops[RDLEN15] = igb_set_dlen;
    igb_macreg_writeops[TDBAL0] = igb_set_dbal;
    igb_macreg_writeops[TDBAL1] = igb_set_dbal;
    igb_macreg_writeops[TDBAL2] = igb_set_dbal;
    igb_macreg_writeops[TDBAL3] = igb_set_dbal;
    igb_macreg_writeops[TDBAL4] = igb_set_dbal;
    igb_macreg_writeops[TDBAL5] = igb_set_dbal;
    igb_macreg_writeops[TDBAL6] = igb_set_dbal;
    igb_macreg_writeops[TDBAL7] = igb_set_dbal;
    igb_macreg_writeops[TDBAL8] = igb_set_dbal;
    igb_macreg_writeops[TDBAL9] = igb_set_dbal;
    igb_macreg_writeops[TDBAL10] = igb_set_dbal;
    igb_macreg_writeops[TDBAL11] = igb_set_dbal;
    igb_macreg_writeops[TDBAL12] = igb_set_dbal;
    igb_macreg_writeops[TDBAL13] = igb_set_dbal;
    igb_macreg_writeops[TDBAL14] = igb_set_dbal;
    igb_macreg_writeops[TDBAL15] = igb_set_dbal;
    igb_macreg_writeops[RDBAL0] = igb_set_dbal;
    igb_macreg_writeops[RDBAL1] = igb_set_dbal;
    igb_macreg_writeops[RDBAL2] = igb_set_dbal;
    igb_macreg_writeops[RDBAL3] = igb_set_dbal;
    igb_macreg_writeops[RDBAL4] = igb_set_dbal;
    igb_macreg_writeops[RDBAL5] = igb_set_dbal;
    igb_macreg_writeops[RDBAL6] = igb_set_dbal;
    igb_macreg_writeops[RDBAL7] = igb_set_dbal;
    igb_macreg_writeops[RDBAL8] = igb_set_dbal;
    igb_macreg_writeops[RDBAL9] = igb_set_dbal;
    igb_macreg_writeops[RDBAL10] = igb_set_dbal;
    igb_macreg_writeops[RDBAL11] = igb_set_dbal;
    igb_macreg_writeops[RDBAL12] = igb_set_dbal;
    igb_macreg_writeops[RDBAL13] = igb_set_dbal;
    igb_macreg_writeops[RDBAL14] = igb_set_dbal;
    igb_macreg_writeops[RDBAL15] = igb_set_dbal;
    igb_macreg_writeops[STATUS] = igb_set_status;
    igb_macreg_writeops[PBACLR] = igb_set_pbaclr;
    igb_macreg_writeops[CTRL_EXT] = igb_set_ctrlext;
    igb_macreg_writeops[FCAH] = igb_set_16bit;
    igb_macreg_writeops[FCT] = igb_set_16bit;
    igb_macreg_writeops[FCTTV] = igb_set_16bit;
    igb_macreg_writeops[FCRTV] = igb_set_16bit;
    igb_macreg_writeops[FCRTH] = igb_set_fcrth;
    igb_macreg_writeops[FCRTL] = igb_set_fcrtl;
    igb_macreg_writeops[CTRL_DUP] = igb_set_ctrl;
    igb_macreg_writeops[RFCTL] = igb_set_rfctl;
    igb_macreg_writeops[TIMINCA] = igb_set_timinca;
    igb_macreg_writeops[TIMADJH] = igb_set_timadjh;
    for (int i = IP6AT; i <= IP6AT + 3; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = IP4AT; i <= IP4AT + 6; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    igb_macreg_writeops[RA] = igb_mac_writereg;
    igb_macreg_writeops[RA + 1] = igb_mac_setmacaddr;
    for (int i = RA + 2; i <= RA + 31; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = RA2; i <= RA2 + 31; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = WUPM; i <= WUPM + 31; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = MTA; i <= MTA + E1000_MC_TBL_SIZE - 1; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = VFTA; i <= VFTA + E1000_VLAN_FILTER_TBL_SIZE - 1; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = FFMT; i <= FFMT + 254; i++)
        igb_macreg_writeops[i] = igb_set_4bit;
    for (int i = MDEF; i <= MDEF + 7; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = FTFT; i <= FTFT + 254; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = RETA; i <= RETA + 31; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = RSSRK; i <= RSSRK + 9; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = MAVTV0; i <= MAVTV3; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = EITR0; i <= EITR0 + IGB_INTR_NUM - 1; i++)
        igb_macreg_writeops[i] = igb_set_eitr;
    /* IGB specific: */
    igb_macreg_writeops[FWSM] = igb_mac_writereg;
    igb_macreg_writeops[SW_FW_SYNC] = igb_mac_writereg;
    igb_macreg_writeops[EICR] = igb_set_eicr;
    igb_macreg_writeops[EICS] = igb_set_eics;
    igb_macreg_writeops[EIAC] = igb_set_eiac;
    igb_macreg_writeops[EIAM] = igb_set_eiam;
    igb_macreg_writeops[EIMC] = igb_set_eimc;
    igb_macreg_writeops[EIMS] = igb_set_eims;
    for (int i = IVAR0; i <= IVAR0 + 7; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    igb_macreg_writeops[IVAR_MISC] = igb_mac_writereg;
    igb_macreg_writeops[TSYNCRXCFG] = igb_mac_writereg;
    for (int i = ETQF0; i <= ETQF0 + 7; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    igb_macreg_writeops[VT_CTL] = igb_mac_writereg;
    for (int i = P2VMAILBOX0; i <= P2VMAILBOX7; i++)
        igb_macreg_writeops[i] = igb_set_pfmailbox;
    for (int i = V2PMAILBOX0; i <= V2PMAILBOX7; i++)
        igb_macreg_writeops[i] = igb_set_vfmailbox;
    igb_macreg_writeops[MBVFICR] = igb_w1c;
    for (int i = VMBMEM0; i <= VMBMEM0 + 127; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    igb_macreg_writeops[MBVFIMR] = igb_mac_writereg;
    igb_macreg_writeops[VFLRE] = igb_w1c;
    igb_macreg_writeops[VFRE] = igb_mac_writereg;
    igb_macreg_writeops[VFTE] = igb_mac_writereg;
    igb_macreg_writeops[QDE] = igb_mac_writereg;
    igb_macreg_writeops[DTXSWC] = igb_mac_writereg;
    igb_macreg_writeops[RPLOLR] = igb_mac_writereg;
    for (int i = VLVF0; i <= VLVF0 + E1000_VLVF_ARRAY_SIZE - 1; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = VMVIR0; i <= VMVIR7; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = VMOLR0; i <= VMOLR7; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    for (int i = UTA; i <= UTA + E1000_MC_TBL_SIZE - 1; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
    igb_macreg_writeops[PVTCTRL0] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL1] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL2] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL3] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL4] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL5] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL6] = igb_set_vtctrl;
    igb_macreg_writeops[PVTCTRL7] = igb_set_vtctrl;
    igb_macreg_writeops[PVTEICS0] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS1] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS2] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS3] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS4] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS5] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS6] = igb_set_vteics;
    igb_macreg_writeops[PVTEICS7] = igb_set_vteics;
    igb_macreg_writeops[PVTEIMS0] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS1] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS2] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS3] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS4] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS5] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS6] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMS7] = igb_set_vteims;
    igb_macreg_writeops[PVTEIMC0] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC1] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC2] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC3] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC4] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC5] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC6] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIMC7] = igb_set_vteimc;
    igb_macreg_writeops[PVTEIAC0] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC1] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC2] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC3] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC4] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC5] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC6] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAC7] = igb_set_vteiac;
    igb_macreg_writeops[PVTEIAM0] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM1] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM2] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM3] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM4] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM5] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM6] = igb_set_vteiam;
    igb_macreg_writeops[PVTEIAM7] = igb_set_vteiam;
    igb_macreg_writeops[PVTEICR0] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR1] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR2] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR3] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR4] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR5] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR6] = igb_set_vteicr;
    igb_macreg_writeops[PVTEICR7] = igb_set_vteicr;
    for (int i = VTIVAR; i <= VTIVAR + 7; i++)
        igb_macreg_writeops[i] = igb_set_vtivar;
    for (int i = VTIVAR_MISC; i <= VTIVAR_MISC + 7; i++)
        igb_macreg_writeops[i] = igb_mac_writereg;
}



enum { MAC_ACCESS_PARTIAL = 1 };

/*
 * The array below combines alias offsets of the index values for the
 * MAC registers that have aliases, with the indication of not fully
 * implemented registers (lowest bit). This combination is possible
 * because all of the offsets are even.
 */
static uint16_t mac_reg_access[E1000E_MAC_SIZE];

static void __attribute__((constructor)) igb_init_mac_reg_access(void)
{
    /* Alias index offsets */
    mac_reg_access[FCRTL_A] = 0x07fe;
    mac_reg_access[RDFH_A] = 0xe904;
    mac_reg_access[RDFT_A] = 0xe904;
    mac_reg_access[TDFH_A] = 0xed00;
    mac_reg_access[TDFT_A] = 0xed00;
    for (int i = RA_A; i <= RA_A + 31; i++)
        mac_reg_access[i] = 0x14f0;
    for (int i = VFTA_A; i <= VFTA_A + E1000_VLAN_FILTER_TBL_SIZE - 1; i++)
        mac_reg_access[i] = 0x1400;
    mac_reg_access[RDBAL0_A] = 0x2600;
    mac_reg_access[RDBAH0_A] = 0x2600;
    mac_reg_access[RDLEN0_A] = 0x2600;
    mac_reg_access[SRRCTL0_A] = 0x2600;
    mac_reg_access[RDH0_A] = 0x2600;
    mac_reg_access[RDT0_A] = 0x2600;
    mac_reg_access[RXDCTL0_A] = 0x2600;
    mac_reg_access[RXCTL0_A] = 0x2600;
    mac_reg_access[RQDPC0_A] = 0x2600;
    mac_reg_access[RDBAL1_A] = 0x25D0;
    mac_reg_access[RDBAL2_A] = 0x25A0;
    mac_reg_access[RDBAL3_A] = 0x2570;
    mac_reg_access[RDBAH1_A] = 0x25D0;
    mac_reg_access[RDBAH2_A] = 0x25A0;
    mac_reg_access[RDBAH3_A] = 0x2570;
    mac_reg_access[RDLEN1_A] = 0x25D0;
    mac_reg_access[RDLEN2_A] = 0x25A0;
    mac_reg_access[RDLEN3_A] = 0x2570;
    mac_reg_access[SRRCTL1_A] = 0x25D0;
    mac_reg_access[SRRCTL2_A] = 0x25A0;
    mac_reg_access[SRRCTL3_A] = 0x2570;
    mac_reg_access[RDH1_A] = 0x25D0;
    mac_reg_access[RDH2_A] = 0x25A0;
    mac_reg_access[RDH3_A] = 0x2570;
    mac_reg_access[RDT1_A] = 0x25D0;
    mac_reg_access[RDT2_A] = 0x25A0;
    mac_reg_access[RDT3_A] = 0x2570;
    mac_reg_access[RXDCTL1_A] = 0x25D0;
    mac_reg_access[RXDCTL2_A] = 0x25A0;
    mac_reg_access[RXDCTL3_A] = 0x2570;
    mac_reg_access[RXCTL1_A] = 0x25D0;
    mac_reg_access[RXCTL2_A] = 0x25A0;
    mac_reg_access[RXCTL3_A] = 0x2570;
    mac_reg_access[RQDPC1_A] = 0x25D0;
    mac_reg_access[RQDPC2_A] = 0x25A0;
    mac_reg_access[RQDPC3_A] = 0x2570;
    mac_reg_access[TDBAL0_A] = 0x2A00;
    mac_reg_access[TDBAH0_A] = 0x2A00;
    mac_reg_access[TDLEN0_A] = 0x2A00;
    mac_reg_access[TDH0_A] = 0x2A00;
    mac_reg_access[TDT0_A] = 0x2A00;
    mac_reg_access[TXCTL0_A] = 0x2A00;
    mac_reg_access[TDWBAL0_A] = 0x2A00;
    mac_reg_access[TDWBAH0_A] = 0x2A00;
    mac_reg_access[TDBAL1_A] = 0x29D0;
    mac_reg_access[TDBAL2_A] = 0x29A0;
    mac_reg_access[TDBAL3_A] = 0x2970;
    mac_reg_access[TDBAH1_A] = 0x29D0;
    mac_reg_access[TDBAH2_A] = 0x29A0;
    mac_reg_access[TDBAH3_A] = 0x2970;
    mac_reg_access[TDLEN1_A] = 0x29D0;
    mac_reg_access[TDLEN2_A] = 0x29A0;
    mac_reg_access[TDLEN3_A] = 0x2970;
    mac_reg_access[TDH1_A] = 0x29D0;
    mac_reg_access[TDH2_A] = 0x29A0;
    mac_reg_access[TDH3_A] = 0x2970;
    mac_reg_access[TDT1_A] = 0x29D0;
    mac_reg_access[TDT2_A] = 0x29A0;
    mac_reg_access[TDT3_A] = 0x2970;
    mac_reg_access[TXDCTL0_A] = 0x2A00;
    mac_reg_access[TXDCTL1_A] = 0x29D0;
    mac_reg_access[TXDCTL2_A] = 0x29A0;
    mac_reg_access[TXDCTL3_A] = 0x2970;
    mac_reg_access[TXCTL1_A] = 0x29D0;
    mac_reg_access[TXCTL2_A] = 0x29A0;
    mac_reg_access[TXCTL3_A] = 0x29D0;
    mac_reg_access[TDWBAL1_A] = 0x29D0;
    mac_reg_access[TDWBAL2_A] = 0x29A0;
    mac_reg_access[TDWBAL3_A] = 0x2970;
    mac_reg_access[TDWBAH1_A] = 0x29D0;
    mac_reg_access[TDWBAH2_A] = 0x29A0;
    mac_reg_access[TDWBAH3_A] = 0x2970;
    /* Access options */
    mac_reg_access[RDFH] = MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFT] = MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFHS] = MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFTS] = MAC_ACCESS_PARTIAL;
    mac_reg_access[RDFPC] = MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFH] = MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFT] = MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFHS] = MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFTS] = MAC_ACCESS_PARTIAL;
    mac_reg_access[TDFPC] = MAC_ACCESS_PARTIAL;
    mac_reg_access[EECD] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FLA] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCAL] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCAH] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCT] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCTTV] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCRTV] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCRTL] = MAC_ACCESS_PARTIAL;
    mac_reg_access[FCRTH] = MAC_ACCESS_PARTIAL;
    for (int i = MAVTV0; i <= MAVTV3; i++)
        mac_reg_access[i] = MAC_ACCESS_PARTIAL;
}



void
igb_core_write(IGBCore *core, hwaddr addr, uint64_t val, unsigned size)
{
    uint16_t index = igb_get_reg_index_with_offset(mac_reg_access, addr);

    if (index < IGB_NWRITEOPS && igb_macreg_writeops[index]) {
        if (mac_reg_access[index] & MAC_ACCESS_PARTIAL) {
            trace_e1000e_wrn_regs_write_trivial(index << 2);
        }
        trace_e1000e_core_write(index << 2, size, val);
        igb_macreg_writeops[index](core, index, val);
    } else if (index < IGB_NREADOPS && igb_macreg_readops[index]) {
        trace_e1000e_wrn_regs_write_ro(index << 2, size, val);
    } else {
        trace_e1000e_wrn_regs_write_unknown(index << 2, size, val);
    }
}

uint64_t
igb_core_read(IGBCore *core, hwaddr addr, unsigned size)
{
    uint64_t val;
    uint16_t index = igb_get_reg_index_with_offset(mac_reg_access, addr);

    if (index < IGB_NREADOPS && igb_macreg_readops[index]) {
        if (mac_reg_access[index] & MAC_ACCESS_PARTIAL) {
            trace_e1000e_wrn_regs_read_trivial(index << 2);
        }
        val = igb_macreg_readops[index](core, index);
        trace_e1000e_core_read(index << 2, size, val);
        return val;
    } else {
        trace_e1000e_wrn_regs_read_unknown(index << 2, size);
    }
    return 0;
}

static void
igb_autoneg_resume(IGBCore *core)
{
    if (igb_have_autoneg(core) &&
        !(core->phy[MII_BMSR] & MII_BMSR_AN_COMP)) {
        qemu_get_queue(core->owner_nic)->link_down = false;
        timer_mod(core->autoneg_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);
    }
}

void
igb_core_pci_realize(IGBCore        *core,
                     const uint16_t *eeprom_templ,
                     uint32_t        eeprom_size,
                     const uint8_t  *macaddr)
{
    int i;

    core->autoneg_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       igb_autoneg_timer, core);
    igb_intrmgr_pci_realize(core);

    for (i = 0; i < IGB_NUM_QUEUES; i++) {
        net_tx_pkt_init(&core->tx[i].tx_pkt, E1000E_MAX_TX_FRAGS);
    }

    net_rx_pkt_init(&core->rx_pkt);

    e1000x_core_prepare_eeprom(core->eeprom,
                               eeprom_templ,
                               eeprom_size,
                               PCI_DEVICE_GET_CLASS(core->owner)->device_id,
                               macaddr);
    igb_update_rx_offloads(core);
}

void
igb_core_pci_uninit(IGBCore *core)
{
    int i;

    timer_free(core->autoneg_timer);

    igb_intrmgr_pci_unint(core);

    for (i = 0; i < IGB_NUM_QUEUES; i++) {
        net_tx_pkt_uninit(core->tx[i].tx_pkt);
    }

    net_rx_pkt_uninit(core->rx_pkt);
}

static uint16_t igb_phy_reg_init[MAX_PHY_REG_ADDRESS + 1];

static void __attribute__((constructor)) igb_init_phy_reg_init(void)
{
    igb_phy_reg_init[MII_BMCR] = MII_BMCR_SPEED1000 |
                 MII_BMCR_FD        |
                 MII_BMCR_AUTOEN;
    igb_phy_reg_init[MII_BMSR] = MII_BMSR_EXTCAP    |
                 MII_BMSR_LINK_ST   |
                 MII_BMSR_AUTONEG   |
                 MII_BMSR_MFPS      |
                 MII_BMSR_EXTSTAT   |
                 MII_BMSR_10T_HD    |
                 MII_BMSR_10T_FD    |
                 MII_BMSR_100TX_HD  |
                 MII_BMSR_100TX_FD;
    igb_phy_reg_init[MII_PHYID1] = IGP03E1000_E_PHY_ID >> 16;
    igb_phy_reg_init[MII_PHYID2] = (IGP03E1000_E_PHY_ID & 0xfff0) | 1;
    igb_phy_reg_init[MII_ANAR] = MII_ANAR_CSMACD | MII_ANAR_10 |
                              MII_ANAR_10FD | MII_ANAR_TX |
                              MII_ANAR_TXFD | MII_ANAR_PAUSE |
                              MII_ANAR_PAUSE_ASYM;
    igb_phy_reg_init[MII_ANLPAR] = MII_ANLPAR_10 | MII_ANLPAR_10FD |
                              MII_ANLPAR_TX | MII_ANLPAR_TXFD |
                              MII_ANLPAR_T4 | MII_ANLPAR_PAUSE;
    igb_phy_reg_init[MII_ANER] = MII_ANER_NP | MII_ANER_NWAY;
    igb_phy_reg_init[MII_ANNP] = 0x1 | MII_ANNP_MP;
    igb_phy_reg_init[MII_CTRL1000] = MII_CTRL1000_HALF | MII_CTRL1000_FULL |
                              MII_CTRL1000_PORT | MII_CTRL1000_MASTER;
    igb_phy_reg_init[MII_STAT1000] = MII_STAT1000_HALF | MII_STAT1000_FULL |
                              MII_STAT1000_ROK | MII_STAT1000_LOK;
    igb_phy_reg_init[MII_EXTSTAT] = MII_EXTSTAT_1000T_HD | MII_EXTSTAT_1000T_FD;
    igb_phy_reg_init[IGP01E1000_PHY_PORT_CONFIG] = BIT(5) | BIT(8);
    igb_phy_reg_init[IGP01E1000_PHY_PORT_STATUS] = IGP01E1000_PSSR_SPEED_1000MBPS;
    igb_phy_reg_init[IGP02E1000_PHY_POWER_MGMT] = BIT(0) | BIT(3) | IGP02E1000_PM_D3_LPLU |
                                   IGP01E1000_PSCFR_SMART_SPEED;
}


static uint32_t igb_mac_reg_init[E1000E_MAC_SIZE];

static void __attribute__((constructor)) igb_init_mac_reg_init(void)
{
    igb_mac_reg_init[LEDCTL] = 2 | (3 << 8) | BIT(15) | (6 << 16) | (7 << 24);
    igb_mac_reg_init[EEMNGCTL] = BIT(31);
    igb_mac_reg_init[TXDCTL0] = E1000_TXDCTL_QUEUE_ENABLE;
    igb_mac_reg_init[RXDCTL0] = E1000_RXDCTL_QUEUE_ENABLE | (1 << 16);
    igb_mac_reg_init[RXDCTL1] = 1 << 16;
    igb_mac_reg_init[RXDCTL2] = 1 << 16;
    igb_mac_reg_init[RXDCTL3] = 1 << 16;
    igb_mac_reg_init[RXDCTL4] = 1 << 16;
    igb_mac_reg_init[RXDCTL5] = 1 << 16;
    igb_mac_reg_init[RXDCTL6] = 1 << 16;
    igb_mac_reg_init[RXDCTL7] = 1 << 16;
    igb_mac_reg_init[RXDCTL8] = 1 << 16;
    igb_mac_reg_init[RXDCTL9] = 1 << 16;
    igb_mac_reg_init[RXDCTL10] = 1 << 16;
    igb_mac_reg_init[RXDCTL11] = 1 << 16;
    igb_mac_reg_init[RXDCTL12] = 1 << 16;
    igb_mac_reg_init[RXDCTL13] = 1 << 16;
    igb_mac_reg_init[RXDCTL14] = 1 << 16;
    igb_mac_reg_init[RXDCTL15] = 1 << 16;
    igb_mac_reg_init[TIPG] = 0x08 | (0x04 << 10) | (0x06 << 20);
    igb_mac_reg_init[CTRL] = E1000_CTRL_FD | E1000_CTRL_LRST | E1000_CTRL_SPD_1000 |
                      E1000_CTRL_ADVD3WUC;
    igb_mac_reg_init[STATUS] = E1000_STATUS_PHYRA | BIT(31);
    igb_mac_reg_init[EECD] = E1000_EECD_FWE_DIS | E1000_EECD_PRES |
                      (2 << E1000_EECD_SIZE_EX_SHIFT);
    igb_mac_reg_init[GCR] = E1000_L0S_ADJUST |
                      E1000_GCR_CMPL_TMOUT_RESEND |
                      E1000_GCR_CAP_VER2 |
                      E1000_L1_ENTRY_LATENCY_MSB |
                      E1000_L1_ENTRY_LATENCY_LSB;
    igb_mac_reg_init[RXCSUM] = E1000_RXCSUM_IPOFLD | E1000_RXCSUM_TUOFLD;
    igb_mac_reg_init[TXPBS] = 0x28;
    igb_mac_reg_init[RXPBS] = 0x40;
    igb_mac_reg_init[TCTL] = E1000_TCTL_PSP | (0xF << E1000_CT_SHIFT) |
                      (0x40 << E1000_COLD_SHIFT) | (0x1 << 26) | (0xA << 28);
    igb_mac_reg_init[TCTL_EXT] = 0x40 | (0x42 << 10);
    igb_mac_reg_init[DTXCTL] = E1000_DTXCTL_8023LL | E1000_DTXCTL_SPOOF_INT;
    igb_mac_reg_init[VET] = ETH_P_VLAN | (ETH_P_VLAN << 16);
    for (int i = V2PMAILBOX0; i <= V2PMAILBOX0 + IGB_MAX_VF_FUNCTIONS - 1; i++)
        igb_mac_reg_init[i] = E1000_V2PMAILBOX_RSTI;
    igb_mac_reg_init[MBVFIMR] = 0xFF;
    igb_mac_reg_init[VFRE] = 0xFF;
    igb_mac_reg_init[VFTE] = 0xFF;
    for (int i = VMOLR0; i <= VMOLR0 + 7; i++)
        igb_mac_reg_init[i] = 0x2600 | E1000_VMOLR_STRCRC;
    igb_mac_reg_init[RPLOLR] = E1000_RPLOLR_STRCRC;
    igb_mac_reg_init[RLPML] = 0x2600;
    igb_mac_reg_init[TXCTL0] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL1] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL2] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL3] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL4] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL5] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL6] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL7] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL8] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL9] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL10] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL11] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL12] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL13] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL14] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
    igb_mac_reg_init[TXCTL15] = E1000_DCA_TXCTRL_DATA_RRO_EN |
                      E1000_DCA_TXCTRL_TX_WB_RO_EN |
                      E1000_DCA_TXCTRL_DESC_RRO_EN;
}


static void igb_reset(IGBCore *core, bool sw)
{
    igb_tx *tx;
    int i;

    timer_del(core->autoneg_timer);

    igb_intrmgr_reset(core);

    memset(core->phy, 0, sizeof core->phy);
    memcpy(core->phy, igb_phy_reg_init, sizeof igb_phy_reg_init);

    for (i = 0; i < E1000E_MAC_SIZE; i++) {
        if (sw &&
            (i == RXPBS || i == TXPBS ||
             (i >= EITR0 && i < EITR0 + IGB_INTR_NUM))) {
            continue;
        }

        core->mac[i] = i < ARRAY_SIZE(igb_mac_reg_init) ?
                       igb_mac_reg_init[i] : 0;
    }

    if (qemu_get_queue(core->owner_nic)->link_down) {
        igb_link_down(core);
    }

    e1000x_reset_mac_addr(core->owner_nic, core->mac, core->permanent_mac);

    for (int vfn = 0; vfn < IGB_MAX_VF_FUNCTIONS; vfn++) {
        /* Set RSTI, so VF can identify a PF reset is in progress */
        core->mac[V2PMAILBOX0 + vfn] |= E1000_V2PMAILBOX_RSTI;
    }

    for (i = 0; i < ARRAY_SIZE(core->tx); i++) {
        tx = &core->tx[i];
        memset(tx->ctx, 0, sizeof(tx->ctx));
        tx->first = true;
        tx->skip_cp = false;
    }
}

void
igb_core_reset(IGBCore *core)
{
    igb_reset(core, false);
}

void igb_core_pre_save(IGBCore *core)
{
    int i;
    NetClientState *nc = qemu_get_queue(core->owner_nic);

    /*
     * If link is down and auto-negotiation is supported and ongoing,
     * complete auto-negotiation immediately. This allows us to look
     * at MII_BMSR_AN_COMP to infer link status on load.
     */
    if (nc->link_down && igb_have_autoneg(core)) {
        core->phy[MII_BMSR] |= MII_BMSR_AN_COMP;
        igb_update_flowctl_status(core);
    }

    for (i = 0; i < ARRAY_SIZE(core->tx); i++) {
        if (net_tx_pkt_has_fragments(core->tx[i].tx_pkt)) {
            core->tx[i].skip_cp = true;
        }
    }
}

int
igb_core_post_load(IGBCore *core)
{
    NetClientState *nc = qemu_get_queue(core->owner_nic);

    /*
     * nc.link_down can't be migrated, so infer link_down according
     * to link status bit in core.mac[STATUS].
     */
    nc->link_down = (core->mac[STATUS] & E1000_STATUS_LU) == 0;

    /*
     * we need to restart intrmgr timers, as an older version of
     * QEMU can have stopped them before migration
     */
    igb_intrmgr_resume(core);
    igb_autoneg_resume(core);

    return 0;
}
