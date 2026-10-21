/*
 * Periodic interrupt pacer for the tc4x machine.
 *
 * Board code that interrupts one core at a fixed period of guest time needs
 * every period's interrupt taken before the next is due: software that
 * counts its interrupts as time steps loses one whenever two raises merge
 * into one pending request.  An emulated core is slower than the real one
 * and is descheduled by the host at random, so the pacer:
 *
 *   - raises the interrupts on an exact grid of guest time slots;
 *   - stops guest time at the next slot until the main loop has raised it,
 *     as the main loop runs the tick late;
 *   - holds guest time when a raised interrupt is still pending check_ns
 *     after the raise: guest time runs at hold_scale until the core takes
 *     it, and a tick that comes due meanwhile waits instead of merging;
 *   - optionally adapts the guest time scale to the hold rate and to the
 *     share of ticks that find the core at task level, so the core keeps
 *     time for its background work between interrupts;
 *   - can also hold guest time while one other interrupt is pending.
 *
 * While board code reports itself idle, ticks come at idle_period_ns and
 * guest time is neither stopped nor slowed.
 *
 * The pacer reaches time, interrupts and timers only through Tc4xPacerHost,
 * so tests can bind it to a fake host.  Everything runs in the main loop
 * under the BQL.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_PACER_H
#define HW_TRICORE_TC4X_PACER_H

#define TC4X_PACER_SRCS_MAX 4

typedef struct Tc4xPacer Tc4xPacer;

/* Hold and adaptive scale tuning: a zero field takes the default in () */
typedef struct Tc4xPacerTuning {
    int64_t hold_max_ns;        /* host time a hold lasts at most (1 s) */
    int64_t tick_poll_ns;       /* host time between looks at a hold */
    int64_t src_poll_ns;        /* ... of tc4x_pacer_hold() (both: period) */
    uint32_t src_hold_scale;    /* scale of tc4x_pacer_hold() (hold_scale) */
    uint32_t max_scale;         /* adaptive scale ceiling (hold_scale) */
    unsigned window;            /* driven ticks per adaptation (100) */
    /*
     * Per window, the scale rises above holds_high held ticks or under
     * task_low ticks that find the core at task level, and falls under
     * holds_low held ticks with over task_high at task level (a quarter,
     * a tenth, a quarter and half of the window)
     */
    unsigned holds_high;
    unsigned holds_low;
    unsigned task_low;
    unsigned task_high;
    unsigned urgent_ticks;      /* ticks between urgent raises (window) */
} Tc4xPacerTuning;

typedef struct Tc4xPacerConfig {
    int64_t period_ns;          /* tick period while driven */
    int64_t idle_period_ns;     /* tick period while idle */
    int64_t check_ns;           /* a tick still pending this long holds */
    uint32_t hold_scale;        /* guest time scale while a tick is held */
    unsigned core;              /* the core that takes the ticks */
    uint32_t srcs[TC4X_PACER_SRCS_MAX]; /* SRC addresses raised per tick */
    unsigned n_srcs;
    uint32_t hold_src;          /* SRC that tc4x_pacer_hold() waits for */
    Tc4xPacerTuning tune;
} Tc4xPacerConfig;

/* Board code, called from the tick */
typedef struct Tc4xPacerOps {
    /* advance board code to guest time now_ns */
    void (*step)(void *opaque, int64_t now_ns);
    /* driven: tick at period_ns and pace guest time */
    bool (*driven)(void *opaque);
    /* raise this tick's interrupts with tc4x_pacer_raise() */
    void (*raise)(void *opaque);
} Tc4xPacerOps;

typedef enum Tc4xPacerTimer {
    TC4X_PACER_TIMER_TICK,
    TC4X_PACER_TIMER_CHECK,
    TC4X_PACER_TIMER_HOLD,
    TC4X_PACER_TIMERS
} Tc4xPacerTimer;

/*
 * What the pacer needs from the SoC.  Host time is QEMU_CLOCK_VIRTUAL,
 * which follows the host clock while the guest runs; guest time runs at
 * 1/scale of it.
 */
