/*
 * TC4x time base: the guest time that the CPUs' system timers (STM) and
 * the eGTM time base unit (TBU) count.
 *
 * Guest time runs at 1/scale of QEMU_CLOCK_VIRTUAL and does not run past
 * a barrier, where an interrupt pacer stops it until the main loop has
 * raised the pacer's tick.  The scale is the largest that any client asks
 * for (Tc4xTimeRequest).  A guest timer (Tc4xGuestTimer) fires once guest
 * time reaches its deadline: it is re-armed whenever the scale changes or
 * guest time resumes from the barrier.
 *
 * sysbus MMIO regions 0..6: the first page of the SFRs of CPU n, which
 * starts with its STM; region 7: the first page of eGTM cluster 0, which
 * holds TBU channel 0.  The pages serve the STM count (CPUn_STM_ABS) and
 * TBU channel 0 without the BQL and pass every other access to the SFR
 * store.  Named gpio-out "stm"[16 * n + 2 * vm + k]: service request k of
 * virtual machine vm of the STM of CPU n.
 *
 * Unless noted otherwise, a function runs with the BQL held.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_TIMEBASE_H
#define HW_TRICORE_TC4X_TIMEBASE_H

#include "qemu/queue.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/tricore/tc4x_pacer.h"
#include "qom/object.h"

#define TYPE_TC4X_TIMEBASE "tc4x-timebase"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xTimebase, TC4X_TIMEBASE)

#define TC4X_TIMEBASE_NUM_CPUS      7
#define TC4X_STM_HZ                 500000000ULL
#define TC4X_STM_VMS                8
#define TC4X_STM_IRQS_PER_CPU       (2 * TC4X_STM_VMS)
/* sysbus MMIO region of the eGTM cluster 0 page */
#define TC4X_TIMEBASE_TBU_MMIO      TC4X_TIMEBASE_NUM_CPUS

/* Guest time in ns (any thread) */
int64_t tc4x_timebase_ns(Tc4xTimebase *tb);

/* QEMU_CLOCK_VIRTUAL time at which guest time reaches @guest_ns */
int64_t tc4x_timebase_host_ns(Tc4xTimebase *tb, int64_t guest_ns);

/* The guest time scale in effect */
uint32_t tc4x_timebase_scale(Tc4xTimebase *tb);

/*
 * One client's request for the guest time scale: guest time runs at the
 * largest scale requested.  A new request asks for scale 1.
 */
typedef struct Tc4xTimeRequest Tc4xTimeRequest;
Tc4xTimeRequest *tc4x_timebase_new_request(Tc4xTimebase *tb);
void tc4x_timebase_request(Tc4xTimebase *tb, Tc4xTimeRequest *r,
                           uint32_t scale);

/*
 * A timer that calls @cb (in the main loop) once guest time reaches the
 * deadline it is armed for.  Fields are private to the time base.
 */
typedef struct Tc4xGuestTimer {
    QEMUTimer timer;
    Tc4xTimebase *tb;
    QEMUTimerCB *cb;
    void *opaque;
    int64_t deadline;
    bool armed;
    QLIST_ENTRY(Tc4xGuestTimer) next;
} Tc4xGuestTimer;

void tc4x_guest_timer_init(Tc4xGuestTimer *t, Tc4xTimebase *tb,
                           QEMUTimerCB *cb, void *opaque);
void tc4x_guest_timer_deinit(Tc4xGuestTimer *t);

/* Arm @t for guest time @guest_ns, replacing an earlier deadline */
void tc4x_guest_timer_mod(Tc4xGuestTimer *t, int64_t guest_ns);
void tc4x_guest_timer_del(Tc4xGuestTimer *t);
bool tc4x_guest_timer_pending(Tc4xGuestTimer *t);

/*
 * Guest time changed its rate or resumed from the barrier: @fn re-arms
 * timers that wait for a guest time and are not Tc4xGuestTimers
 */
typedef void (*Tc4xRetimeFn)(void *opaque);
void tc4x_timebase_add_retime_notifier(Tc4xTimebase *tb, Tc4xRetimeFn fn,
                                       void *opaque);

/*
 * A guest write of @val to the CPU SFR at @addr has been stored: apply its
 * effect on the STM (interrupt flags, compare timers)
 */
void tc4x_timebase_cpu_sfr_written(Tc4xTimebase *tb, uint32_t addr,
                                   uint32_t val);

/* An interrupt pacer (tc4x_pacer.h) on this time base, disabled */
Tc4xPacer *tc4x_timebase_new_pacer(Tc4xTimebase *tb,
                                   const Tc4xPacerConfig *cfg,
                                   const Tc4xPacerOps *ops, void *opaque);

#endif
