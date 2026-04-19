/*
 * RISC-V APLIC (Advanced Platform Level Interrupt Controller)
 *
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "system/address-spaces.h"
#include "hw/sysbus.h"
#include "hw/pci/msi.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/irq.h"
#include "target/riscv/cpu.h"
#include "system/system.h"
#include "system/kvm.h"
#include "system/tcg.h"
#include "kvm/kvm_riscv.h"
#include "migration/vmstate.h"

#define APLIC_MAX_IDC                  (1UL << 14)
#define APLIC_MAX_SOURCE               1024
#define APLIC_MIN_IPRIO_BITS           1
#define APLIC_MAX_IPRIO_BITS           8
#define APLIC_MAX_CHILDREN             1024

#define APLIC_DOMAINCFG                0x0000
#define APLIC_DOMAINCFG_RDONLY         0x80000000
#define APLIC_DOMAINCFG_IE             (1 << 8)
#define APLIC_DOMAINCFG_DM             (1 << 2)
#define APLIC_DOMAINCFG_BE             (1 << 0)

#define APLIC_SOURCECFG_BASE           0x0004
#define APLIC_SOURCECFG_D              (1 << 10)
#define APLIC_SOURCECFG_CHILDIDX_MASK  0x000003ff
#define APLIC_SOURCECFG_SM_MASK        0x00000007
#define APLIC_SOURCECFG_SM_INACTIVE    0x0
#define APLIC_SOURCECFG_SM_DETACH      0x1
#define APLIC_SOURCECFG_SM_EDGE_RISE   0x4
#define APLIC_SOURCECFG_SM_EDGE_FALL   0x5
#define APLIC_SOURCECFG_SM_LEVEL_HIGH  0x6
#define APLIC_SOURCECFG_SM_LEVEL_LOW   0x7

#define APLIC_MMSICFGADDR              0x1bc0
#define APLIC_MMSICFGADDRH             0x1bc4
#define APLIC_SMSICFGADDR              0x1bc8
#define APLIC_SMSICFGADDRH             0x1bcc

#define APLIC_xMSICFGADDRH_L           (1UL << 31)
#define APLIC_xMSICFGADDRH_HHXS_MASK   0x1f
#define APLIC_xMSICFGADDRH_HHXS_SHIFT  24
#define APLIC_xMSICFGADDRH_LHXS_MASK   0x7
#define APLIC_xMSICFGADDRH_LHXS_SHIFT  20
#define APLIC_xMSICFGADDRH_HHXW_MASK   0x7
#define APLIC_xMSICFGADDRH_HHXW_SHIFT  16
#define APLIC_xMSICFGADDRH_LHXW_MASK   0xf
#define APLIC_xMSICFGADDRH_LHXW_SHIFT  12
#define APLIC_xMSICFGADDRH_BAPPN_MASK  0xfff

#define APLIC_xMSICFGADDR_PPN_SHIFT    12

#define APLIC_xMSICFGADDR_PPN_HART(__lhxs) \
    ((1UL << (__lhxs)) - 1)

#define APLIC_xMSICFGADDR_PPN_LHX_MASK(__lhxw) \
    ((1UL << (__lhxw)) - 1)
#define APLIC_xMSICFGADDR_PPN_LHX_SHIFT(__lhxs) \
    ((__lhxs))
#define APLIC_xMSICFGADDR_PPN_LHX(__lhxw, __lhxs) \
    (APLIC_xMSICFGADDR_PPN_LHX_MASK(__lhxw) << \
     APLIC_xMSICFGADDR_PPN_LHX_SHIFT(__lhxs))

#define APLIC_xMSICFGADDR_PPN_HHX_MASK(__hhxw) \
    ((1UL << (__hhxw)) - 1)
#define APLIC_xMSICFGADDR_PPN_HHX_SHIFT(__hhxs) \
    ((__hhxs) + APLIC_xMSICFGADDR_PPN_SHIFT)
#define APLIC_xMSICFGADDR_PPN_HHX(__hhxw, __hhxs) \
    (APLIC_xMSICFGADDR_PPN_HHX_MASK(__hhxw) << \
     APLIC_xMSICFGADDR_PPN_HHX_SHIFT(__hhxs))

#define APLIC_MMSICFGADDRH_VALID_MASK   \
    (APLIC_xMSICFGADDRH_L | \
     (APLIC_xMSICFGADDRH_HHXS_MASK << APLIC_xMSICFGADDRH_HHXS_SHIFT) | \
     (APLIC_xMSICFGADDRH_LHXS_MASK << APLIC_xMSICFGADDRH_LHXS_SHIFT) | \
     (APLIC_xMSICFGADDRH_HHXW_MASK << APLIC_xMSICFGADDRH_HHXW_SHIFT) | \
     (APLIC_xMSICFGADDRH_LHXW_MASK << APLIC_xMSICFGADDRH_LHXW_SHIFT) | \
     APLIC_xMSICFGADDRH_BAPPN_MASK)

#define APLIC_SMSICFGADDRH_VALID_MASK   \
    ((APLIC_xMSICFGADDRH_LHXS_MASK << APLIC_xMSICFGADDRH_LHXS_SHIFT) | \
     APLIC_xMSICFGADDRH_BAPPN_MASK)

#define APLIC_SETIP_BASE               0x1c00
#define APLIC_SETIPNUM                 0x1cdc

#define APLIC_CLRIP_BASE               0x1d00
#define APLIC_CLRIPNUM                 0x1ddc

#define APLIC_SETIE_BASE               0x1e00
#define APLIC_SETIENUM                 0x1edc

#define APLIC_CLRIE_BASE               0x1f00
#define APLIC_CLRIENUM                 0x1fdc

#define APLIC_SETIPNUM_LE              0x2000
#define APLIC_SETIPNUM_BE              0x2004

#define APLIC_ISTATE_PENDING           (1U << 0)
#define APLIC_ISTATE_ENABLED           (1U << 1)
#define APLIC_ISTATE_ENPEND            (APLIC_ISTATE_ENABLED | \
                                        APLIC_ISTATE_PENDING)
#define APLIC_ISTATE_INPUT             (1U << 8)

#define APLIC_GENMSI                   0x3000

#define APLIC_TARGET_BASE              0x3004
#define APLIC_TARGET_HART_IDX_SHIFT    18
#define APLIC_TARGET_HART_IDX_MASK     0x3fff
#define APLIC_TARGET_GUEST_IDX_SHIFT   12
#define APLIC_TARGET_GUEST_IDX_MASK    0x3f
#define APLIC_TARGET_IPRIO_MASK        0xff
#define APLIC_TARGET_EIID_MASK         0x7ff

#define APLIC_IDC_BASE                 0x4000
#define APLIC_IDC_SIZE                 32

#define APLIC_IDC_IDELIVERY            0x00

#define APLIC_IDC_IFORCE               0x04

#define APLIC_IDC_ITHRESHOLD           0x08

#define APLIC_IDC_TOPI                 0x18
#define APLIC_IDC_TOPI_ID_SHIFT        16
#define APLIC_IDC_TOPI_ID_MASK         0x3ff
#define APLIC_IDC_TOPI_PRIO_MASK       0xff

#define APLIC_IDC_CLAIMI               0x1c

static inline RISCVAPLICState *riscv_aplic_from_obj(void *obj)
{
    return reinterpret_cast<RISCVAPLICState *>(reinterpret_cast<RISCVAPLICState *>(obj));
}

/*
 * KVM AIA only supports APLIC MSI, fallback to QEMU emulation if we want to use
 * APLIC Wired.
 */
