/*
 * TC4x time base: guest time, the CPUs' system timers (STM) and the eGTM
 * time base unit (TBU) channel 0.
 *
 * The STM of each CPU counts guest time at TC4X_STM_HZ; each of its
 * virtual machines has two compare registers that raise one of the VM's
 * two service requests when the selected bits of the count match.
 * TBU channel 0 shows 27 bits of the same count.  Register layout:
 * Ifx_CPU_STM_* and Ifx_EGTM_CLS_TBU_CH0_BASE of the iLLD SFR headers
 * (tc4x_regs.h).  The compare and control registers are SFR store words;
 * the CPU SFR writes reach the time base through
 * tc4x_timebase_cpu_sfr_written() once stored.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/seqlock.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "hw/tricore/tc4x_ir.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "hw/tricore/tc4x_timebase.h"
#include "trace.h"

#define CPU_SFR_STRIDE      (A_CPU1_HRA_PC - A_CPU0_HRA_PC)
#define STM_ABS             (A_CPU0_STM_ABS - A_CPU0_STM_CLC)
#define STM_VM0             (A_CPU0_STMHV_VM0_CMP0 - A_CPU0_STM_CLC)
#define STM_VM_STRIDE       (A_CPU0_STMHV_VM1_CMP0 - A_CPU0_STMHV_VM0_CMP0)
#define STM_VM_END          (STM_VM0 + TC4X_STM_VMS * STM_VM_STRIDE)
/* registers of one VM, as offsets from its CMP0 */
#define STM_CMP0            0u
#define STM_CMP1            (A_CPU0_STMHV_VM0_CMP1 - A_CPU0_STMHV_VM0_CMP0)
#define STM_CMCON           (A_CPU0_STMHV_VM0_CMCON - A_CPU0_STMHV_VM0_CMP0)
#define STM_ICR             (A_CPU0_STMHV_VM0_STM_ICR - A_CPU0_STMHV_VM0_CMP0)
#define STM_ISCR            (A_CPU0_STMHV_VM0_ISCR - A_CPU0_STMHV_VM0_CMP0)
#define STM_ISR             (A_CPU0_STMHV_VM0_ISR - A_CPU0_STMHV_VM0_CMP0)
/*
 * The VM view (CPUn_STM_*) shows the registers of the VM the CPU runs in.
 * Without a hypervisor model that is VM0.
 */
#define STM_VIEW            (A_CPU0_STM_CMP0 - A_CPU0_STM_CLC)

#define TBU_CH0             A_EGTM_CLS0_TBU_CH0_BASE

/*
 * Idle cores poll CPUn_STM_ABS continuously, so each page is a lockless_io
 * region of its own: a smaller region would sit behind a subpage container
 * that takes the BQL.
 */
#define TIMEBASE_PAGE_SIZE  (1u << TARGET_PAGE_BITS)
#define TBU_PAGE            (TBU_CH0 & ~(TIMEBASE_PAGE_SIZE - 1))

/* A compare that lags the count by less than this is due at once */
#define STM_LATE_WINDOW     (TC4X_STM_HZ / 10)     /* 100 ms */

typedef struct Tc4xTimebasePage {
    MemoryRegion mr;
    Tc4xTimebase *tb;
    uint32_t base;
} Tc4xTimebasePage;

/* Compare register @cmp of VM @vm of the STM of CPU @cpu */
typedef struct Tc4xStmCompare {
    Tc4xGuestTimer timer;
    Tc4xTimebase *tb;
    uint8_t cpu;
    uint8_t vm;
    uint8_t cmp;
    bool fired;
    uint64_t fired_target;  /* compare value of the last match */
    uint64_t due;           /* STM count the timer is armed for */
} Tc4xStmCompare;

struct Tc4xTimeRequest {
    uint32_t scale;
};

typedef struct Tc4xRetimeNotifier {
    Tc4xRetimeFn fn;
    void *opaque;
} Tc4xRetimeNotifier;

/*
 * Locking: guest time (scale, base_host, base_guest, barrier) is written
 * under the BQL inside the seqlock and read on any thread; the rest is
 * under the BQL.
 */
struct Tc4xTimebase {
    SysBusDevice parent_obj;

