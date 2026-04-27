/*
 * ARM Generic Interrupt Controller v3 (emulation)
 *
 * Copyright (c) 2015 Huawei.
 * Copyright (c) 2016 Linaro Limited
 * Written by Shlomo Pongratz, Peter Maydell
 *
 * This code is licensed under the GPL, version 2 or (at your option)
 * any later version.
 */

/* This file contains implementation code for an interrupt controller
 * which implements the GICv3 architecture. Specifically this is where
 * the device class itself and the functions for handling interrupts
 * coming in and going out live.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/intc/arm_gicv3.h"
#include "gicv3_internal.h"

static bool irqbetter(GICv3CPUState *cs, int irq, uint8_t prio, bool nmi)
{
    /* Return true if this IRQ at this priority should take
     * precedence over the current recorded highest priority
     * pending interrupt for this CPU. We also return true if
     * the current recorded highest priority pending interrupt
     * is the same as this one (a property which the calling code
     * relies on).
     */
    if (prio != cs->hppi.prio) {
        return prio < cs->hppi.prio;
    }

    /*
     * The same priority IRQ with non-maskable property should signal to
     * the CPU as it have the priority higher than the labelled 0x80 or 0x00.
     */
    if (nmi != cs->hppi.nmi) {
        return nmi;
    }

    /* If multiple pending interrupts have the same priority then it is an
     * IMPDEF choice which of them to signal to the CPU. We choose to
     * signal the one with the lowest interrupt number.
     */
    if (irq <= cs->hppi.irq) {
        return true;
    }
    return false;
}

uint32_t GICv3State::gisdIntPending(int irq)
{
    /* Recalculate which distributor interrupts are actually pending
     * in the group of 32 interrupts starting at irq (which should be a multiple
     * of 32), and return a 32-bit integer which has a bit set for each
     * interrupt that is eligible to be signaled to the CPU interface.
     *
     * An interrupt is pending if:
     *  + the PENDING latch is set OR it is level triggered and the input is 1
     *  + its ENABLE bit is set
     *  + the GICD enable bit for its group is set
     *  + its ACTIVE bit is not set (otherwise it would be Active+Pending)
     * Conveniently we can bulk-calculate this with bitwise operations.
     */
    uint32_t pend, grpmask;
    uint32_t pend_bmp = *gic_bmp_ptr32(pending, irq);
    uint32_t edge_trig = *gic_bmp_ptr32(edge_trigger, irq);
    uint32_t lvl = *gic_bmp_ptr32(level, irq);
    uint32_t grp = *gic_bmp_ptr32(group, irq);
    uint32_t grpmod_val = *gic_bmp_ptr32(grpmod, irq);
    uint32_t enable = *gic_bmp_ptr32(enabled, irq);
    uint32_t act = *gic_bmp_ptr32(active, irq);

    pend = pend_bmp | (~edge_trig & lvl);
    pend &= enable;
    pend &= ~act;

    if (gicd_ctlr & GICD_CTLR_DS) {
        grpmod_val = 0;
    }

    grpmask = 0;
    if (gicd_ctlr & GICD_CTLR_EN_GRP1NS) {
        grpmask |= grp;
    }
    if (gicd_ctlr & GICD_CTLR_EN_GRP1S) {
        grpmask |= (~grp & grpmod_val);
    }
    if (gicd_ctlr & GICD_CTLR_EN_GRP0) {
        grpmask |= (~grp & ~grpmod_val);
    }
    pend &= grpmask;

    return pend;
}

