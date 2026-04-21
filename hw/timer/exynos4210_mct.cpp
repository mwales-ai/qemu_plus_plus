/*
 * Samsung exynos4210 Multi Core timer
 *
 * Copyright (c) 2000 - 2011 Samsung Electronics Co., Ltd.
 * All rights reserved.
 *
 * Evgeny Voevodin <e.voevodin@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Global Timer:
 *
 * Consists of two timers. First represents Free Running Counter and second
 * is used to measure interval from FRC to nearest comparator.
 *
 *        0                                                           UINT64_MAX
 *        |                              timer0                             |
 *        | <-------------------------------------------------------------- |
 *        | --------------------------------------------frc---------------> |
 *        |______________________________________________|__________________|
 *                CMP0          CMP1             CMP2    |           CMP3
 *                                                     __|            |_
 *                                                     |     timer1     |
 *                                                     | -------------> |
 *                                                    frc              CMPx
 *
 * Problem: when implementing global timer as is, overflow arises.
 * next_time = cur_time + period * count;
 * period and count are 64 bits width.
 * Lets arm timer for MCT_GT_COUNTER_STEP count and update internal G_CNT
 * register during each event.
 *
 * Problem: both timers need to be implemented using MCT_XT_COUNTER_STEP because
 * local timer contains two counters: TCNT and ICNT. TCNT == 0 -> ICNT--.
 * IRQ is generated when ICNT riches zero. Implementation where TCNT == 0
 * generates IRQs suffers from too frequently events. Better to have one
 * uint64_t counter equal to TCNT*ICNT and arm ptimer.c for a minimum(TCNT*ICNT,
 * MCT_GT_COUNTER_STEP); (yes, if target tunes ICNT * TCNT to be too low values,
 * there is no way to avoid frequently events).
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "hw/ptimer.h"

#include "hw/arm/exynos4210.h"
#include "hw/irq.h"
#include "qom/object.h"

//#define DEBUG_MCT

#ifdef DEBUG_MCT
#define DPRINTF(fmt, ...) \
        do { fprintf(stdout, "MCT: [%24s:%5d] " fmt, __func__, __LINE__, \
                     ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define    MCT_CFG          0x000
#define    G_CNT_L          0x100
#define    G_CNT_U          0x104
#define    G_CNT_WSTAT      0x110
#define    G_COMP0_L        0x200
#define    G_COMP0_U        0x204
#define    G_COMP0_ADD_INCR 0x208
#define    G_COMP1_L        0x210
#define    G_COMP1_U        0x214
#define    G_COMP1_ADD_INCR 0x218
#define    G_COMP2_L        0x220
#define    G_COMP2_U        0x224
#define    G_COMP2_ADD_INCR 0x228
#define    G_COMP3_L        0x230
#define    G_COMP3_U        0x234
#define    G_COMP3_ADD_INCR 0x238
#define    G_TCON           0x240
#define    G_INT_CSTAT      0x244
#define    G_INT_ENB        0x248
#define    G_WSTAT          0x24C
#define    L0_TCNTB         0x300
#define    L0_TCNTO         0x304
#define    L0_ICNTB         0x308
#define    L0_ICNTO         0x30C
#define    L0_FRCNTB        0x310
#define    L0_FRCNTO        0x314
#define    L0_TCON          0x320
#define    L0_INT_CSTAT     0x330
#define    L0_INT_ENB       0x334
#define    L0_WSTAT         0x340
#define    L1_TCNTB         0x400
#define    L1_TCNTO         0x404
#define    L1_ICNTB         0x408
#define    L1_ICNTO         0x40C
#define    L1_FRCNTB        0x410
#define    L1_FRCNTO        0x414
#define    L1_TCON          0x420
#define    L1_INT_CSTAT     0x430
#define    L1_INT_ENB       0x434
#define    L1_WSTAT         0x440

#define MCT_CFG_GET_PRESCALER(x)    ((x) & 0xFF)
#define MCT_CFG_GET_DIVIDER(x)      (1 << ((x) >> 8 & 7))

#define GET_G_COMP_IDX(offset)          (((offset) - G_COMP0_L) / 0x10)
#define GET_G_COMP_ADD_INCR_IDX(offset) (((offset) - G_COMP0_ADD_INCR) / 0x10)

#define G_COMP_L(x) (G_COMP0_L + (x) * 0x10)
#define G_COMP_U(x) (G_COMP0_U + (x) * 0x10)

#define G_COMP_ADD_INCR(x)  (G_COMP0_ADD_INCR + (x) * 0x10)

/* MCT bits */
#define G_TCON_COMP_ENABLE(x)   (1 << 2 * (x))
#define G_TCON_AUTO_ICREMENT(x) (1 << (2 * (x) + 1))
#define G_TCON_TIMER_ENABLE     (1 << 8)

#define G_INT_ENABLE(x)         (1 << (x))
#define G_INT_CSTAT_COMP(x)     (1 << (x))

#define G_CNT_WSTAT_L           1
#define G_CNT_WSTAT_U           2

#define G_WSTAT_COMP_L(x)       (1 << 4 * (x))
#define G_WSTAT_COMP_U(x)       (1 << ((4 * (x)) + 1))
#define G_WSTAT_COMP_ADDINCR(x) (1 << ((4 * (x)) + 2))
#define G_WSTAT_TCON_WRITE      (1 << 16)

#define GET_L_TIMER_IDX(offset) ((((offset) & 0xF00) - L0_TCNTB) / 0x100)
#define GET_L_TIMER_CNT_REG_IDX(offset, lt_i) \
        (((offset) - (L0_TCNTB + 0x100 * (lt_i))) >> 2)

#define L_ICNTB_MANUAL_UPDATE   (1 << 31)

#define L_TCON_TICK_START       (1)
#define L_TCON_INT_START        (1 << 1)
#define L_TCON_INTERVAL_MODE    (1 << 2)
#define L_TCON_FRC_START        (1 << 3)

#define L_INT_CSTAT_INTCNT      (1 << 0)
#define L_INT_CSTAT_FRCCNT      (1 << 1)

#define L_INT_INTENB_ICNTEIE    (1 << 0)
#define L_INT_INTENB_FRCEIE     (1 << 1)

#define L_WSTAT_TCNTB_WRITE     (1 << 0)
#define L_WSTAT_ICNTB_WRITE     (1 << 1)
#define L_WSTAT_FRCCNTB_WRITE   (1 << 2)
#define L_WSTAT_TCON_WRITE      (1 << 3)