    Tc4xSfrStore *sfr;
    Tc4xIr *ir;
    TriCoreCPU *cpu[TC4X_TIMEBASE_NUM_CPUS];
    bool imprecise_io;
    bool deterministic;     /* scale fixed at time_scale, no pacing */
    uint32_t time_scale;

    Tc4xTimebasePage stm_page[TC4X_TIMEBASE_NUM_CPUS];
    Tc4xTimebasePage tbu_page;
    qemu_irq stm_irq[TC4X_TIMEBASE_NUM_CPUS * TC4X_STM_IRQS_PER_CPU];

    /*
     * Guest time = base_guest + (QEMU virtual time - base_host) / scale,
     * at most barrier
     */
    QemuSeqLock lock;
    uint32_t scale;
    int64_t base_host;
    int64_t base_guest;
    int64_t barrier;

    GPtrArray *requests;
    Tc4xTimeRequest *sim_request;   /* of the sim bridge's timescale */
    QLIST_HEAD(, Tc4xGuestTimer) timers;
    GArray *retime;             /* Tc4xRetimeNotifier */
    Tc4xStmCompare stm[TC4X_TIMEBASE_NUM_CPUS][TC4X_STM_VMS][2];
};

/* Guest time at QEMU virtual time @virt_ns, before the barrier */
static int64_t guest_at(Tc4xTimebase *tb, int64_t virt_ns)
{
    return qatomic_read(&tb->base_guest) +
           (virt_ns - qatomic_read(&tb->base_host)) /
           qatomic_read(&tb->scale);
}