bool riscv_is_kvm_aia_aplic_imsic(bool msimode)
{
    return kvm_irqchip_in_kernel() && msimode;
}

bool riscv_use_emulated_aplic(bool msimode)
{
#ifdef CONFIG_KVM
    if (tcg_enabled()) {
        return true;
    }

    if (!riscv_is_kvm_aia_aplic_imsic(msimode)) {
        return true;
    }

    return kvm_kernel_irqchip_split();
#else
    return true;
#endif
}

void riscv_aplic_set_kvm_msicfgaddr(RISCVAPLICState *aplic, hwaddr addr)
{
#ifdef CONFIG_KVM
    if (riscv_use_emulated_aplic(aplic->msimode)) {
        addr >>= APLIC_xMSICFGADDR_PPN_SHIFT;
        aplic->kvm_msicfgaddr = extract64(addr, 0, 32);
        aplic->kvm_msicfgaddrH = extract64(addr, 32, 32) &
                                 APLIC_MMSICFGADDRH_VALID_MASK;
    }
#endif
}

bool RISCVAPLICState::irqRectifiedVal(uint32_t irq)
{
    RISCVAPLICState *aplic = this;
    uint32_t sourcecfg, sm, raw_input, irq_inverted;

    if (!irq || aplic->num_irqs <= irq) {
        return false;
    }

    sourcecfg = aplic->sourcecfg[irq];
    if (sourcecfg & APLIC_SOURCECFG_D) {
        return false;
    }

    sm = sourcecfg & APLIC_SOURCECFG_SM_MASK;
    if (sm == APLIC_SOURCECFG_SM_INACTIVE) {
        return false;
    }

    raw_input = (aplic->state[irq] & APLIC_ISTATE_INPUT) ? 1 : 0;
    irq_inverted = (sm == APLIC_SOURCECFG_SM_LEVEL_LOW ||
                    sm == APLIC_SOURCECFG_SM_EDGE_FALL) ? 1 : 0;

    return !!(raw_input ^ irq_inverted);
}

uint32_t RISCVAPLICState::readInputWord(uint32_t word)
{
    uint32_t i, irq, rectified_val, ret = 0;

    for (i = 0; i < 32; i++) {
        irq = word * 32 + i;

        rectified_val = irqRectifiedVal(irq);
        ret |= rectified_val << i;
    }

    return ret;
}

uint32_t RISCVAPLICState::readPendingWord(uint32_t word)
{
    RISCVAPLICState *aplic = this;
    uint32_t i, irq, ret = 0;

    for (i = 0; i < 32; i++) {
        irq = word * 32 + i;
        if (!irq || aplic->num_irqs <= irq) {
            continue;
        }

        ret |= ((aplic->state[irq] & APLIC_ISTATE_PENDING) ? 1 : 0) << i;
    }

    return ret;
}

void RISCVAPLICState::setPendingRaw(uint32_t irq, bool pending)
{
    RISCVAPLICState *aplic = this;
    if (pending) {
        aplic->state[irq] |= APLIC_ISTATE_PENDING;
    } else {
        aplic->state[irq] &= ~APLIC_ISTATE_PENDING;
    }
}