typedef struct Tc4xPacerHost {
    int64_t (*guest_ns)(void *opaque);
    int64_t (*host_ns)(void *opaque);
    int64_t (*host_ns_for_guest)(void *opaque, int64_t guest_ns);
    /* guest time stops at guest_ns until moved (INT64_MAX: no barrier) */
    void (*barrier)(void *opaque, int64_t guest_ns);
    /* the pacer's request for the guest time scale, and its last one */
    void (*request_scale)(void *opaque, uint32_t scale);
    uint32_t (*requested_scale)(void *opaque);
    /* service request @src is enabled and pending */
    bool (*pending)(void *opaque, uint32_t src);
    void (*raise)(void *opaque, uint32_t src);
    /* current CPU priority (ICR.CCPN) of @core: 0 at task level */
    uint32_t (*cpu_prio)(void *opaque, unsigned core);
    void (*timer_mod)(void *opaque, Tc4xPacerTimer t, int64_t host_ns);
    void (*timer_del)(void *opaque, Tc4xPacerTimer t);
} Tc4xPacerHost;

typedef struct Tc4xPacerStats {
    int64_t ticks;
    int64_t merged;             /* raised while the last one was pending */
    int64_t merged_unchecked;   /* ... before the half-period check ran */
    int64_t merged_timeout;     /* ... after a hold of hold_max_ns */
    int64_t late;               /* raised over half a period late */
    /* host-time lateness of driven ticks: < 10, 30, 100, 300, 1000 us, more */
    int64_t late_hist[6];
    int64_t task_share;         /* per mille, last adaptation window */
    int64_t holds;
    int64_t src_holds;
    bool driven;
    bool adaptive;
    uint32_t scale;             /* scale applied while driven */
} Tc4xPacerStats;

/* A pacer for @cfg and @ops on the tc4x SoC, disabled */
typedef struct Tc4xSoc Tc4xSoc;
Tc4xPacer *tc4x_pacer_new(Tc4xSoc *soc, const Tc4xPacerConfig *cfg,
                          const Tc4xPacerOps *ops, void *opaque);

/* A pacer on another host, e.g. a test fake */
Tc4xPacer *tc4x_pacer_create(const Tc4xPacerHost *host, void *host_opaque,
                             const Tc4xPacerConfig *cfg,
                             const Tc4xPacerOps *ops, void *opaque);

/*
 * Start or stop ticking.  A stopped pacer leaves guest time alone: no
 * barrier, hold or raised scale.
 */
void tc4x_pacer_set_enabled(Tc4xPacer *p, bool enabled);

/*
 * Guest time scale while driven: @scale fixed, or with @adaptive the floor
 * of a scale that follows the hold rate, applied times @headroom (>= 1).
 * Takes effect at the next change of the applied scale.
 */
void tc4x_pacer_set_scale(Tc4xPacer *p, uint32_t scale, bool adaptive,
                          double headroom);

/* From Tc4xPacerOps.raise: raise @src for this tick */
void tc4x_pacer_raise(Tc4xPacer *p, uint32_t src);

/*
 * From Tc4xPacerOps.raise, before the raise of the core's main interrupt:
 * count this tick for the adaptive scale (holds since the last sample, core
 * at task level)
 */
void tc4x_pacer_sample(Tc4xPacer *p);

/*
 * While driven and adaptive: hold guest time until the core takes the
 * pending cfg->hold_src, for hold_max_ns of host time at most
 */
void tc4x_pacer_hold(Tc4xPacer *p);

/*
 * A deadline was missed (an interrupt raised while its last one was still
 * pending): raise the adaptive scale by half at once, at most once per
 * urgent_ticks ticks
 */
void tc4x_pacer_urgent(Tc4xPacer *p);

void tc4x_pacer_stats(Tc4xPacer *p, Tc4xPacerStats *stats);

/* Host side: timer @t expired, a core took @src, the guest time scale moved */
void tc4x_pacer_timer(Tc4xPacer *p, Tc4xPacerTimer t);
void tc4x_pacer_acked(Tc4xPacer *p, uint32_t src);
void tc4x_pacer_retime(Tc4xPacer *p);

#endif