int64_t tc4x_timebase_ns(Tc4xTimebase *tb)
{
    int64_t ns;
    unsigned start;

    do {
        start = seqlock_read_begin(&tb->lock);
        ns = guest_at(tb, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        ns = MIN(ns, qatomic_read(&tb->barrier));
    } while (seqlock_read_retry(&tb->lock, start));
    return ns;
}

int64_t tc4x_timebase_host_ns(Tc4xTimebase *tb, int64_t guest_ns)
{
    return tb->base_host + (guest_ns - tb->base_guest) * tb->scale;
}

uint32_t tc4x_timebase_scale(Tc4xTimebase *tb)
{
    return tb->scale;
}

static uint64_t stm_now(Tc4xTimebase *tb)
{
    /* read on every timer poll: keep it a division by a constant */
    QEMU_BUILD_BUG_ON(NANOSECONDS_PER_SECOND % TC4X_STM_HZ);
    return tc4x_timebase_ns(tb) / (NANOSECONDS_PER_SECOND / TC4X_STM_HZ);
}

static void guest_timer_expired(void *opaque)
{
    Tc4xGuestTimer *t = opaque;

    if (tc4x_timebase_ns(t->tb) < t->deadline) {
        /* guest time stands at the barrier: re-armed when it resumes */
        return;
    }
    t->armed = false;
    t->cb(t->opaque);
}

void tc4x_guest_timer_init(Tc4xGuestTimer *t, Tc4xTimebase *tb,
                           QEMUTimerCB *cb, void *opaque)
{
    *t = (Tc4xGuestTimer) { .tb = tb, .cb = cb, .opaque = opaque };
    timer_init_ns(&t->timer, QEMU_CLOCK_VIRTUAL, guest_timer_expired, t);
    QLIST_INSERT_HEAD(&tb->timers, t, next);
}

void tc4x_guest_timer_deinit(Tc4xGuestTimer *t)
{
    timer_del(&t->timer);
    QLIST_REMOVE(t, next);
}

void tc4x_guest_timer_mod(Tc4xGuestTimer *t, int64_t guest_ns)
{
    t->deadline = guest_ns;
    t->armed = true;
    timer_mod(&t->timer, tc4x_timebase_host_ns(t->tb, guest_ns));
}

void tc4x_guest_timer_del(Tc4xGuestTimer *t)
{
    t->armed = false;
    timer_del(&t->timer);
}

bool tc4x_guest_timer_pending(Tc4xGuestTimer *t)
{
    return t->armed;
}

void tc4x_timebase_add_retime_notifier(Tc4xTimebase *tb, Tc4xRetimeFn fn,
                                       void *opaque)
{
    Tc4xRetimeNotifier n = { fn, opaque };

    g_array_append_val(tb->retime, n);
}

/* The mapping from guest to host time changed: re-arm every guest timer */
static void timebase_retime(Tc4xTimebase *tb)
{
    Tc4xGuestTimer *t;

    QLIST_FOREACH(t, &tb->timers, next) {
        if (t->armed) {
            timer_mod(&t->timer, tc4x_timebase_host_ns(tb, t->deadline));
        }
    }
    for (guint i = 0; i < tb->retime->len; i++) {
        Tc4xRetimeNotifier *n = &g_array_index(tb->retime,
                                               Tc4xRetimeNotifier, i);

        n->fn(n->opaque);
    }
}

/*
 * Move the barrier to @barrier (INT64_MAX: none).  If guest time stood at
 * the old barrier, it resumes from there (the wall time it stood still is
 * not made up), so the mapping changes and timers are re-armed.
 */
static void timebase_set_barrier(Tc4xTimebase *tb, int64_t barrier)
{
    int64_t v, old, unclamped, now;
    bool stood, held = false;

    /*
     * One clock sample inside the write section, so readers see guest time
     * continue from the same instant across the update.  Guest time never
     * runs backwards: a barrier behind it (a slot guest time ran past while
     * the barrier was lifted, e.g. during a long tick hold) holds guest time
     * where it is instead.
     */
    seqlock_write_begin(&tb->lock);
    v = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    old = qatomic_read(&tb->barrier);
    unclamped = guest_at(tb, v);
    stood = unclamped > old;
    now = MIN(unclamped, old);
    if (barrier < now) {
        barrier = now;
        held = true;
    }
    if (stood) {
        qatomic_set(&tb->base_host, v);
        qatomic_set(&tb->base_guest, now);
    }
    qatomic_set(&tb->barrier, barrier);
    seqlock_write_end(&tb->lock);
    trace_tc4x_timebase_barrier(barrier, held, stood);
    if (stood) {
        timebase_retime(tb);
    }
}

/* Guest time runs at 1/@scale of virtual time, continuous across the change */
static void timebase_set_scale(Tc4xTimebase *tb, uint32_t scale)
{
    int64_t v, now;

    seqlock_write_begin(&tb->lock);
    v = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    now = MIN(guest_at(tb, v), qatomic_read(&tb->barrier));
    qatomic_set(&tb->base_host, v);
    qatomic_set(&tb->base_guest, now);
    qatomic_set(&tb->scale, scale);
    seqlock_write_end(&tb->lock);
    trace_tc4x_timebase_set_scale(scale, now);
    timebase_retime(tb);
}

Tc4xTimeRequest *tc4x_timebase_new_request(Tc4xTimebase *tb)
{
    Tc4xTimeRequest *r = g_new(Tc4xTimeRequest, 1);

    r->scale = 1;
    g_ptr_array_add(tb->requests, r);
    return r;
}

void tc4x_timebase_request(Tc4xTimebase *tb, Tc4xTimeRequest *r,
                           uint32_t scale)
{
    uint32_t eff = 1;

    r->scale = MAX(1, scale);
    if (tb->deterministic) {
        return;
    }
    for (guint i = 0; i < tb->requests->len; i++) {
        Tc4xTimeRequest *q = g_ptr_array_index(tb->requests, i);

        eff = MAX(eff, q->scale);
    }
    if (eff != tb->scale) {
        timebase_set_scale(tb, eff);
    }
}

static uint32_t stm_vm_reg(int cpu, int vm, uint32_t off)
{
    return A_CPU0_STM_CLC + cpu * CPU_SFR_STRIDE + STM_VM0 +
           vm * STM_VM_STRIDE + off;
}

static uint32_t stm_get(Tc4xTimebase *tb, int cpu, int vm, uint32_t off)
{
    return tc4x_sfr_store_get(tb->sfr, stm_vm_reg(cpu, vm, off));
}

static void stm_schedule(Tc4xTimebase *tb, int cpu, int vm, int cmp)
{
    Tc4xStmCompare *t = &tb->stm[cpu][vm][cmp];
    uint32_t icr = stm_get(tb, cpu, vm, STM_ICR);
    uint32_t cmcon = stm_get(tb, cpu, vm, STM_CMCON);
    uint32_t msize = cmp ? FIELD_EX32(cmcon, CPU_STM_CMCON, MSIZE1) :
                           FIELD_EX32(cmcon, CPU_STM_CMCON, MSIZE0);
    uint32_t mstart = cmp ? FIELD_EX32(cmcon, CPU_STM_CMCON, MSTART1) :
                            FIELD_EX32(cmcon, CPU_STM_CMCON, MSTART0);
    bool en = cmp ? FIELD_EX32(icr, CPU_STM_ICR, CMP1EN) :
                    FIELD_EX32(icr, CPU_STM_ICR, CMP0EN);
    uint64_t mask = (2ULL << msize) - 1;
    uint64_t now = stm_now(tb);
    uint64_t cur, target, delta, due;

    if (!en) {
        tc4x_guest_timer_del(&t->timer);
        return;
    }
    target = stm_get(tb, cpu, vm, cmp ? STM_CMP1 : STM_CMP0) & mask;
    cur = (now >> mstart) & mask;
    delta = (target - cur) & mask;
    if (delta == 0) {
        delta = mask + 1;
    }
    /*
     * A compare can be found slightly behind the counter without having
     * matched: software that re-arms with CMP = ABS + period can be
     * preempted for a period in between, or a time-scale change reschedules
     * a timer whose callback had not run yet.  Rather than waiting 8.6 s for
     * the 32-bit wrap, treat it as due now unless this value already fired.
     */
    if (mask > 2 * STM_LATE_WINDOW &&
        ((cur - target) & mask) < STM_LATE_WINDOW &&
        !(t->fired && (t->fired_target & mask) == target)) {
        delta = 1;
    }
    due = (now >> mstart << mstart) + (delta << mstart);
    t->due = due;
    tc4x_guest_timer_mod(&t->timer, muldiv64(due, NANOSECONDS_PER_SECOND,
                                             TC4X_STM_HZ));
}

static void stm_fire(void *opaque)
{
    Tc4xStmCompare *t = opaque;
    Tc4xTimebase *tb = t->tb;
    uint32_t isr_addr = stm_vm_reg(t->cpu, t->vm, STM_ISR);
    uint32_t icr = stm_get(tb, t->cpu, t->vm, STM_ICR);
    bool os = t->cmp ? FIELD_EX32(icr, CPU_STM_ICR, CMP1OS) :
                       FIELD_EX32(icr, CPU_STM_ICR, CMP0OS);

    trace_tc4x_timebase_stm_match(t->cpu, t->vm, t->cmp,
                                  tc4x_timebase_ns(tb));
    tc4x_sfr_store_set(tb->sfr, isr_addr,
                       tc4x_sfr_store_get(tb->sfr, isr_addr) | BIT(t->cmp));
    t->fired = true;
    t->fired_target = stm_get(tb, t->cpu, t->vm,
                              t->cmp ? STM_CMP1 : STM_CMP0);
    qemu_irq_raise(tb->stm_irq[t->cpu * TC4X_STM_IRQS_PER_CPU +
                               2 * t->vm + os]);
    stm_schedule(tb, t->cpu, t->vm, t->cmp);
}

void tc4x_timebase_cpu_sfr_written(Tc4xTimebase *tb, uint32_t addr,
                                   uint32_t val)
{
    uint32_t off = (addr - A_CPU0_STM_CLC) % CPU_SFR_STRIDE;
    int cpu = (addr - A_CPU0_STM_CLC) / CPU_SFR_STRIDE;
    int vm;
    uint32_t r;

    if (off < STM_VM0 || off >= STM_VM_END) {
        return;
    }
    vm = (off - STM_VM0) / STM_VM_STRIDE;
    r = (off - STM_VM0) % STM_VM_STRIDE;
    if (r == STM_ISCR) {
        uint32_t isr_addr = stm_vm_reg(cpu, vm, STM_ISR);
        uint32_t isr = tc4x_sfr_store_get(tb->sfr, isr_addr);

        if (FIELD_EX32(val, CPU_STM_ISCR, CMP0IRR)) {
            isr &= ~R_CPU_STM_ISR_CMP0IR_MASK;
        }
        if (FIELD_EX32(val, CPU_STM_ISCR, CMP1IRR)) {
            isr &= ~R_CPU_STM_ISR_CMP1IR_MASK;
        }
        if (FIELD_EX32(val, CPU_STM_ISCR, CMP0IRS)) {
            isr |= R_CPU_STM_ISR_CMP0IR_MASK;
        }
        if (FIELD_EX32(val, CPU_STM_ISCR, CMP1IRS)) {
            isr |= R_CPU_STM_ISR_CMP1IR_MASK;
        }
        tc4x_sfr_store_set(tb->sfr, isr_addr, isr);
        tc4x_sfr_store_set(tb->sfr, addr, 0);
    } else if (r <= STM_ICR) {
        stm_schedule(tb, cpu, vm, 0);
        stm_schedule(tb, cpu, vm, 1);
    }
}

static hwaddr stm_page_offset(hwaddr off)
{
    if (off >= STM_VIEW && off < STM_VIEW + STM_VM_STRIDE) {
        return off - STM_VIEW + STM_VM0;
    }
    return off;
}

static MemTxResult stm_page_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    Tc4xTimebasePage *pg = opaque;

    if (tc4x_master_refused(attrs, "tc4x-timebase", pg->base + off, false)) {
        return MEMTX_ERROR;
    }
    if (off >= STM_ABS && off + size <= STM_ABS + 8) {
        *data = extract64(stm_now(pg->tb), (off - STM_ABS) * 8, size * 8);
    } else {
        *data = tc4x_sfr_store_access_read(pg->tb->sfr,
                                           pg->base + stm_page_offset(off),
                                           size, attrs);
    }
    return MEMTX_OK;
}