void RISCVAPLICState::setPending(uint32_t irq, bool pending)
{
    RISCVAPLICState *aplic = this;
    uint32_t sourcecfg, sm;

    if ((irq <= 0) || (aplic->num_irqs <= irq)) {
        return;
    }

    sourcecfg = aplic->sourcecfg[irq];
    if (sourcecfg & APLIC_SOURCECFG_D) {
        return;
    }

    sm = sourcecfg & APLIC_SOURCECFG_SM_MASK;
    if (sm == APLIC_SOURCECFG_SM_INACTIVE) {
        return;
    }

    if ((sm == APLIC_SOURCECFG_SM_LEVEL_HIGH) ||
        (sm == APLIC_SOURCECFG_SM_LEVEL_LOW)) {
        if (!aplic->msimode) {
            return;
        }
        if (aplic->msimode && !pending) {
            goto noskip_write_pending;
        }
        if ((aplic->state[irq] & APLIC_ISTATE_INPUT) &&
            (sm == APLIC_SOURCECFG_SM_LEVEL_LOW)) {
            return;
        }
        if (!(aplic->state[irq] & APLIC_ISTATE_INPUT) &&
            (sm == APLIC_SOURCECFG_SM_LEVEL_HIGH)) {
            return;
        }
    }

noskip_write_pending:
    setPendingRaw(irq, pending);
}

void RISCVAPLICState::setPendingWord(uint32_t word, uint32_t value,
                                     bool pending)
{
    RISCVAPLICState *aplic = this;
    uint32_t i, irq;

    for (i = 0; i < 32; i++) {
        irq = word * 32 + i;
        if (!irq || aplic->num_irqs <= irq) {
            continue;
        }

        if (value & (1U << i)) {
            setPending(irq, pending);
        }
    }
}

uint32_t RISCVAPLICState::readEnabledWord(int word)
{
    RISCVAPLICState *aplic = this;
    uint32_t i, irq, ret = 0;

    for (i = 0; i < 32; i++) {
        irq = word * 32 + i;
        if (!irq || aplic->num_irqs <= irq) {
            continue;
        }

        ret |= ((aplic->state[irq] & APLIC_ISTATE_ENABLED) ? 1 : 0) << i;
    }

    return ret;
}

void RISCVAPLICState::setEnabledRaw(uint32_t irq, bool enabled)
{
    RISCVAPLICState *aplic = this;
    if (enabled) {
        aplic->state[irq] |= APLIC_ISTATE_ENABLED;
    } else {
        aplic->state[irq] &= ~APLIC_ISTATE_ENABLED;
    }
}

void RISCVAPLICState::setEnabled(uint32_t irq, bool enabled)
{
    RISCVAPLICState *aplic = this;
    uint32_t sourcecfg, sm;

    if ((irq <= 0) || (aplic->num_irqs <= irq)) {
        return;
    }

    sourcecfg = aplic->sourcecfg[irq];
    if (sourcecfg & APLIC_SOURCECFG_D) {
        return;
    }

    sm = sourcecfg & APLIC_SOURCECFG_SM_MASK;
    if (sm == APLIC_SOURCECFG_SM_INACTIVE) {
        return;
    }

    setEnabledRaw(irq, enabled);
}

void RISCVAPLICState::setEnabledWord(uint32_t word, uint32_t value,
                                     bool enabled)
{
    RISCVAPLICState *aplic = this;
    uint32_t i, irq;

    for (i = 0; i < 32; i++) {
        irq = word * 32 + i;
        if (!irq || aplic->num_irqs <= irq) {
            continue;
        }

        if (value & (1U << i)) {
            setEnabled(irq, enabled);
        }
    }
}

void RISCVAPLICState::msiSend(uint32_t hart_idx, uint32_t guest_idx,
                              uint32_t eiid)
{
    RISCVAPLICState *aplic = this;
    uint64_t addr;
    MemTxResult result;
    RISCVAPLICState *aplic_m;
    uint32_t lhxs, lhxw, hhxs, hhxw, group_idx, msicfgaddr, msicfgaddrH;

    aplic_m = aplic;

    if (!aplic->kvm_splitmode) {
        while (aplic_m && !aplic_m->mmode) {
            aplic_m = aplic_m->parent;
        }
        if (!aplic_m) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: m-level APLIC not found\n",
                          __func__);
            return;
        }
    }

    if (aplic->kvm_splitmode) {
        msicfgaddr = aplic->kvm_msicfgaddr;
        msicfgaddrH = ((uint64_t)aplic->kvm_msicfgaddrH << 32);
    } else {
        msicfgaddr = aplic_m->mmsicfgaddr;
        msicfgaddrH = aplic_m->mmsicfgaddrH;
    }

    lhxs = (msicfgaddrH >> APLIC_xMSICFGADDRH_LHXS_SHIFT) &
            APLIC_xMSICFGADDRH_LHXS_MASK;
    lhxw = (msicfgaddrH >> APLIC_xMSICFGADDRH_LHXW_SHIFT) &
            APLIC_xMSICFGADDRH_LHXW_MASK;
    hhxs = (msicfgaddrH >> APLIC_xMSICFGADDRH_HHXS_SHIFT) &
            APLIC_xMSICFGADDRH_HHXS_MASK;
    hhxw = (msicfgaddrH >> APLIC_xMSICFGADDRH_HHXW_SHIFT) &
            APLIC_xMSICFGADDRH_HHXW_MASK;

    if (!aplic->kvm_splitmode && !aplic->mmode) {
        msicfgaddrH = aplic_m->smsicfgaddrH;
        msicfgaddr = aplic_m->smsicfgaddr;

        lhxs = (msicfgaddrH >> APLIC_xMSICFGADDRH_LHXS_SHIFT) &
            APLIC_xMSICFGADDRH_LHXS_MASK;
    }

    group_idx = hart_idx >> lhxw;

    addr = msicfgaddr;
    addr |= ((uint64_t)(msicfgaddrH & APLIC_xMSICFGADDRH_BAPPN_MASK)) << 32;
    addr |= ((uint64_t)(group_idx & APLIC_xMSICFGADDR_PPN_HHX_MASK(hhxw))) <<
             APLIC_xMSICFGADDR_PPN_HHX_SHIFT(hhxs);
    addr |= ((uint64_t)(hart_idx & APLIC_xMSICFGADDR_PPN_LHX_MASK(lhxw))) <<
             APLIC_xMSICFGADDR_PPN_LHX_SHIFT(lhxs);
    addr |= (uint64_t)(guest_idx & APLIC_xMSICFGADDR_PPN_HART(lhxs));
    addr <<= APLIC_xMSICFGADDR_PPN_SHIFT;

    address_space_stl_le(&address_space_memory, addr,
                         eiid, MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: MSI write failed for "
                      "hart_index=%d guest_index=%d eiid=%d\n",
                      __func__, hart_idx, guest_idx, eiid);
    }
}