static uint32_t gicr_int_pending(GICv3CPUState *cs)
{
    /* Recalculate which redistributor interrupts are actually pending,
     * and return a 32-bit integer which has a bit set for each interrupt
     * that is eligible to be signaled to the CPU interface.
     *
     * An interrupt is pending if:
     *  + the PENDING latch is set OR it is level triggered and the input is 1
     *  + its ENABLE bit is set
     *  + the GICD enable bit for its group is set
     *  + its ACTIVE bit is not set (otherwise it would be Active+Pending)
     * Conveniently we can bulk-calculate this with bitwise operations.
     */
    uint32_t pend, grpmask, grpmod;

    pend = cs->gicr_ipendr0 | (~cs->edge_trigger & cs->level);
    pend &= cs->gicr_ienabler0;
    pend &= ~cs->gicr_iactiver0;

    if (cs->gic->gicd_ctlr & GICD_CTLR_DS) {
        grpmod = 0;
    } else {
        grpmod = cs->gicr_igrpmodr0;
    }

    grpmask = 0;
    if (cs->gic->gicd_ctlr & GICD_CTLR_EN_GRP1NS) {
        grpmask |= cs->gicr_igroupr0;
    }
    if (cs->gic->gicd_ctlr & GICD_CTLR_EN_GRP1S) {
        grpmask |= (~cs->gicr_igroupr0 & grpmod);
    }
    if (cs->gic->gicd_ctlr & GICD_CTLR_EN_GRP0) {
        grpmask |= (~cs->gicr_igroupr0 & ~grpmod);
    }
    pend &= grpmask;

    return pend;
}

static bool gicv3_get_priority(GICv3CPUState *cs, bool is_redist, int irq,
                               uint8_t *prio)
{
    uint32_t nmi = 0x0;

    if (is_redist) {
        nmi = extract32(cs->gicr_inmir0, irq, 1);
    } else {
        nmi = *gic_bmp_ptr32(cs->gic->nmi, irq);
        nmi = nmi & (1 << (irq & 0x1f));
    }

    if (nmi) {
        /* DS = 0 & Non-secure NMI */
        if (!(cs->gic->gicd_ctlr & GICD_CTLR_DS) &&
            ((is_redist && extract32(cs->gicr_igroupr0, irq, 1)) ||
             (!is_redist && gicv3_gicd_group_test(cs->gic, irq)))) {
            *prio = 0x80;
        } else {
            *prio = 0x0;
        }

        return true;
    }

    if (is_redist) {
        *prio = cs->gicr_ipriorityr[irq];
    } else {
        *prio = cs->gic->gicd_ipriority[irq];
    }

    return false;
}

/* Update the interrupt status after state in a redistributor
 * or CPU interface has changed, but don't tell the CPU i/f.
 */
static void gicv3_redist_update_noirqset(GICv3CPUState *cs)
{
    /* Find the highest priority pending interrupt among the
     * redistributor interrupts (SGIs and PPIs).
     */
    bool seenbetter = false;
    uint8_t prio;
    int i;
    uint32_t pend;
    bool nmi = false;

    /* Find out which redistributor interrupts are eligible to be
     * signaled to the CPU interface.
     */
    pend = gicr_int_pending(cs);

    if (pend) {
        for (i = 0; i < GIC_INTERNAL; i++) {
            if (!(pend & (1 << i))) {
                continue;
            }
            nmi = gicv3_get_priority(cs, true, i, &prio);
            if (irqbetter(cs, i, prio, nmi)) {
                cs->hppi.irq = i;
                cs->hppi.prio = prio;
                cs->hppi.nmi = nmi;
                seenbetter = true;
            }
        }
    }

    if (seenbetter) {
        cs->hppi.grp = gicv3_irq_group(cs->gic, cs, cs->hppi.irq);
    }

    if ((cs->gicr_ctlr & GICR_CTLR_ENABLE_LPIS) && cs->gic->lpi_enable &&
        (cs->gic->gicd_ctlr & GICD_CTLR_EN_GRP1NS) &&
        (cs->hpplpi.prio != 0xff)) {
        if (irqbetter(cs, cs->hpplpi.irq, cs->hpplpi.prio, cs->hpplpi.nmi)) {
            cs->hppi.irq = cs->hpplpi.irq;
            cs->hppi.prio = cs->hpplpi.prio;
            cs->hppi.nmi = cs->hpplpi.nmi;
            cs->hppi.grp = cs->hpplpi.grp;
            seenbetter = true;
        }
    }

    /* If the best interrupt we just found would preempt whatever
     * was the previous best interrupt before this update, then
     * we know it's definitely the best one now.
     * If we didn't find an interrupt that would preempt the previous
     * best, and the previous best is outside our range (or there was no
     * previous pending interrupt at all), then that is still valid, and
     * we leave it as the best.
     * Otherwise, we need to do a full update (because the previous best
     * interrupt has reduced in priority and any other interrupt could
     * now be the new best one).
     */
    if (!seenbetter && cs->hppi.prio != 0xff &&
        (cs->hppi.irq < GIC_INTERNAL ||
         cs->hppi.irq >= GICV3_LPI_INTID_START)) {
        cs->gic->fullUpdateNoirqset();
    }
}