static MemTxResult stm_page_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    Tc4xTimebasePage *pg = opaque;

    if (tc4x_master_refused(attrs, "tc4x-timebase", pg->base + off, true)) {
        return MEMTX_ERROR;
    }
    tc4x_sfr_store_access_write(pg->tb->sfr, pg->base + stm_page_offset(off),
                                val, size, attrs);
    return MEMTX_OK;
}

static const MemoryRegionOps stm_page_ops = {
    .read_with_attrs = stm_page_read,
    .write_with_attrs = stm_page_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Served without the BQL like the STM, so that back-to-back STM and TBU
 * reads see the same delay.
 */
static MemTxResult tbu_page_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    Tc4xTimebasePage *pg = opaque;
    hwaddr ch0 = TBU_CH0 - TBU_PAGE;

    if (tc4x_master_refused(attrs, "tc4x-timebase", pg->base + off, false)) {
        return MEMTX_ERROR;
    }
    if (off >= ch0 && off + size <= ch0 + 4) {
        uint32_t base = stm_now(pg->tb) & R_EGTM_CLS_TBU_CH0_BASE_BASE_MASK;

        *data = extract32(base, (off - ch0) * 8, size * 8);
    } else {
        *data = tc4x_sfr_store_access_read(pg->tb->sfr, pg->base + off, size,
                                           attrs);
    }
    return MEMTX_OK;
}