void RISCVAPLICState::msiIrqUpdate(uint32_t irq)
{
    RISCVAPLICState *aplic = this;
    uint32_t hart_idx, guest_idx, eiid;

    if (!aplic->msimode || (aplic->num_irqs <= irq) ||
        !(aplic->domaincfg & APLIC_DOMAINCFG_IE)) {
        return;
    }

    if ((aplic->state[irq] & APLIC_ISTATE_ENPEND) != APLIC_ISTATE_ENPEND) {
        return;
    }

    setPendingRaw(irq, false);

    hart_idx = aplic->target[irq] >> APLIC_TARGET_HART_IDX_SHIFT;
    hart_idx &= APLIC_TARGET_HART_IDX_MASK;
    if (aplic->mmode) {
        /* M-level APLIC ignores guest_index */
        guest_idx = 0;
    } else {
        guest_idx = aplic->target[irq] >> APLIC_TARGET_GUEST_IDX_SHIFT;
        guest_idx &= APLIC_TARGET_GUEST_IDX_MASK;
    }
    eiid = aplic->target[irq] & APLIC_TARGET_EIID_MASK;
    msiSend(hart_idx, guest_idx, eiid);
}

uint32_t RISCVAPLICState::idcTopi(uint32_t idc)
{
    RISCVAPLICState *aplic = this;
    uint32_t best_irq, best_iprio;
    uint32_t irq, iprio, ihartidx, ithres;

    if (aplic->num_harts <= idc) {
        return 0;
    }

    ithres = aplic->ithreshold[idc];
    best_irq = best_iprio = UINT32_MAX;
    for (irq = 1; irq < aplic->num_irqs; irq++) {
        if ((aplic->state[irq] & APLIC_ISTATE_ENPEND) !=
            APLIC_ISTATE_ENPEND) {
            continue;
        }

        ihartidx = aplic->target[irq] >> APLIC_TARGET_HART_IDX_SHIFT;
        ihartidx &= APLIC_TARGET_HART_IDX_MASK;
        if (ihartidx != idc) {
            continue;
        }

        iprio = aplic->target[irq] & aplic->iprio_mask;
        if (ithres && iprio >= ithres) {
            continue;
        }

        if (iprio < best_iprio) {
            best_irq = irq;
            best_iprio = iprio;
        }
    }

    if (best_irq < aplic->num_irqs && best_iprio <= aplic->iprio_mask) {
        return (best_irq << APLIC_IDC_TOPI_ID_SHIFT) | best_iprio;
    }

    return 0;
}

void RISCVAPLICState::idcUpdate(uint32_t idc)
{
    RISCVAPLICState *aplic = this;
    uint32_t topi;

    if (aplic->msimode || aplic->num_harts <= idc) {
        return;
    }

    topi = idcTopi(idc);
    if ((aplic->domaincfg & APLIC_DOMAINCFG_IE) &&
        aplic->idelivery[idc] &&
        (aplic->iforce[idc] || topi)) {
        qemu_irq_raise(aplic->external_irqs[idc]);
    } else {
        qemu_irq_lower(aplic->external_irqs[idc]);
    }
}