enum LocalTimerRegCntIndexes {
    L_REG_CNT_TCNTB,
    L_REG_CNT_TCNTO,
    L_REG_CNT_ICNTB,
    L_REG_CNT_ICNTO,
    L_REG_CNT_FRCCNTB,
    L_REG_CNT_FRCCNTO,

    L_REG_CNT_AMOUNT
};

#define MCT_SFR_SIZE            0x444

#define MCT_GT_CMP_NUM          4

#define MCT_GT_COUNTER_STEP     0x100000000ULL
#define MCT_LT_COUNTER_STEP    0x100000000ULL
#define MCT_LT_CNT_LOW_LIMIT   0x100

/* Named structs extracted from parent structs for C++ compatibility */
struct gregs {
    uint64_t cnt;
    uint32_t cnt_wstat;
    uint32_t tcon;
    uint32_t int_cstat;
    uint32_t int_enb;
    uint32_t wstat;
    uint64_t comp[MCT_GT_CMP_NUM];
    uint32_t comp_add_incr[MCT_GT_CMP_NUM];
};

struct tick_timer {
    uint32_t cnt_run;           /* cnt timer is running */
    uint32_t int_run;           /* int timer is running */

    uint32_t last_icnto;
    uint32_t last_tcnto;
    uint32_t tcntb;             /* initial value for TCNTB */
    uint32_t icntb;             /* initial value for ICNTB */

    /* for step mode */
    uint64_t    distance;       /* distance to count to the next event */
    uint64_t    progress;       /* progress when counting by steps */
    uint64_t    count;          /* count to arm timer with */

    ptimer_state *ptimer_tick;  /* timer for tick counter */

    /* methods */
    uint32_t getIntCnto();
    uint32_t getCntCnto();
    void intStart();
    void intStop();
    void cntStart();
    void cntStop();
    void txBegin();
    void txCommit();
    void setCntb(uint32_t new_cnt, uint32_t new_int);
    void recalcCount();
    void timerInit();
    void timerEvent();
};

struct lregs {
    uint32_t    cnt[L_REG_CNT_AMOUNT];
    uint32_t    tcon;
    uint32_t    int_cstat;
    uint32_t    int_enb;
    uint32_t    wstat;
};

/* global timer */
typedef struct {
    qemu_irq  irq[MCT_GT_CMP_NUM];

    struct gregs reg;

    uint64_t count;            /* Value FRC was armed with */
    int32_t curr_comp;             /* Current comparator FRC is running to */

    ptimer_state *ptimer_frc;                   /* FRC timer */

    /* methods */
    void setFrcCount(uint64_t count);
    uint64_t getFrcCount();
    void frcStop();
    void frcStart();
    void frcTxBegin();
    void frcTxCommit();
    void raiseCompIrq(uint32_t id);
    void lowerCompIrq(uint32_t id);

} Exynos4210MCTGT;

/* local timer */
typedef struct {
    int         id;             /* timer id */
    qemu_irq    irq;            /* local timer irq */

    struct tick_timer tick_timer;

    /* use ptimer.c to represent count down timer */

    ptimer_state *ptimer_frc;   /* timer for free running counter */

    /* registers */
    struct lregs reg;

    /* methods */
    uint64_t getFrcCount();
    void updateFrcCount();
    void frcStart();
    void frcStop();
    void frcTxBegin();
    void frcTxCommit();
    static void lfrcEvent(void *opaque);
    static void ltickEvent(void *opaque);

} Exynos4210MCTLT;

#define TYPE_EXYNOS4210_MCT "exynos4210.mct"
OBJECT_DECLARE_SIMPLE_TYPE(Exynos4210MCTState, EXYNOS4210_MCT)

struct Exynos4210MCTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Registers */
    uint32_t    reg_mct_cfg;

    Exynos4210MCTLT l_timer[2];
    Exynos4210MCTGT g_timer;

    uint32_t    freq;                   /* all timers tick frequency, TCLK */

    /* Methods */
    int32_t findGcomp();
    uint64_t getGcompDistance(int32_t id);
    void restartGfrc();
    void updateFreq();

    static void gfrcEvent(void *opaque);
    static uint64_t mctRead(void *opaque, hwaddr offset, unsigned size);
    static void mctWrite(void *opaque, hwaddr offset, uint64_t value, unsigned size);
    void init();
    void finalize();
    void reset();
    static void classInit(DeviceClass *dc);
};