static MemTxResult tbu_page_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    Tc4xTimebasePage *pg = opaque;

    if (tc4x_master_refused(attrs, "tc4x-timebase", pg->base + off, true)) {
        return MEMTX_ERROR;
    }
    tc4x_sfr_store_access_write(pg->tb->sfr, pg->base + off, val, size,
                                attrs);
    return MEMTX_OK;
}

static const MemoryRegionOps tbu_page_ops = {
    .read_with_attrs = tbu_page_read,
    .write_with_attrs = tbu_page_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* An interrupt pacer bound to the time base, the router and the CPUs */
typedef struct Tc4xTimebasePacer {
    Tc4xTimebase *tb;
    Tc4xPacer *pacer;
    Tc4xTimeRequest *request;
    QEMUTimer timer[TC4X_PACER_TIMERS];
} Tc4xTimebasePacer;

static int64_t pacer_guest_ns(void *opaque)
{
    Tc4xTimebasePacer *tp = opaque;

    return tc4x_timebase_ns(tp->tb);
}

static int64_t pacer_host_ns(void *opaque)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int64_t pacer_host_ns_for_guest(void *opaque, int64_t ns)
{
    Tc4xTimebasePacer *tp = opaque;

    return tc4x_timebase_host_ns(tp->tb, ns);
}

static void pacer_barrier(void *opaque, int64_t ns)
{
    Tc4xTimebasePacer *tp = opaque;

    timebase_set_barrier(tp->tb, ns);
}

static void pacer_request_scale(void *opaque, uint32_t scale)
{
    Tc4xTimebasePacer *tp = opaque;

    tc4x_timebase_request(tp->tb, tp->request, scale);
}

static uint32_t pacer_requested_scale(void *opaque)
{
    Tc4xTimebasePacer *tp = opaque;

    return tp->request->scale;
}

static bool pacer_pending(void *opaque, uint32_t src)
{
    Tc4xTimebasePacer *tp = opaque;
    uint32_t v = tc4x_ir_src_get(tp->tb->ir, src);
    uint32_t mask = R_SRC_SRCR_SRE_MASK | R_SRC_SRCR_SRR_MASK;

    return (v & mask) == mask;
}

static void pacer_raise(void *opaque, uint32_t src)
{
    Tc4xTimebasePacer *tp = opaque;

    qemu_irq_raise(tc4x_ir_src(tp->tb->ir, src));
}

static uint32_t pacer_cpu_prio(void *opaque, unsigned core)
{
    Tc4xTimebasePacer *tp = opaque;

    return tp->tb->cpu[core]->env.ICR & 0xFFu;
}

static void pacer_timer_mod(void *opaque, Tc4xPacerTimer t, int64_t ns)
{
    Tc4xTimebasePacer *tp = opaque;

    timer_mod(&tp->timer[t], ns);
}

static void pacer_timer_del(void *opaque, Tc4xPacerTimer t)
{
    Tc4xTimebasePacer *tp = opaque;

    timer_del(&tp->timer[t]);
}

static const Tc4xPacerHost pacer_host = {
    .guest_ns = pacer_guest_ns,
    .host_ns = pacer_host_ns,
    .host_ns_for_guest = pacer_host_ns_for_guest,
    .barrier = pacer_barrier,
    .request_scale = pacer_request_scale,
    .requested_scale = pacer_requested_scale,
    .pending = pacer_pending,
    .raise = pacer_raise,
    .cpu_prio = pacer_cpu_prio,
    .timer_mod = pacer_timer_mod,
    .timer_del = pacer_timer_del,
};

static void pacer_tick_timer(void *opaque)
{
    Tc4xTimebasePacer *tp = opaque;

    tc4x_pacer_timer(tp->pacer, TC4X_PACER_TIMER_TICK);
}

static void pacer_check_timer(void *opaque)
{
    Tc4xTimebasePacer *tp = opaque;

    tc4x_pacer_timer(tp->pacer, TC4X_PACER_TIMER_CHECK);
}

static void pacer_hold_timer(void *opaque)
{
    Tc4xTimebasePacer *tp = opaque;

    tc4x_pacer_timer(tp->pacer, TC4X_PACER_TIMER_HOLD);
}

static void pacer_acked(void *opaque, uint32_t src)
{
    Tc4xTimebasePacer *tp = opaque;

    tc4x_pacer_acked(tp->pacer, src);
}

static void pacer_retime(void *opaque)
{
    Tc4xTimebasePacer *tp = opaque;

    tc4x_pacer_retime(tp->pacer);
}

Tc4xPacer *tc4x_timebase_new_pacer(Tc4xTimebase *tb,
                                   const Tc4xPacerConfig *cfg,
                                   const Tc4xPacerOps *ops, void *opaque)
{
    Tc4xTimebasePacer *tp = g_new0(Tc4xTimebasePacer, 1);

    tp->tb = tb;
    tp->request = tc4x_timebase_new_request(tb);
    timer_init_ns(&tp->timer[TC4X_PACER_TIMER_TICK], QEMU_CLOCK_VIRTUAL,
                  pacer_tick_timer, tp);
    timer_init_ns(&tp->timer[TC4X_PACER_TIMER_CHECK], QEMU_CLOCK_VIRTUAL,
                  pacer_check_timer, tp);
    timer_init_ns(&tp->timer[TC4X_PACER_TIMER_HOLD], QEMU_CLOCK_VIRTUAL,
                  pacer_hold_timer, tp);
    tp->pacer = tc4x_pacer_create(&pacer_host, tp, cfg, ops, opaque);
    if (tb->deterministic) {
        tc4x_pacer_set_fixed(tp->pacer);
    }
    for (unsigned i = 0; i < cfg->n_srcs; i++) {
        tc4x_ir_add_ack_listener(tb->ir, cfg->srcs[i], pacer_acked, tp);
    }
    if (cfg->hold_src) {
        tc4x_ir_add_ack_listener(tb->ir, cfg->hold_src, pacer_acked, tp);
    }
    tc4x_timebase_add_retime_notifier(tb, pacer_retime, tp);
    return tp->pacer;
}

static void tc4x_timebase_reset_hold(Object *obj, ResetType type)
{
    Tc4xTimebase *tb = TC4X_TIMEBASE(obj);

    for (int c = 0; c < TC4X_TIMEBASE_NUM_CPUS; c++) {
        for (int vm = 0; vm < TC4X_STM_VMS; vm++) {
            for (int cmp = 0; cmp < 2; cmp++) {
                tc4x_guest_timer_del(&tb->stm[c][vm][cmp].timer);
                tb->stm[c][vm][cmp].fired = false;
            }
        }
    }
}

static void timebase_init_page(Tc4xTimebase *tb, Tc4xTimebasePage *pg,
                               const MemoryRegionOps *ops, const char *name,
                               uint32_t base)
{
    pg->tb = tb;
    pg->base = base;
    memory_region_init_io(&pg->mr, OBJECT(tb), ops, pg, name,
                          TIMEBASE_PAGE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(tb), &pg->mr);
}

static void tc4x_timebase_init(Object *obj)
{
    Tc4xTimebase *tb = TC4X_TIMEBASE(obj);

    for (int c = 0; c < TC4X_TIMEBASE_NUM_CPUS; c++) {
        g_autofree char *name = g_strdup_printf("cpu%d.stm-page", c);

        timebase_init_page(tb, &tb->stm_page[c], &stm_page_ops, name,
                           A_CPU0_STM_CLC + c * CPU_SFR_STRIDE);
    }
    timebase_init_page(tb, &tb->tbu_page, &tbu_page_ops, "egtm.tbu-page",
                       TBU_PAGE);
    qdev_init_gpio_out_named(DEVICE(obj), tb->stm_irq, "stm",
                             ARRAY_SIZE(tb->stm_irq));
    seqlock_init(&tb->lock);
    tb->scale = 1;
    tb->barrier = INT64_MAX;
    tb->requests = g_ptr_array_new_with_free_func(g_free);
    QLIST_INIT(&tb->timers);
    tb->retime = g_array_new(false, false, sizeof(Tc4xRetimeNotifier));
    for (int c = 0; c < TC4X_TIMEBASE_NUM_CPUS; c++) {
        for (int vm = 0; vm < TC4X_STM_VMS; vm++) {
            for (int cmp = 0; cmp < 2; cmp++) {
                Tc4xStmCompare *t = &tb->stm[c][vm][cmp];

                t->tb = tb;
                t->cpu = c;
                t->vm = vm;
                t->cmp = cmp;
                tc4x_guest_timer_init(&t->timer, tb, stm_fire, t);
            }
        }
    }
}

/*
 * The time stamps, and the compare view, whose accesses go to the store
 * words of VM0
 */
static bool stm_page_serves(void *opaque, hwaddr off)
{
    return (off >= STM_ABS && off < STM_ABS + 8) ||
           (off >= STM_VIEW && off < STM_VIEW + STM_VM_STRIDE);
}

static bool tbu_page_serves(void *opaque, hwaddr off)
{
    return (off & ~3u) == TBU_CH0 - TBU_PAGE;
}

/* A page passes the accesses it does not serve to the store */
static void timebase_enable_page(Tc4xTimebase *tb, Tc4xTimebasePage *pg,
                                 Tc4xSfrServesFn serves)
{
    memory_region_enable_lockless_io(&pg->mr);
    memory_region_set_imprecise_io(&pg->mr, tb->imprecise_io);
    tc4x_sfr_store_add_passthrough(tb->sfr, &pg->mr, serves, pg);
}

static void tc4x_timebase_realize(DeviceState *dev, Error **errp)
{
    Tc4xTimebase *tb = TC4X_TIMEBASE(dev);

    if (!tb->sfr) {
        error_setg(errp, "tc4x-timebase: link sfr-store is not set");
        return;
    }
    if (!tb->ir) {
        error_setg(errp, "tc4x-timebase: link ir is not set");
        return;
    }
    for (int c = 0; c < TC4X_TIMEBASE_NUM_CPUS; c++) {
        if (!tb->cpu[c]) {
            error_setg(errp, "tc4x-timebase: link cpu%d is not set", c);
            return;
        }
    }
    if (tb->time_scale < 1) {
        error_setg(errp, "tc4x-timebase: time-scale must be at least 1");
        return;
    }
    if (tb->deterministic) {
        tb->scale = tb->time_scale;
    } else if (tb->time_scale != 1) {
        error_setg(errp, "tc4x-timebase: time-scale needs deterministic=on");
        return;
    }
    for (int c = 0; c < TC4X_TIMEBASE_NUM_CPUS; c++) {
        timebase_enable_page(tb, &tb->stm_page[c], stm_page_serves);
    }
    timebase_enable_page(tb, &tb->tbu_page, tbu_page_serves);
}

static const Property tc4x_timebase_properties[] = {
    DEFINE_PROP_BOOL("imprecise-io", Tc4xTimebase, imprecise_io, false),
    DEFINE_PROP_BOOL("deterministic", Tc4xTimebase, deterministic, false),
    DEFINE_PROP_UINT32("time-scale", Tc4xTimebase, time_scale, 1),
    DEFINE_PROP_LINK("sfr-store", Tc4xTimebase, sfr, TYPE_TC4X_SFR_STORE,
                     Tc4xSfrStore *),
    DEFINE_PROP_LINK("ir", Tc4xTimebase, ir, TYPE_TC4X_IR, Tc4xIr *),
    DEFINE_PROP_LINK("cpu0", Tc4xTimebase, cpu[0], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
    DEFINE_PROP_LINK("cpu1", Tc4xTimebase, cpu[1], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
    DEFINE_PROP_LINK("cpu2", Tc4xTimebase, cpu[2], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
    DEFINE_PROP_LINK("cpu3", Tc4xTimebase, cpu[3], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
    DEFINE_PROP_LINK("cpu4", Tc4xTimebase, cpu[4], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
    DEFINE_PROP_LINK("cpu5", Tc4xTimebase, cpu[5], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
    DEFINE_PROP_LINK("cpu6", Tc4xTimebase, cpu[6], TYPE_TRICORE_CPU,
                     TriCoreCPU *),
};

static const VMStateDescription vmstate_tc4x_timebase = {
    .name = TYPE_TC4X_TIMEBASE,
    .unmigratable = 1,
};

/* timescale N: request guest time at 1/N of virtual time (0, 1: none) */
static bool tc4x_timebase_sim_run(Tc4xSimCommands *obj, int argc,
                                  char **argv, GString *out)
{
    Tc4xTimebase *tb = TC4X_TIMEBASE(obj);
    uint64_t scale;

    if (argc != 2 || !tc4x_sim_parse_uint(argv[1], UINT32_MAX, &scale)) {
        g_string_append(out, "err usage: timescale N\n");
        return true;
    }
    if (tb->deterministic) {
        g_string_append_printf(out, "err timescale is fixed at %u in "
                               "deterministic mode\n", tb->scale);
        return true;
    }
    if (!tb->sim_request) {
        tb->sim_request = tc4x_timebase_new_request(tb);
    }
    tc4x_timebase_request(tb, tb->sim_request, scale);
    g_string_append_printf(out, "ok %u\n", tc4x_timebase_scale(tb));
    return true;
}

static const char *const tc4x_timebase_sim_commands[] = { "timescale", NULL };

static void tc4x_timebase_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    Tc4xSimCommandsClass *sc = TC4X_SIM_COMMANDS_CLASS(oc);

    dc->desc = "TC4x time base (STM, TBU)";
    dc->realize = tc4x_timebase_realize;
    dc->vmsd = &vmstate_tc4x_timebase;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_timebase_reset_hold;
    sc->commands = tc4x_timebase_sim_commands;
    sc->run = tc4x_timebase_sim_run;
    device_class_set_props(dc, tc4x_timebase_properties);
}

static const TypeInfo tc4x_timebase_types[] = {
    {
        .name = TYPE_TC4X_TIMEBASE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xTimebase),
        .instance_init = tc4x_timebase_init,
        .class_init = tc4x_timebase_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_SIM_COMMANDS },
            { }
        },
    },
};

DEFINE_TYPES(tc4x_timebase_types)