uint32_t RISCVAPLICState::idcClaimi(uint32_t idc)
{
    RISCVAPLICState *aplic = this;
    uint32_t irq, sm, topi = idcTopi(idc);

    if (!topi) {
        aplic->iforce[idc] = 0;
        idcUpdate(idc);
        return 0;
    }

    irq = (topi >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;
    sm = aplic->sourcecfg[irq] & APLIC_SOURCECFG_SM_MASK;
    uint32_t irq_state = aplic->state[irq];
    setPendingRaw(irq, false);
    if ((sm == APLIC_SOURCECFG_SM_LEVEL_HIGH) &&
        (irq_state & APLIC_ISTATE_INPUT)) {
        setPendingRaw(irq, true);
    } else if ((sm == APLIC_SOURCECFG_SM_LEVEL_LOW) &&
               !(irq_state & APLIC_ISTATE_INPUT)) {
        setPendingRaw(irq, true);
    }
    idcUpdate(idc);

    return topi;
}

static void riscv_aplic_request(void *opaque, int irq, int level)
{
    bool update = false;
    RISCVAPLICState *aplic = static_cast<RISCVAPLICState *>(opaque);
    uint32_t sourcecfg, childidx, irq_state, idc;

    assert((0 < irq) && (irq < aplic->num_irqs));

    sourcecfg = aplic->sourcecfg[irq];
    if (sourcecfg & APLIC_SOURCECFG_D) {
        childidx = sourcecfg & APLIC_SOURCECFG_CHILDIDX_MASK;
        if (childidx < aplic->num_children) {
            riscv_aplic_request(aplic->children[childidx], irq, level);
        }
        return;
    }

    irq_state = aplic->state[irq];
    switch (sourcecfg & APLIC_SOURCECFG_SM_MASK) {
    case APLIC_SOURCECFG_SM_EDGE_RISE:
        if ((level > 0) && !(irq_state & APLIC_ISTATE_INPUT) &&
            !(irq_state & APLIC_ISTATE_PENDING)) {
            aplic->setPendingRaw(irq, true);
            update = true;
        }
        break;
    case APLIC_SOURCECFG_SM_EDGE_FALL:
        if ((level <= 0) && (irq_state & APLIC_ISTATE_INPUT) &&
            !(irq_state & APLIC_ISTATE_PENDING)) {
            aplic->setPendingRaw(irq, true);
            update = true;
        }
        break;
    case APLIC_SOURCECFG_SM_LEVEL_HIGH:
        if ((level > 0) && !(irq_state & APLIC_ISTATE_PENDING)) {
            aplic->setPendingRaw(irq, true);
            update = true;
        }
        break;
    case APLIC_SOURCECFG_SM_LEVEL_LOW:
        if ((level <= 0) && !(irq_state & APLIC_ISTATE_PENDING)) {
            aplic->setPendingRaw(irq, true);
            update = true;
        }
        break;
    default:
        break;
    }

    if (level <= 0) {
        aplic->state[irq] &= ~APLIC_ISTATE_INPUT;
    } else {
        aplic->state[irq] |= APLIC_ISTATE_INPUT;
    }

    if (update) {
        if (aplic->msimode) {
            aplic->msiIrqUpdate(irq);
        } else {
            idc = aplic->target[irq] >> APLIC_TARGET_HART_IDX_SHIFT;
            idc &= APLIC_TARGET_HART_IDX_MASK;
            aplic->idcUpdate(idc);
        }
    }
}

uint64_t RISCVAPLICState::mmioRead(hwaddr addr, unsigned size)
{
    uint32_t irq, word, idc, sm;
    RISCVAPLICState *aplic = this;

    /* Reads must be 4 byte words */
    if ((addr & 0x3) != 0) {
        goto err;
    }

    if (addr == APLIC_DOMAINCFG) {
        return APLIC_DOMAINCFG_RDONLY | aplic->domaincfg |
               (aplic->msimode ? APLIC_DOMAINCFG_DM : 0);
    } else if ((APLIC_SOURCECFG_BASE <= addr) &&
            (addr < (APLIC_SOURCECFG_BASE + (aplic->num_irqs - 1) * 4))) {
        irq  = ((addr - APLIC_SOURCECFG_BASE) >> 2) + 1;
        return aplic->sourcecfg[irq];
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_MMSICFGADDR)) {
        return aplic->mmsicfgaddr;
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_MMSICFGADDRH)) {
        return aplic->mmsicfgaddrH;
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_SMSICFGADDR)) {
        /*
         * Registers SMSICFGADDR and SMSICFGADDRH are implemented only if:
         * (a) the interrupt domain is at machine level
         * (b) the domain's harts implement supervisor mode
         * (c) the domain has one or more child supervisor-level domains
         *     that support MSI delivery mode (domaincfg.DM is not read-
         *     only zero in at least one of the supervisor-level child
         * domains).
         */
        return (aplic->num_children) ? aplic->smsicfgaddr : 0;
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_SMSICFGADDRH)) {
        return (aplic->num_children) ? aplic->smsicfgaddrH : 0;
    } else if ((APLIC_SETIP_BASE <= addr) &&
            (addr < (APLIC_SETIP_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_SETIP_BASE) >> 2;
        return readPendingWord(word);
    } else if (addr == APLIC_SETIPNUM) {
        return 0;
    } else if ((APLIC_CLRIP_BASE <= addr) &&
            (addr < (APLIC_CLRIP_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_CLRIP_BASE) >> 2;
        return readInputWord(word);
    } else if (addr == APLIC_CLRIPNUM) {
        return 0;
    } else if ((APLIC_SETIE_BASE <= addr) &&
            (addr < (APLIC_SETIE_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_SETIE_BASE) >> 2;
        return readEnabledWord(word);
    } else if (addr == APLIC_SETIENUM) {
        return 0;
    } else if ((APLIC_CLRIE_BASE <= addr) &&
            (addr < (APLIC_CLRIE_BASE + aplic->bitfield_words * 4))) {
        return 0;
    } else if (addr == APLIC_CLRIENUM) {
        return 0;
    } else if (addr == APLIC_SETIPNUM_LE) {
        return 0;
    } else if (addr == APLIC_SETIPNUM_BE) {
        return 0;
    } else if (addr == APLIC_GENMSI) {
        return (aplic->msimode) ? aplic->genmsi : 0;
    } else if ((APLIC_TARGET_BASE <= addr) &&
            (addr < (APLIC_TARGET_BASE + (aplic->num_irqs - 1) * 4))) {
        irq = ((addr - APLIC_TARGET_BASE) >> 2) + 1;
        sm = aplic->sourcecfg[irq] & APLIC_SOURCECFG_SM_MASK;
        if (sm == APLIC_SOURCECFG_SM_INACTIVE) {
            return 0;
        }
        return aplic->target[irq];
    } else if (!aplic->msimode && (APLIC_IDC_BASE <= addr) &&
            (addr < (APLIC_IDC_BASE + aplic->num_harts * APLIC_IDC_SIZE))) {
        idc = (addr - APLIC_IDC_BASE) / APLIC_IDC_SIZE;
        switch (addr - (APLIC_IDC_BASE + idc * APLIC_IDC_SIZE)) {
        case APLIC_IDC_IDELIVERY:
            return aplic->idelivery[idc];
        case APLIC_IDC_IFORCE:
            return aplic->iforce[idc];
        case APLIC_IDC_ITHRESHOLD:
            return aplic->ithreshold[idc];
        case APLIC_IDC_TOPI:
            return idcTopi(idc);
        case APLIC_IDC_CLAIMI:
            return idcClaimi(idc);
        default:
            goto err;
        };
    }

err:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: Invalid register read 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

void RISCVAPLICState::mmioWrite(hwaddr addr, uint64_t value,
        unsigned size)
{
    RISCVAPLICState *aplic = this;
    uint32_t irq, word, idc = UINT32_MAX;

    /* Writes must be 4 byte words */
    if ((addr & 0x3) != 0) {
        goto err;
    }

    if (addr == APLIC_DOMAINCFG) {
        /* Only IE bit writable at the moment */
        value &= APLIC_DOMAINCFG_IE;
        aplic->domaincfg = value;
    } else if ((APLIC_SOURCECFG_BASE <= addr) &&
            (addr < (APLIC_SOURCECFG_BASE + (aplic->num_irqs - 1) * 4))) {
        irq  = ((addr - APLIC_SOURCECFG_BASE) >> 2) + 1;
        if (!aplic->num_children && (value & APLIC_SOURCECFG_D)) {
            value = 0;
        }
        if (value & APLIC_SOURCECFG_D) {
            value &= (APLIC_SOURCECFG_D | APLIC_SOURCECFG_CHILDIDX_MASK);
        } else {
            value &= (APLIC_SOURCECFG_D | APLIC_SOURCECFG_SM_MASK);
        }
        aplic->sourcecfg[irq] = value;
        if ((aplic->sourcecfg[irq] & APLIC_SOURCECFG_D) ||
            (aplic->sourcecfg[irq] == 0)) {
            setPendingRaw(irq, false);
            setEnabledRaw(irq, false);
        } else {
            if (irqRectifiedVal(irq)) {
                setPendingRaw(irq, true);
            }
        }
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_MMSICFGADDR)) {
        if (!(aplic->mmsicfgaddrH & APLIC_xMSICFGADDRH_L)) {
            aplic->mmsicfgaddr = value;
        }
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_MMSICFGADDRH)) {
        if (!(aplic->mmsicfgaddrH & APLIC_xMSICFGADDRH_L)) {
            aplic->mmsicfgaddrH = value & APLIC_MMSICFGADDRH_VALID_MASK;
        }
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_SMSICFGADDR)) {
        /*
         * Registers SMSICFGADDR and SMSICFGADDRH are implemented only if:
         * (a) the interrupt domain is at machine level
         * (b) the domain's harts implement supervisor mode
         * (c) the domain has one or more child supervisor-level domains
         *     that support MSI delivery mode (domaincfg.DM is not read-
         *     only zero in at least one of the supervisor-level child
         * domains).
         */
        if (aplic->num_children &&
            !(aplic->mmsicfgaddrH & APLIC_xMSICFGADDRH_L)) {
            aplic->smsicfgaddr = value;
        }
    } else if (aplic->mmode && aplic->msimode &&
               (addr == APLIC_SMSICFGADDRH)) {
        if (aplic->num_children &&
            !(aplic->mmsicfgaddrH & APLIC_xMSICFGADDRH_L)) {
            aplic->smsicfgaddrH = value & APLIC_SMSICFGADDRH_VALID_MASK;
        }
    } else if ((APLIC_SETIP_BASE <= addr) &&
            (addr < (APLIC_SETIP_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_SETIP_BASE) >> 2;
        setPendingWord(word, value, true);
    } else if (addr == APLIC_SETIPNUM) {
        setPending(value, true);
    } else if ((APLIC_CLRIP_BASE <= addr) &&
            (addr < (APLIC_CLRIP_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_CLRIP_BASE) >> 2;
        setPendingWord(word, value, false);
    } else if (addr == APLIC_CLRIPNUM) {
        setPending(value, false);
    } else if ((APLIC_SETIE_BASE <= addr) &&
            (addr < (APLIC_SETIE_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_SETIE_BASE) >> 2;
        setEnabledWord(word, value, true);
    } else if (addr == APLIC_SETIENUM) {
        setEnabled(value, true);
    } else if ((APLIC_CLRIE_BASE <= addr) &&
            (addr < (APLIC_CLRIE_BASE + aplic->bitfield_words * 4))) {
        word = (addr - APLIC_CLRIE_BASE) >> 2;
        setEnabledWord(word, value, false);
    } else if (addr == APLIC_CLRIENUM) {
        setEnabled(value, false);
    } else if (addr == APLIC_SETIPNUM_LE) {
        setPending(value, true);
    } else if (addr == APLIC_SETIPNUM_BE) {
        setPending(bswap32(value), true);
    } else if (addr == APLIC_GENMSI) {
        if (aplic->msimode) {
            aplic->genmsi = value & ~(APLIC_TARGET_GUEST_IDX_MASK <<
                                      APLIC_TARGET_GUEST_IDX_SHIFT);
            msiSend(value >> APLIC_TARGET_HART_IDX_SHIFT,
                                 0,
                                 value & APLIC_TARGET_EIID_MASK);
        }
    } else if ((APLIC_TARGET_BASE <= addr) &&
            (addr < (APLIC_TARGET_BASE + (aplic->num_irqs - 1) * 4))) {
        irq = ((addr - APLIC_TARGET_BASE) >> 2) + 1;
        if (aplic->msimode) {
            aplic->target[irq] = value;
        } else {
            aplic->target[irq] = (value & ~APLIC_TARGET_IPRIO_MASK) |
                                 ((value & aplic->iprio_mask) ?
                                  (value & aplic->iprio_mask) : 1);
        }
    } else if (!aplic->msimode && (APLIC_IDC_BASE <= addr) &&
            (addr < (APLIC_IDC_BASE + aplic->num_harts * APLIC_IDC_SIZE))) {
        idc = (addr - APLIC_IDC_BASE) / APLIC_IDC_SIZE;
        switch (addr - (APLIC_IDC_BASE + idc * APLIC_IDC_SIZE)) {
        case APLIC_IDC_IDELIVERY:
            aplic->idelivery[idc] = value & 0x1;
            break;
        case APLIC_IDC_IFORCE:
            aplic->iforce[idc] = value & 0x1;
            break;
        case APLIC_IDC_ITHRESHOLD:
            aplic->ithreshold[idc] = value & aplic->iprio_mask;
            break;
        default:
            goto err;
        };
    } else {
        goto err;
    }

    if (aplic->msimode) {
        for (irq = 1; irq < aplic->num_irqs; irq++) {
            msiIrqUpdate(irq);
        }
    } else {
        if (idc == UINT32_MAX) {
            for (idc = 0; idc < aplic->num_harts; idc++) {
                idcUpdate(idc);
            }
        } else {
            idcUpdate(idc);
        }
    }

    return;

err:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: Invalid register write 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
}

static uint64_t riscv_aplic_read(void *opaque, hwaddr addr, unsigned size)
{
    RISCVAPLICState *aplic = static_cast<RISCVAPLICState *>(opaque);
    return aplic->mmioRead(addr, size);
}

static void riscv_aplic_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    RISCVAPLICState *aplic = static_cast<RISCVAPLICState *>(opaque);
    aplic->mmioWrite(addr, value, size);
}

static const MemoryRegionOps riscv_aplic_ops = {
    .read = riscv_aplic_read,
    .write = riscv_aplic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4
    }
};

void RISCVAPLICState::realize(Error **errp)
{
    DeviceState *dev = reinterpret_cast<DeviceState *>(this);
    uint32_t i;

    if (riscv_use_emulated_aplic(msimode)) {
        /* Create output IRQ lines for non-MSI mode */
        if (!msimode) {
            /* Claim the CPU interrupt to be triggered by this APLIC */
            for (i = 0; i < num_harts; i++) {
                CPUState *temp = cpu_by_arch_id(hartid_base + i);
                if (temp == NULL) {
                    /* Valid for sparse hart layouts - skip this hart ID */
                    continue;
                }
                RISCVCPU *cpu = RISCV_CPU(temp);
                if (riscv_cpu_claim_interrupts(cpu,
                    (mmode) ? MIP_MEIP : MIP_SEIP) < 0) {
                    error_report("%s already claimed",
                                 (mmode) ? "MEIP" : "SEIP");
                    exit(1);
                }
            }

            external_irqs = static_cast<qemu_irq *>(g_malloc(sizeof(qemu_irq) *
                                            num_harts));
            qdev_init_gpio_out(dev, external_irqs, num_harts);
        }

        bitfield_words = (num_irqs + 31) >> 5;
        sourcecfg = g_new0(uint32_t, num_irqs);
        state = g_new0(uint32_t, num_irqs);
        target = g_new0(uint32_t, num_irqs);
        if (!msimode) {
            for (i = 0; i < num_irqs; i++) {
                target[i] = 1;
            }
        }
        idelivery = g_new0(uint32_t, num_harts);
        iforce = g_new0(uint32_t, num_harts);
        ithreshold = g_new0(uint32_t, num_harts);

        memory_region_init_io(&mmio, reinterpret_cast<Object *>(dev), &riscv_aplic_ops,
                              this, TYPE_RISCV_APLIC, aperture_size);
        sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(dev), &mmio);

        if (kvm_enabled()) {
            kvm_splitmode = true;
        }
    }

    /*
     * Only root APLICs have hardware IRQ lines. All non-root APLICs
     * have IRQ lines delegated by their parent APLIC.
     */
    if (!parent) {
        if (kvm_enabled() && !riscv_use_emulated_aplic(msimode)) {
            qdev_init_gpio_in(dev, riscv_kvm_aplic_request, num_irqs);
        } else {
            qdev_init_gpio_in(dev, riscv_aplic_request, num_irqs);
        }
    }

    msi_nonbroken = true;
}