/*** VMState ***/
static const VMStateField vmstate_tick_timer_fields[] = {
    VMSTATE_UINT32(cnt_run, struct tick_timer),
    VMSTATE_UINT32(int_run, struct tick_timer),
    VMSTATE_UINT32(last_icnto, struct tick_timer),
    VMSTATE_UINT32(last_tcnto, struct tick_timer),
    VMSTATE_UINT32(tcntb, struct tick_timer),
    VMSTATE_UINT32(icntb, struct tick_timer),
    VMSTATE_UINT64(distance, struct tick_timer),
    VMSTATE_UINT64(progress, struct tick_timer),
    VMSTATE_UINT64(count, struct tick_timer),
    VMSTATE_PTIMER(ptimer_tick, struct tick_timer),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_tick_timer = {
    .name = "exynos4210.mct.tick_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_tick_timer_fields,
};

static const VMStateField vmstate_lregs_fields[] = {
    VMSTATE_UINT32_ARRAY(cnt, struct lregs, L_REG_CNT_AMOUNT),
    VMSTATE_UINT32(tcon, struct lregs),
    VMSTATE_UINT32(int_cstat, struct lregs),
    VMSTATE_UINT32(int_enb, struct lregs),
    VMSTATE_UINT32(wstat, struct lregs),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_lregs = {
    .name = "exynos4210.mct.lregs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_lregs_fields,
};

static const VMStateField vmstate_exynos4210_mct_lt_fields[] = {
    VMSTATE_INT32(id, Exynos4210MCTLT),
    VMSTATE_STRUCT(tick_timer, Exynos4210MCTLT, 0,
            vmstate_tick_timer,
            struct tick_timer),
    VMSTATE_PTIMER(ptimer_frc, Exynos4210MCTLT),
    VMSTATE_STRUCT(reg, Exynos4210MCTLT, 0,
            vmstate_lregs,
            struct lregs),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_exynos4210_mct_lt = {
    .name = "exynos4210.mct.lt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_exynos4210_mct_lt_fields,
};

static const VMStateField vmstate_gregs_fields[] = {
    VMSTATE_UINT64(cnt, struct gregs),
    VMSTATE_UINT32(cnt_wstat, struct gregs),
    VMSTATE_UINT32(tcon, struct gregs),
    VMSTATE_UINT32(int_cstat, struct gregs),
    VMSTATE_UINT32(int_enb, struct gregs),
    VMSTATE_UINT32(wstat, struct gregs),
    VMSTATE_UINT64_ARRAY(comp, struct gregs, MCT_GT_CMP_NUM),
    VMSTATE_UINT32_ARRAY(comp_add_incr, struct gregs,
            MCT_GT_CMP_NUM),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_gregs = {
    .name = "exynos4210.mct.lregs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_gregs_fields,
};

static const VMStateField vmstate_exynos4210_mct_gt_fields[] = {
    VMSTATE_STRUCT(reg, Exynos4210MCTGT, 0, vmstate_gregs,
            struct gregs),
    VMSTATE_UINT64(count, Exynos4210MCTGT),
    VMSTATE_INT32(curr_comp, Exynos4210MCTGT),
    VMSTATE_PTIMER(ptimer_frc, Exynos4210MCTGT),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_exynos4210_mct_gt = {
    .name = "exynos4210.mct.lt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_exynos4210_mct_gt_fields,
};

static const VMStateField vmstate_exynos4210_mct_state_fields[] = {
    VMSTATE_UINT32(reg_mct_cfg, Exynos4210MCTState),
    VMSTATE_STRUCT_ARRAY(l_timer, Exynos4210MCTState, 2, 0,
        vmstate_exynos4210_mct_lt, Exynos4210MCTLT),
    VMSTATE_STRUCT(g_timer, Exynos4210MCTState, 0,
        vmstate_exynos4210_mct_gt, Exynos4210MCTGT),
    VMSTATE_UINT32(freq, Exynos4210MCTState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_exynos4210_mct_state = {
    .name = "exynos4210.mct",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_exynos4210_mct_state_fields,
};

/*** Exynos4210MCTGT methods ***/

/*
 * Set counter of FRC global timer.
 * Must be called within frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTGT::setFrcCount(uint64_t cnt)
{
    count = cnt;
    DPRINTF("global timer frc set count 0x%llx\n", cnt);
    ptimer_set_count(ptimer_frc, cnt);
}

/*
 * Get counter of FRC global timer.
 */
uint64_t Exynos4210MCTGT::getFrcCount()
{
    uint64_t cnt = 0;
    cnt = ptimer_get_count(ptimer_frc);
    cnt = count - cnt;
    return reg.cnt + cnt;
}

/*
 * Stop global FRC timer
 * Must be called within frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTGT::frcStop()
{
    DPRINTF("global timer frc stop\n");
    ptimer_stop(ptimer_frc);
}

/*
 * Start global FRC timer
 * Must be called within frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTGT::frcStart()
{
    DPRINTF("global timer frc start\n");
    ptimer_run(ptimer_frc, 1);
}

/*
 * Start ptimer transaction for global FRC timer; this is just for
 * consistency with the way we wrap operations like stop and run.
 */
void Exynos4210MCTGT::frcTxBegin()
{
    ptimer_transaction_begin(ptimer_frc);
}

/* Commit ptimer transaction for global FRC timer. */
void Exynos4210MCTGT::frcTxCommit()
{
    ptimer_transaction_commit(ptimer_frc);
}

/*
 * Raise global timer CMP IRQ
 */
void Exynos4210MCTGT::raiseCompIrq(uint32_t id)
{
    /* If CSTAT is pending and IRQ is enabled */
    if ((reg.int_cstat & G_INT_CSTAT_COMP(id)) &&
            (reg.int_enb & G_INT_ENABLE(id))) {
        DPRINTF("gcmp timer[%u] IRQ\n", id);
        qemu_irq_raise(irq[id]);
    }
}

/*
 * Lower global timer CMP IRQ
 */
void Exynos4210MCTGT::lowerCompIrq(uint32_t id)
{
    qemu_irq_lower(irq[id]);
}

/*** Exynos4210MCTLT methods ***/

/*
 * Get counter of FRC local timer.
 */
uint64_t Exynos4210MCTLT::getFrcCount()
{
    return ptimer_get_count(ptimer_frc);
}

/*
 * Set counter of FRC local timer.
 * Must be called from within frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTLT::updateFrcCount()
{
    if (!reg.cnt[L_REG_CNT_FRCCNTB]) {
        ptimer_set_count(ptimer_frc, MCT_LT_COUNTER_STEP);
    } else {
        ptimer_set_count(ptimer_frc, reg.cnt[L_REG_CNT_FRCCNTB]);
    }
}

/*
 * Start local FRC timer
 * Must be called from within frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTLT::frcStart()
{
    ptimer_run(ptimer_frc, 1);
}

/*
 * Stop local FRC timer
 * Must be called from within frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTLT::frcStop()
{
    ptimer_stop(ptimer_frc);
}

/* Start ptimer transaction for local FRC timer */
void Exynos4210MCTLT::frcTxBegin()
{
    ptimer_transaction_begin(ptimer_frc);
}

/* Commit ptimer transaction for local FRC timer */
void Exynos4210MCTLT::frcTxCommit()
{
    ptimer_transaction_commit(ptimer_frc);
}

/*
 * Local timer free running counter tick handler
 */
void Exynos4210MCTLT::lfrcEvent(void *opaque)
{
    Exynos4210MCTLT *s = (Exynos4210MCTLT *)opaque;

    /* local frc expired */

    DPRINTF("\n");

    s->reg.int_cstat |= L_INT_CSTAT_FRCCNT;

    /* update frc counter */
    s->updateFrcCount();

    /* raise irq */
    if (s->reg.int_enb & L_INT_INTENB_FRCEIE) {
        qemu_irq_raise(s->irq);
    }

    /*  we reached here, this means that timer is enabled */
    s->frcStart();
}

/*
 * Local timer tick counter handler.
 * Don't use reloaded timers. If timer counter = zero
 * then handler called but after handler finished no
 * timer reload occurs.
 */
void Exynos4210MCTLT::ltickEvent(void *opaque)
{
    Exynos4210MCTLT *s = (Exynos4210MCTLT *)opaque;
    uint32_t tcnto;
    uint32_t icnto;
#ifdef DEBUG_MCT
    static uint64_t time1[2] = {0};
    static uint64_t time2[2] = {0};
#endif

    /* Call tick_timer event handler, it will update its tcntb and icntb. */
    s->tick_timer.timerEvent();

    /* get tick_timer cnt */
    tcnto = s->tick_timer.getCntCnto();

    /* get tick_timer int */
    icnto = s->tick_timer.getIntCnto();

    /* raise IRQ if needed */
    if (!icnto && s->reg.tcon & L_TCON_INT_START) {
        /* INT counter enabled and expired */

        s->reg.int_cstat |= L_INT_CSTAT_INTCNT;

        /* raise interrupt if enabled */
        if (s->reg.int_enb & L_INT_INTENB_ICNTEIE) {
#ifdef DEBUG_MCT
            time2[s->id] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            DPRINTF("local timer[%d] IRQ: %llx\n", s->id,
                    time2[s->id] - time1[s->id]);
            time1[s->id] = time2[s->id];
#endif
            qemu_irq_raise(s->irq);
        }

        /* reload ICNTB */
        if (s->reg.tcon & L_TCON_INTERVAL_MODE) {
            s->tick_timer.setCntb(
                    s->reg.cnt[L_REG_CNT_TCNTB],
                    s->reg.cnt[L_REG_CNT_ICNTB]);
        }
    } else {
        /* reload TCNTB */
        if (!tcnto) {
            s->tick_timer.setCntb(
                    s->reg.cnt[L_REG_CNT_TCNTB],
                    icnto);
        }
    }

    /* start tick_timer cnt */
    s->tick_timer.cntStart();

    /* start tick_timer int */
    s->tick_timer.intStart();
}

/*** tick_timer methods ***/

/*
 * Action on enabling local tick int timer
 */
void tick_timer::intStart()
{
    if (!int_run) {
        int_run = 1;
    }
}

/*
 * Action on disabling local tick int timer
 */
void tick_timer::intStop()
{
    if (int_run) {
        last_icnto = getIntCnto();
        int_run = 0;
    }
}

/*
 * Get count for INT timer
 */
uint32_t tick_timer::getIntCnto()
{
    uint32_t icnto;
    uint64_t remain;
    uint64_t counted;
    uint64_t cur_progress;

    uint64_t cnt = ptimer_get_count(ptimer_tick);
    if (cnt) {
        /* timer is still counting, called not from event */
        counted = count - ptimer_get_count(ptimer_tick);
        cur_progress = progress + counted;
    } else {
        /* timer expired earlier */
        cur_progress = progress;
    }

    remain = distance - cur_progress;

    if (!int_run) {
        /* INT is stopped. */
        icnto = last_icnto;
    } else {
        /* Both are counting */
        icnto = remain / tcntb;
    }

    return icnto;
}

/*
 * Start local tick cnt timer.
 * Must be called within txBegin/txCommit block.
 */
void tick_timer::cntStart()
{
    if (!cnt_run) {

        recalcCount();
        ptimer_set_count(ptimer_tick, count);
        ptimer_run(ptimer_tick, 1);

        cnt_run = 1;
    }
}

/*
 * Stop local tick cnt timer.
 * Must be called within txBegin/txCommit block.
 */
void tick_timer::cntStop()
{
    if (cnt_run) {

        last_tcnto = getCntCnto();

        if (int_run) {
            intStop();
        }

        ptimer_stop(ptimer_tick);

        cnt_run = 0;
    }
}

/* Start ptimer transaction for local tick timer */
void tick_timer::txBegin()
{
    ptimer_transaction_begin(ptimer_tick);
}

/* Commit ptimer transaction for local tick timer */
void tick_timer::txCommit()
{
    ptimer_transaction_commit(ptimer_tick);
}

/*
 * Get counter for CNT timer
 */
uint32_t tick_timer::getCntCnto()
{
    uint32_t tcnto;
    uint32_t icnto;
    uint64_t remain;
    uint64_t counted;
    uint64_t cur_progress;

    uint64_t cnt = ptimer_get_count(ptimer_tick);
    if (cnt) {
        /* timer is still counting, called not from event */
        counted = count - ptimer_get_count(ptimer_tick);
        cur_progress = progress + counted;
    } else {
        /* timer expired earlier */
        cur_progress = progress;
    }

    remain = distance - cur_progress;

    if (!cnt_run) {
        /* Both are stopped. */
        tcnto = last_tcnto;
    } else if (!int_run) {
        /* INT counter is stopped, progress is by CNT timer */
        tcnto = remain % tcntb;
    } else {
        /* Both are counting */
        icnto = remain / tcntb;
        if (icnto) {
            tcnto = remain % ((uint64_t)icnto * tcntb);
        } else {
            tcnto = remain % tcntb;
        }
    }

    return tcnto;
}

/*
 * Set new values of counters for CNT and INT timers
 * Must be called within txBegin/txCommit block.
 */
void tick_timer::setCntb(uint32_t new_cnt, uint32_t new_int)
{
    uint32_t cnt_stopped = 0;
    uint32_t int_stopped = 0;

    if (cnt_run) {
        cntStop();
        cnt_stopped = 1;
    }

    if (int_run) {
        intStop();
        int_stopped = 1;
    }

    tcntb = new_cnt + 1;
    icntb = new_int + 1;

    if (cnt_stopped) {
        cntStart();
    }
    if (int_stopped) {
        intStart();
    }

}

/*
 * Calculate new counter value for tick timer
 */
void tick_timer::recalcCount()
{
    uint64_t to_count;

    if ((cnt_run && last_tcnto) || (int_run && last_icnto)) {
        /*
         * one or both timers run and not counted to the end;
         * distance is not passed, recalculate with last_tcnto * last_icnto
         */

        if (last_tcnto) {
            to_count = (uint64_t)last_tcnto * last_icnto;
        } else {
            to_count = last_icnto;
        }
    } else {
        /* distance is passed, recalculate with tcnto * icnto */
        if (icntb) {
            distance = (uint64_t)tcntb * icntb;
        } else {
            distance = tcntb;
        }

        to_count = distance;
        progress = 0;
    }

    if (to_count > MCT_LT_COUNTER_STEP) {
        /* count by step */
        count = MCT_LT_COUNTER_STEP;
    } else {
        count = to_count;
    }
}

/*
 * Initialize tick_timer
 */
void tick_timer::timerInit()
{
    intStop();
    txBegin();
    cntStop();
    txCommit();

    count = 0;
    distance = 0;
    progress = 0;
    icntb = 0;
    tcntb = 0;
}

/*
 * tick_timer event.
 * Raises when abstract tick_timer expires.
 */
void tick_timer::timerEvent()
{
    progress += count;
}

/*** Exynos4210MCTState methods ***/

static void tx_ptimer_set_freq(ptimer_state *s, uint32_t freq)
{
    /*
     * callers of updateFreq() never do anything
     * else that needs to be in the same ptimer transaction, so
     * to avoid a lot of repetition we have a convenience function
     * for begin/set_freq/commit.
     */
    ptimer_transaction_begin(s);
    ptimer_set_freq(s, freq);
    ptimer_transaction_commit(s);
}

/*
 * Find next nearest Comparator. If current Comparator value equals to other
 * Comparator value, skip them both
 */
int32_t Exynos4210MCTState::findGcomp()
{
    int res;
    int i;
    int enabled;
    uint64_t min;
    int min_comp_i;
    uint64_t gfrc;
    uint64_t distance;
    uint64_t distance_min;
    int comp_i;

    /* get gfrc count */
    gfrc = g_timer.getFrcCount();

    min = UINT64_MAX;
    distance_min = UINT64_MAX;
    comp_i = MCT_GT_CMP_NUM;
    min_comp_i = MCT_GT_CMP_NUM;
    enabled = 0;

    /* lookup for nearest comparator */
    for (i = 0; i < MCT_GT_CMP_NUM; i++) {

        if (g_timer.reg.tcon & G_TCON_COMP_ENABLE(i)) {

            enabled = 1;

            if (g_timer.reg.comp[i] > gfrc) {
                /* Comparator is upper then FRC */
                distance = g_timer.reg.comp[i] - gfrc;

                if (distance <= distance_min) {
                    distance_min = distance;
                    comp_i = i;
                }
            } else {
                /* Comparator is below FRC, find the smallest */

                if (g_timer.reg.comp[i] <= min) {
                    min = g_timer.reg.comp[i];
                    min_comp_i = i;
                }
            }
        }
    }

    if (!enabled) {
        /* All Comparators disabled */
        res = -1;
    } else if (comp_i < MCT_GT_CMP_NUM) {
        /* Found upper Comparator */
        res = comp_i;
    } else {
        /* All Comparators are below or equal to FRC  */
        res = min_comp_i;
    }

    if (res >= 0) {
        DPRINTF("found comparator %d: "
                "comp 0x%llx distance 0x%llx, gfrc 0x%llx\n",
                res,
                g_timer.reg.comp[res],
                distance_min,
                gfrc);
    }

    return res;
}

/*
 * Get distance to nearest Comparator
 */
uint64_t Exynos4210MCTState::getGcompDistance(int32_t id)
{
    if (id == -1) {
        /* no enabled Comparators, choose max distance */
        return MCT_GT_COUNTER_STEP;
    }
    if (g_timer.reg.comp[id] - g_timer.reg.cnt < MCT_GT_COUNTER_STEP) {
        return g_timer.reg.comp[id] - g_timer.reg.cnt;
    } else {
        return MCT_GT_COUNTER_STEP;
    }
}

/*
 * Restart global FRC timer
 * Must be called within g_timer.frcTxBegin/frcTxCommit block.
 */
void Exynos4210MCTState::restartGfrc()
{
    uint64_t distance;

    g_timer.frcStop();

    g_timer.curr_comp = findGcomp();

    distance = getGcompDistance(g_timer.curr_comp);

    if (distance > MCT_GT_COUNTER_STEP || !distance) {
        distance = MCT_GT_COUNTER_STEP;
    }

    g_timer.setFrcCount(distance);
    g_timer.frcStart();
}

/*
 * Global timer FRC event handler.
 * Each event occurs when internal counter reaches counter + MCT_GT_COUNTER_STEP
 * Every time we arm global FRC timer to count for MCT_GT_COUNTER_STEP value
 */
void Exynos4210MCTState::gfrcEvent(void *opaque)
{
    Exynos4210MCTState *s = (Exynos4210MCTState *)opaque;
    int i;
    uint64_t distance;

    DPRINTF("\n");

    s->g_timer.reg.cnt += s->g_timer.count;

    /* Process all comparators */
    for (i = 0; i < MCT_GT_CMP_NUM; i++) {

        if (s->g_timer.reg.cnt == s->g_timer.reg.comp[i]) {
            /* reached nearest comparator */

            s->g_timer.reg.int_cstat |= G_INT_CSTAT_COMP(i);

            /* Auto increment */
            if (s->g_timer.reg.tcon & G_TCON_AUTO_ICREMENT(i)) {
                s->g_timer.reg.comp[i] += s->g_timer.reg.comp_add_incr[i];
            }

            /* IRQ */
            s->g_timer.raiseCompIrq(i);
        }
    }

    /* Reload FRC to reach nearest comparator */
    s->g_timer.curr_comp = s->findGcomp();
    distance = s->getGcompDistance(s->g_timer.curr_comp);
    if (distance > MCT_GT_COUNTER_STEP || !distance) {
        distance = MCT_GT_COUNTER_STEP;
    }
    s->g_timer.setFrcCount(distance);

    s->g_timer.frcStart();
}

/* update timer frequency */
void Exynos4210MCTState::updateFreq()
{
    uint32_t old_freq = freq;
    freq = 24000000 /
            ((MCT_CFG_GET_PRESCALER(reg_mct_cfg) + 1) *
                    MCT_CFG_GET_DIVIDER(reg_mct_cfg));

    if (old_freq != freq) {
        DPRINTF("freq=%uHz\n", freq);

        /* global timer */
        tx_ptimer_set_freq(g_timer.ptimer_frc, freq);

        /* local timer */
        tx_ptimer_set_freq(l_timer[0].tick_timer.ptimer_tick, freq);
        tx_ptimer_set_freq(l_timer[0].ptimer_frc, freq);
        tx_ptimer_set_freq(l_timer[1].tick_timer.ptimer_tick, freq);
        tx_ptimer_set_freq(l_timer[1].ptimer_frc, freq);
    }
}

/* set defaul_timer values for all fields */
void Exynos4210MCTState::reset()
{
    uint32_t i;

    reg_mct_cfg = 0;

    /* global timer */
    memset(&g_timer.reg, 0, sizeof(g_timer.reg));
    g_timer.frcTxBegin();
    g_timer.frcStop();
    g_timer.frcTxCommit();

    /* local timer */
    memset(l_timer[0].reg.cnt, 0, sizeof(l_timer[0].reg.cnt));
    memset(l_timer[1].reg.cnt, 0, sizeof(l_timer[1].reg.cnt));
    for (i = 0; i < 2; i++) {
        l_timer[i].reg.int_cstat = 0;
        l_timer[i].reg.int_enb = 0;
        l_timer[i].reg.tcon = 0;
        l_timer[i].reg.wstat = 0;
        l_timer[i].tick_timer.count = 0;
        l_timer[i].tick_timer.distance = 0;
        l_timer[i].tick_timer.progress = 0;
        l_timer[i].frcTxBegin();
        ptimer_stop(l_timer[i].ptimer_frc);
        l_timer[i].frcTxCommit();

        l_timer[i].tick_timer.timerInit();
    }

    updateFreq();

}

/* Multi Core Timer read */
uint64_t Exynos4210MCTState::mctRead(void *opaque, hwaddr offset,
        unsigned size)
{
    Exynos4210MCTState *s = static_cast<Exynos4210MCTState *>(opaque);
    int index;
    int shift;
    uint64_t count;
    uint32_t value = 0;
    int lt_i;

    switch (offset) {

    case MCT_CFG:
        value = s->reg_mct_cfg;
        break;

    case G_CNT_L: case G_CNT_U:
        shift = 8 * (offset & 0x4);
        count = s->g_timer.getFrcCount();
        value = UINT32_MAX & (count >> shift);
        DPRINTF("read FRC=0x%llx\n", count);
        break;

    case G_CNT_WSTAT:
        value = s->g_timer.reg.cnt_wstat;
        break;

    case G_COMP_L(0): case G_COMP_L(1): case G_COMP_L(2): case G_COMP_L(3):
    case G_COMP_U(0): case G_COMP_U(1): case G_COMP_U(2): case G_COMP_U(3):
        index = GET_G_COMP_IDX(offset);
        shift = 8 * (offset & 0x4);
        value = UINT32_MAX & (s->g_timer.reg.comp[index] >> shift);
    break;

    case G_TCON:
        value = s->g_timer.reg.tcon;
        break;

    case G_INT_CSTAT:
        value = s->g_timer.reg.int_cstat;
        break;

    case G_INT_ENB:
        value = s->g_timer.reg.int_enb;
        break;
    case G_WSTAT:
        value = s->g_timer.reg.wstat;
        break;

    case G_COMP0_ADD_INCR: case G_COMP1_ADD_INCR:
    case G_COMP2_ADD_INCR: case G_COMP3_ADD_INCR:
        value = s->g_timer.reg.comp_add_incr[GET_G_COMP_ADD_INCR_IDX(offset)];
        break;

        /* Local timers */
    case L0_TCNTB: case L0_ICNTB: case L0_FRCNTB:
    case L1_TCNTB: case L1_ICNTB: case L1_FRCNTB:
        lt_i = GET_L_TIMER_IDX(offset);
        index = GET_L_TIMER_CNT_REG_IDX(offset, lt_i);
        value = s->l_timer[lt_i].reg.cnt[index];
        break;

    case L0_TCNTO: case L1_TCNTO:
        lt_i = GET_L_TIMER_IDX(offset);

        value = s->l_timer[lt_i].tick_timer.getCntCnto();
        DPRINTF("local timer[%d] read TCNTO %x\n", lt_i, value);
        break;

    case L0_ICNTO: case L1_ICNTO:
        lt_i = GET_L_TIMER_IDX(offset);

        value = s->l_timer[lt_i].tick_timer.getIntCnto();
        DPRINTF("local timer[%d] read ICNTO %x\n", lt_i, value);
        break;

    case L0_FRCNTO: case L1_FRCNTO:
        lt_i = GET_L_TIMER_IDX(offset);

        value = s->l_timer[lt_i].getFrcCount();
        break;

    case L0_TCON: case L1_TCON:
        lt_i = ((offset & 0xF00) - L0_TCNTB) / 0x100;
        value = s->l_timer[lt_i].reg.tcon;
        break;

    case L0_INT_CSTAT: case L1_INT_CSTAT:
        lt_i = ((offset & 0xF00) - L0_TCNTB) / 0x100;
        value = s->l_timer[lt_i].reg.int_cstat;
        break;

    case L0_INT_ENB: case L1_INT_ENB:
        lt_i = ((offset & 0xF00) - L0_TCNTB) / 0x100;
        value = s->l_timer[lt_i].reg.int_enb;
        break;

    case L0_WSTAT: case L1_WSTAT:
        lt_i = ((offset & 0xF00) - L0_TCNTB) / 0x100;
        value = s->l_timer[lt_i].reg.wstat;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        break;
    }
    return value;
}

/* MCT write */
void Exynos4210MCTState::mctWrite(void *opaque, hwaddr offset,
        uint64_t value, unsigned size)
{
    Exynos4210MCTState *s = static_cast<Exynos4210MCTState *>(opaque);
    int index;  /* index in buffer which represents register set */
    int shift;
    int lt_i;
    uint64_t new_frc;
    uint32_t i;
    uint32_t old_val;
#ifdef DEBUG_MCT
    static uint32_t icntb_max[2] = {0};
    static uint32_t icntb_min[2] = {UINT32_MAX, UINT32_MAX};
    static uint32_t tcntb_max[2] = {0};
    static uint32_t tcntb_min[2] = {UINT32_MAX, UINT32_MAX};
#endif

    new_frc = s->g_timer.reg.cnt;

    switch (offset) {

    case MCT_CFG:
        s->reg_mct_cfg = value;
        s->updateFreq();
        break;

    case G_CNT_L:
    case G_CNT_U:
        if (offset == G_CNT_L) {

            DPRINTF("global timer write to reg.cntl %llx\n", value);

            new_frc = (s->g_timer.reg.cnt & (uint64_t)UINT32_MAX << 32) + value;
            s->g_timer.reg.cnt_wstat |= G_CNT_WSTAT_L;
        }
        if (offset == G_CNT_U) {

            DPRINTF("global timer write to reg.cntu %llx\n", value);

            new_frc = (s->g_timer.reg.cnt & UINT32_MAX) +
                    ((uint64_t)value << 32);
            s->g_timer.reg.cnt_wstat |= G_CNT_WSTAT_U;
        }

        s->g_timer.reg.cnt = new_frc;
        s->g_timer.frcTxBegin();
        s->restartGfrc();
        s->g_timer.frcTxCommit();
        break;

    case G_CNT_WSTAT:
        s->g_timer.reg.cnt_wstat &= ~(value);
        break;

    case G_COMP_L(0): case G_COMP_L(1): case G_COMP_L(2): case G_COMP_L(3):
    case G_COMP_U(0): case G_COMP_U(1): case G_COMP_U(2): case G_COMP_U(3):
        index = GET_G_COMP_IDX(offset);
        shift = 8 * (offset & 0x4);
        s->g_timer.reg.comp[index] =
                (s->g_timer.reg.comp[index] &
                (((uint64_t)UINT32_MAX << 32) >> shift)) +
                (value << shift);

        DPRINTF("comparator %d write 0x%llx val << %d\n", index, value, shift);

        if (offset & 0x4) {
            s->g_timer.reg.wstat |= G_WSTAT_COMP_U(index);
        } else {
            s->g_timer.reg.wstat |= G_WSTAT_COMP_L(index);
        }

        s->g_timer.frcTxBegin();
        s->restartGfrc();
        s->g_timer.frcTxCommit();
        break;

    case G_TCON:
        old_val = s->g_timer.reg.tcon;
        s->g_timer.reg.tcon = value;
        s->g_timer.reg.wstat |= G_WSTAT_TCON_WRITE;

        DPRINTF("global timer write to reg.g_tcon %llx\n", value);

        s->g_timer.frcTxBegin();

        /* Start FRC if transition from disabled to enabled */
        if ((value & G_TCON_TIMER_ENABLE) > (old_val &
                G_TCON_TIMER_ENABLE)) {
            s->restartGfrc();
        }
        if ((value & G_TCON_TIMER_ENABLE) < (old_val &
                G_TCON_TIMER_ENABLE)) {
            s->g_timer.frcStop();
        }

        /* Start CMP if transition from disabled to enabled */
        for (i = 0; i < MCT_GT_CMP_NUM; i++) {
            if ((value & G_TCON_COMP_ENABLE(i)) != (old_val &
                    G_TCON_COMP_ENABLE(i))) {
                s->restartGfrc();
            }
        }

        s->g_timer.frcTxCommit();
        break;

    case G_INT_CSTAT:
        s->g_timer.reg.int_cstat &= ~(value);
        for (i = 0; i < MCT_GT_CMP_NUM; i++) {
            if (value & G_INT_CSTAT_COMP(i)) {
                s->g_timer.lowerCompIrq(i);
            }
        }
        break;

    case G_INT_ENB:
        /* Raise IRQ if transition from disabled to enabled and CSTAT pending */
        for (i = 0; i < MCT_GT_CMP_NUM; i++) {
            if ((value & G_INT_ENABLE(i)) > (s->g_timer.reg.tcon &
                    G_INT_ENABLE(i))) {
                if (s->g_timer.reg.int_cstat & G_INT_CSTAT_COMP(i)) {
                    s->g_timer.raiseCompIrq(i);
                }
            }

            if ((value & G_INT_ENABLE(i)) < (s->g_timer.reg.tcon &
                    G_INT_ENABLE(i))) {
                s->g_timer.lowerCompIrq(i);
            }
        }

        DPRINTF("global timer INT enable %llx\n", value);
        s->g_timer.reg.int_enb = value;
        break;

    case G_WSTAT:
        s->g_timer.reg.wstat &= ~(value);
        break;

    case G_COMP0_ADD_INCR: case G_COMP1_ADD_INCR:
    case G_COMP2_ADD_INCR: case G_COMP3_ADD_INCR:
        index = GET_G_COMP_ADD_INCR_IDX(offset);
        s->g_timer.reg.comp_add_incr[index] = value;
        s->g_timer.reg.wstat |= G_WSTAT_COMP_ADDINCR(index);
        break;

        /* Local timers */
    case L0_TCON: case L1_TCON:
        lt_i = GET_L_TIMER_IDX(offset);
        old_val = s->l_timer[lt_i].reg.tcon;

        s->l_timer[lt_i].reg.wstat |= L_WSTAT_TCON_WRITE;
        s->l_timer[lt_i].reg.tcon = value;

        s->l_timer[lt_i].tick_timer.txBegin();
        /* Stop local CNT */
        if ((value & L_TCON_TICK_START) <
                (old_val & L_TCON_TICK_START)) {
            DPRINTF("local timer[%d] stop cnt\n", lt_i);
            s->l_timer[lt_i].tick_timer.cntStop();
        }

        /* Stop local INT */
        if ((value & L_TCON_INT_START) <
                (old_val & L_TCON_INT_START)) {
            DPRINTF("local timer[%d] stop int\n", lt_i);
            s->l_timer[lt_i].tick_timer.intStop();
        }

        /* Start local CNT */
        if ((value & L_TCON_TICK_START) >
        (old_val & L_TCON_TICK_START)) {
            DPRINTF("local timer[%d] start cnt\n", lt_i);
            s->l_timer[lt_i].tick_timer.cntStart();
        }

        /* Start local INT */
        if ((value & L_TCON_INT_START) >
        (old_val & L_TCON_INT_START)) {
            DPRINTF("local timer[%d] start int\n", lt_i);
            s->l_timer[lt_i].tick_timer.intStart();
        }
        s->l_timer[lt_i].tick_timer.txCommit();

        /* Start or Stop local FRC if TCON changed */
        s->l_timer[lt_i].frcTxBegin();
        if ((value & L_TCON_FRC_START) >
        (s->l_timer[lt_i].reg.tcon & L_TCON_FRC_START)) {
            DPRINTF("local timer[%d] start frc\n", lt_i);
            s->l_timer[lt_i].frcStart();
        }
        if ((value & L_TCON_FRC_START) <
                (s->l_timer[lt_i].reg.tcon & L_TCON_FRC_START)) {
            DPRINTF("local timer[%d] stop frc\n", lt_i);
            s->l_timer[lt_i].frcStop();
        }
        s->l_timer[lt_i].frcTxCommit();
        break;

    case L0_TCNTB: case L1_TCNTB:
        lt_i = GET_L_TIMER_IDX(offset);

        /*
         * TCNTB is updated to internal register only after CNT expired.
         * Due to this we should reload timer to nearest moment when CNT is
         * expired and then in event handler update tcntb to new TCNTB value.
         */
        s->l_timer[lt_i].tick_timer.txBegin();
        s->l_timer[lt_i].tick_timer.setCntb(value,
                s->l_timer[lt_i].tick_timer.icntb);
        s->l_timer[lt_i].tick_timer.txCommit();

        s->l_timer[lt_i].reg.wstat |= L_WSTAT_TCNTB_WRITE;
        s->l_timer[lt_i].reg.cnt[L_REG_CNT_TCNTB] = value;

#ifdef DEBUG_MCT
        if (tcntb_min[lt_i] > value) {
            tcntb_min[lt_i] = value;
        }
        if (tcntb_max[lt_i] < value) {
            tcntb_max[lt_i] = value;
        }
        DPRINTF("local timer[%d] TCNTB write %llx; max=%x, min=%x\n",
                lt_i, value, tcntb_max[lt_i], tcntb_min[lt_i]);
#endif
        break;

    case L0_ICNTB: case L1_ICNTB:
        lt_i = GET_L_TIMER_IDX(offset);

        s->l_timer[lt_i].reg.wstat |= L_WSTAT_ICNTB_WRITE;
        s->l_timer[lt_i].reg.cnt[L_REG_CNT_ICNTB] = value &
                ~L_ICNTB_MANUAL_UPDATE;

        /*
         * We need to avoid too small values for TCNTB*ICNTB. If not, IRQ event
         * could raise too fast disallowing QEMU to execute target code.
         */
        if (s->l_timer[lt_i].reg.cnt[L_REG_CNT_ICNTB] *
            s->l_timer[lt_i].reg.cnt[L_REG_CNT_TCNTB] < MCT_LT_CNT_LOW_LIMIT) {
            if (!s->l_timer[lt_i].reg.cnt[L_REG_CNT_TCNTB]) {
                s->l_timer[lt_i].reg.cnt[L_REG_CNT_ICNTB] =
                        MCT_LT_CNT_LOW_LIMIT;
            } else {
                s->l_timer[lt_i].reg.cnt[L_REG_CNT_ICNTB] =
                        MCT_LT_CNT_LOW_LIMIT /
                        s->l_timer[lt_i].reg.cnt[L_REG_CNT_TCNTB];
            }
        }

        if (value & L_ICNTB_MANUAL_UPDATE) {
            s->l_timer[lt_i].tick_timer.setCntb(
                    s->l_timer[lt_i].tick_timer.tcntb,
                    s->l_timer[lt_i].reg.cnt[L_REG_CNT_ICNTB]);
        }

#ifdef DEBUG_MCT
        if (icntb_min[lt_i] > value) {
            icntb_min[lt_i] = value;
        }
        if (icntb_max[lt_i] < value) {
            icntb_max[lt_i] = value;
        }
        DPRINTF("local timer[%d] ICNTB write %llx; max=%x, min=%x\n\n",
                lt_i, value, icntb_max[lt_i], icntb_min[lt_i]);
#endif
        break;

    case L0_FRCNTB: case L1_FRCNTB:
        lt_i = GET_L_TIMER_IDX(offset);
        DPRINTF("local timer[%d] FRCNTB write %llx\n", lt_i, value);

        s->l_timer[lt_i].reg.wstat |= L_WSTAT_FRCCNTB_WRITE;
        s->l_timer[lt_i].reg.cnt[L_REG_CNT_FRCCNTB] = value;

        break;

    case L0_TCNTO: case L1_TCNTO:
    case L0_ICNTO: case L1_ICNTO:
    case L0_FRCNTO: case L1_FRCNTO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "exynos4210.mct: write to RO register " HWADDR_FMT_plx,
                      offset);
        break;

    case L0_INT_CSTAT: case L1_INT_CSTAT:
        lt_i = GET_L_TIMER_IDX(offset);

        DPRINTF("local timer[%d] CSTAT write %llx\n", lt_i, value);

        s->l_timer[lt_i].reg.int_cstat &= ~value;
        if (!s->l_timer[lt_i].reg.int_cstat) {
            qemu_irq_lower(s->l_timer[lt_i].irq);
        }
        break;

    case L0_INT_ENB: case L1_INT_ENB:
        lt_i = GET_L_TIMER_IDX(offset);
        old_val = s->l_timer[lt_i].reg.int_enb;

        /* Raise Local timer IRQ if cstat is pending */
        if ((value & L_INT_INTENB_ICNTEIE) > (old_val & L_INT_INTENB_ICNTEIE)) {
            if (s->l_timer[lt_i].reg.int_cstat & L_INT_CSTAT_INTCNT) {
                qemu_irq_raise(s->l_timer[lt_i].irq);
            }
        }

        s->l_timer[lt_i].reg.int_enb = value;

        break;

    case L0_WSTAT: case L1_WSTAT:
        lt_i = GET_L_TIMER_IDX(offset);

        s->l_timer[lt_i].reg.wstat &= ~value;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps exynos4210_mct_ops = {
    .read = Exynos4210MCTState::mctRead,
    .write = Exynos4210MCTState::mctWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* MCT init */
void Exynos4210MCTState::init()
{
    int i;
    Object *obj = reinterpret_cast<Object *>(this);
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(this);

    /* Global timer */
    g_timer.ptimer_frc = ptimer_init(Exynos4210MCTState::gfrcEvent, this,
                                        PTIMER_POLICY_LEGACY);
    memset(&g_timer.reg, 0, sizeof(struct gregs));

    /* Local timers */
    for (i = 0; i < 2; i++) {
        l_timer[i].tick_timer.ptimer_tick =
            ptimer_init(Exynos4210MCTLT::ltickEvent, &l_timer[i],
                        PTIMER_POLICY_LEGACY);
        l_timer[i].ptimer_frc =
            ptimer_init(Exynos4210MCTLT::lfrcEvent, &l_timer[i],
                        PTIMER_POLICY_LEGACY);
        l_timer[i].id = i;
    }

    /* IRQs */
    for (i = 0; i < MCT_GT_CMP_NUM; i++) {
        sysbus_init_irq(dev, &g_timer.irq[i]);
    }
    for (i = 0; i < 2; i++) {
        sysbus_init_irq(dev, &l_timer[i].irq);
    }

    memory_region_init_io(&iomem, obj, &exynos4210_mct_ops, this,
                          "exynos4210-mct", MCT_SFR_SIZE);
    sysbus_init_mmio(dev, &iomem);
}

void Exynos4210MCTState::finalize()
{
    int i;

    ptimer_free(g_timer.ptimer_frc);

    for (i = 0; i < 2; i++) {
        ptimer_free(l_timer[i].tick_timer.ptimer_tick);
        ptimer_free(l_timer[i].ptimer_frc);
    }
}

void Exynos4210MCTState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_exynos4210_mct_state;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(Exynos4210MCTState, TYPE_EXYNOS4210_MCT, TYPE_SYS_BUS_DEVICE)