/* Update the GIC status after state in a redistributor or
 * CPU interface has changed, and inform the CPU i/f of
 * its new highest priority pending interrupt.
 */
void gicv3_redist_update(GICv3CPUState *cs)
{
    gicv3_redist_update_noirqset(cs);
    gicv3_cpuif_update(cs);
}

/* Update the GIC status after state in the distributor has
 * changed affecting @len interrupts starting at @start,
 * but don't tell the CPU i/f.
 */
void GICv3State::updateNoirqset(int start, int len)
{
    int i;
    uint8_t prio;
    uint32_t pend = 0;
    bool nmi_val = false;

    assert(start >= GIC_INTERNAL);
    assert(len > 0);

    for (i = 0; i < num_cpu; i++) {
        cpu[i].seenbetter = false;
    }

    /* Find the highest priority pending interrupt in this range. */
    for (i = start; i < start + len; i++) {
        GICv3CPUState *cs;

        if (i == start || (i & 0x1f) == 0) {
            /* Calculate the next 32 bits worth of pending status */
            pend = gisdIntPending(i & ~0x1f);
        }

        if (!(pend & (1 << (i & 0x1f)))) {
            continue;
        }
        cs = gicd_irouter_target[i];
        if (!cs) {
            /* Interrupts targeting no implemented CPU should remain pending
             * and not be forwarded to any CPU.
             */
            continue;
        }
        nmi_val = gicv3_get_priority(cs, false, i, &prio);
        if (irqbetter(cs, i, prio, nmi_val)) {
            cs->hppi.irq = i;
            cs->hppi.prio = prio;
            cs->hppi.nmi = nmi_val;
            cs->seenbetter = true;
        }
    }

    /* If the best interrupt we just found would preempt whatever
     * was the previous best interrupt before this update, then
     * we know it's definitely the best one now.
     * If we didn't find an interrupt that would preempt the previous
     * best, and the previous best is outside our range (or there was
     * no previous pending interrupt at all), then that
     * is still valid, and we leave it as the best.
     * Otherwise, we need to do a full update (because the previous best
     * interrupt has reduced in priority and any other interrupt could
     * now be the new best one).
     */
    for (i = 0; i < num_cpu; i++) {
        GICv3CPUState *cs = &cpu[i];

        if (cs->seenbetter) {
            cs->hppi.grp = gicv3_irq_group(cs->gic, cs, cs->hppi.irq);
        }

        if (!cs->seenbetter && cs->hppi.prio != 0xff &&
            cs->hppi.irq >= start && cs->hppi.irq < start + len) {
            fullUpdateNoirqset();
            break;
        }
    }
}

void GICv3State::update(int start, int len)
{
    int i;

    updateNoirqset(start, len);
    for (i = 0; i < num_cpu; i++) {
        gicv3_cpuif_update(&cpu[i]);
    }
}

void gicv3_update(GICv3State *s, int start, int len)
{
    s->update(start, len);
}

void GICv3State::fullUpdateNoirqset()
{
    /* Completely recalculate the GIC status from scratch, but
     * don't update any outbound IRQ lines.
     */
    int i;

    for (i = 0; i < num_cpu; i++) {
        cpu[i].hppi.prio = 0xff;
        cpu[i].hppi.nmi = false;
    }

    /* Note that we can guarantee that these functions will not
     * recursively call back into fullUpdate(), because
     * at each point the "previous best" is always outside the
     * range we ask them to update.
     */
    updateNoirqset(GIC_INTERNAL, num_irq - GIC_INTERNAL);

    for (i = 0; i < num_cpu; i++) {
        gicv3_redist_update_noirqset(&cpu[i]);
    }
}