static const Property riscv_aplic_properties[] = {
    DEFINE_PROP_UINT32("aperture-size", RISCVAPLICState, aperture_size, 0),
    DEFINE_PROP_UINT32("hartid-base", RISCVAPLICState, hartid_base, 0),
    DEFINE_PROP_UINT32("num-harts", RISCVAPLICState, num_harts, 0),
    DEFINE_PROP_UINT32("iprio-mask", RISCVAPLICState, iprio_mask, 0),
    DEFINE_PROP_UINT32("num-irqs", RISCVAPLICState, num_irqs, 0),
    DEFINE_PROP_BOOL("msimode", RISCVAPLICState, msimode, 0),
    DEFINE_PROP_BOOL("mmode", RISCVAPLICState, mmode, 0),
};

static bool riscv_aplic_state_needed(void *opaque)
{
    RISCVAPLICState *aplic = static_cast<RISCVAPLICState *>(opaque);

    return riscv_use_emulated_aplic(aplic->msimode);
}

static const VMStateField vmstate_riscv_aplic_fields[] = {
    VMSTATE_UINT32(domaincfg, RISCVAPLICState),
    VMSTATE_UINT32(mmsicfgaddr, RISCVAPLICState),
    VMSTATE_UINT32(mmsicfgaddrH, RISCVAPLICState),
    VMSTATE_UINT32(smsicfgaddr, RISCVAPLICState),
    VMSTATE_UINT32(smsicfgaddrH, RISCVAPLICState),
    VMSTATE_UINT32(genmsi, RISCVAPLICState),
    VMSTATE_UINT32(kvm_msicfgaddr, RISCVAPLICState),
    VMSTATE_UINT32(kvm_msicfgaddrH, RISCVAPLICState),
    VMSTATE_VARRAY_UINT32(sourcecfg, RISCVAPLICState,
                          num_irqs, 0,
                          vmstate_info_uint32, uint32_t),
    VMSTATE_VARRAY_UINT32(state, RISCVAPLICState,
                          num_irqs, 0,
                          vmstate_info_uint32, uint32_t),
    VMSTATE_VARRAY_UINT32(target, RISCVAPLICState,
                          num_irqs, 0,
                          vmstate_info_uint32, uint32_t),
    VMSTATE_VARRAY_UINT32(idelivery, RISCVAPLICState,
                          num_harts, 0,
                          vmstate_info_uint32, uint32_t),
    VMSTATE_VARRAY_UINT32(iforce, RISCVAPLICState,
                          num_harts, 0,
                          vmstate_info_uint32, uint32_t),
    VMSTATE_VARRAY_UINT32(ithreshold, RISCVAPLICState,
                          num_harts, 0,
                          vmstate_info_uint32, uint32_t),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_riscv_aplic = {
    .name = "riscv_aplic",
    .version_id = 3,
    .minimum_version_id = 3,
    .needed = riscv_aplic_state_needed,
    .fields = vmstate_riscv_aplic_fields,
};

void RISCVAPLICState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, riscv_aplic_properties);
    dc->vmsd = &vmstate_riscv_aplic;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(RISCVAPLICState, TYPE_RISCV_APLIC, TYPE_SYS_BUS_DEVICE)

/*
 * Add a APLIC device to another APLIC device as child for
 * interrupt delegation.
 */
void riscv_aplic_add_child(DeviceState *parent, DeviceState *child)
{
    RISCVAPLICState *caplic, *paplic;

    assert(parent && child);
    caplic = riscv_aplic_from_obj(child);
    paplic = riscv_aplic_from_obj(parent);

    assert(paplic->num_irqs == caplic->num_irqs);
    assert(paplic->num_children <= QEMU_APLIC_MAX_CHILDREN);

    caplic->parent = paplic;
    paplic->children[paplic->num_children] = caplic;
    paplic->num_children++;
}

/*
 * Create APLIC device.
 */
DeviceState *riscv_aplic_create(hwaddr addr, hwaddr size,
    uint32_t hartid_base, uint32_t num_harts, uint32_t num_sources,
    uint32_t iprio_bits, bool msimode, bool mmode, DeviceState *parent)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_APLIC);
    uint32_t i;

    assert(num_harts < APLIC_MAX_IDC);
    assert((APLIC_IDC_BASE + (num_harts * APLIC_IDC_SIZE)) <= size);
    assert(num_sources < APLIC_MAX_SOURCE);
    assert(APLIC_MIN_IPRIO_BITS <= iprio_bits);
    assert(iprio_bits <= APLIC_MAX_IPRIO_BITS);

    qdev_prop_set_uint32(dev, "aperture-size", size);
    qdev_prop_set_uint32(dev, "hartid-base", hartid_base);
    qdev_prop_set_uint32(dev, "num-harts", num_harts);
    qdev_prop_set_uint32(dev, "iprio-mask", ((1U << iprio_bits) - 1));
    qdev_prop_set_uint32(dev, "num-irqs", num_sources + 1);
    qdev_prop_set_bit(dev, "msimode", msimode);
    qdev_prop_set_bit(dev, "mmode", mmode);

    if (parent) {
        riscv_aplic_add_child(parent, dev);
    }

    sysbus_realize_and_unref(reinterpret_cast<SysBusDevice *>(dev), &error_fatal);

    if (riscv_use_emulated_aplic(msimode)) {
        sysbus_mmio_map(reinterpret_cast<SysBusDevice *>(dev), 0, addr);

        if (!msimode) {
            for (i = 0; i < num_harts; i++) {
                CPUState *cpu = cpu_by_arch_id(hartid_base + i);
                if (cpu == NULL) {
                    /* Valid for sparse hart layouts - skip this hart ID */
                    continue;
                }

                qdev_connect_gpio_out_named(dev, NULL, i,
                                            qdev_get_gpio_in(reinterpret_cast<DeviceState *>(cpu),
                                            (mmode) ? IRQ_M_EXT : IRQ_S_EXT));
            }
        }
    }

    return dev;
}