void gicv3_full_update_noirqset(GICv3State *s)
{
    s->fullUpdateNoirqset();
}

void GICv3State::fullUpdate()
{
    /* Completely recalculate the GIC status from scratch, including
     * updating outbound IRQ lines.
     */
    int i;

    fullUpdateNoirqset();
    for (i = 0; i < num_cpu; i++) {
        gicv3_cpuif_update(&cpu[i]);
    }
}

void gicv3_full_update(GICv3State *s)
{
    s->fullUpdate();
}

/* Process a change in an external IRQ input. */
void GICv3State::setIrq(int irq, int level)
{
    /* Meaning of the 'irq' parameter:
     *  [0..N-1] : external interrupts
     *  [N..N+31] : PPI (internal) interrupts for CPU 0
     *  [N+32..N+63] : PPI (internal interrupts for CPU 1
     *  ...
     */
    if (irq < (num_irq - GIC_INTERNAL)) {
        /* external interrupt (SPI) */
        gicv3_dist_set_irq(this, irq + GIC_INTERNAL, level);
    } else {
        /* per-cpu interrupt (PPI) */
        int cpuidx;

        irq -= (num_irq - GIC_INTERNAL);
        cpuidx = irq / GIC_INTERNAL;
        irq %= GIC_INTERNAL;
        assert(cpuidx < num_cpu);
        /* Raising SGIs via this function would be a bug in how the board
         * model wires up interrupts.
         */
        assert(irq >= GIC_NR_SGIS);
        gicv3_redist_set_irq(&cpu[cpuidx], irq, level);
    }
}

static void gicv3_set_irq(void *opaque, int irq, int level)
{
    GICv3State *s = static_cast<GICv3State *>(opaque);
    s->setIrq(irq, level);
}

void GICv3State::postLoad()
{
    int i;
    /* Recalculate our cached idea of the current highest priority
     * pending interrupt, but don't set IRQ or FIQ lines.
     */
    for (i = 0; i < num_cpu; i++) {
        gicv3_redist_update_lpi_only(&cpu[i]);
    }
    fullUpdateNoirqset();
    /* Repopulate the cache of GICv3CPUState pointers for target CPUs */
    gicv3_cache_all_target_cpustates(this);
}

static void arm_gicv3_post_load(GICv3State *s)
{
    s->postLoad();
}

static const MemoryRegionOps gic_ops[] = {
    {
        .read_with_attrs = gicv3_dist_read,
        .write_with_attrs = gicv3_dist_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8, },
        .impl = { .min_access_size = 1, .max_access_size = 8, },
    },
    {
        .read_with_attrs = gicv3_redist_read,
        .write_with_attrs = gicv3_redist_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8, },
        .impl = { .min_access_size = 1, .max_access_size = 8, },
    }
};

void GICv3State::realize(DeviceState *dev, Error **errp)
{
    /* Device instance realize function for the GIC sysbus device */
    ARMGICv3Class *agc = ARM_GICV3_GET_CLASS(this);
    Error *local_err = NULL;

    agc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    gicv3_init_irqs_and_mmio(this, gicv3_set_irq, gic_ops);

    gicv3_init_cpuif(this);
}

static void arm_gic_realize(DeviceState *dev, Error **errp)
{
    reinterpret_cast<GICv3State *>(dev)->realize(dev, errp);
}

#include "qom/cpp/object.h"

static void arm_gicv3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ARMGICv3CommonClass *agcc = ARM_GICV3_COMMON_CLASS(oc);
    ARMGICv3Class *agc = ARM_GICV3_CLASS(oc);

    agcc->post_load = arm_gicv3_post_load;
    device_class_set_parent_realize(dc, arm_gic_realize, &agc->parent_realize);
}

REGISTER_QEMU_DEVICE_CUSTOM_CI(GICv3State, ARMGICv3Class, TYPE_ARM_GICV3,
                               TYPE_ARM_GICV3_COMMON, arm_gicv3_class_init)
