/*
 * Periodic interrupt pacer for the tc4x machine (see tc4x_pacer.h).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/tricore/tc4x_pacer.h"

/*
 * Adaptive scale: per window of driven ticks it rises with many held ticks
 * and falls with few (down to the floor).  The core's task level must also
 * get CPU between the interrupts, so the scale also rises when few ticks
 * found the core at task level and only falls when many did.
 */
struct Tc4xPacer {
    const Tc4xPacerHost *host;
    void *host_opaque;
    Tc4xPacerConfig cfg;
    Tc4xPacerTuning tune;       /* cfg.tune with the defaults filled in */
    const Tc4xPacerOps *ops;
    void *opaque;

    bool enabled;
    bool driven;
    int64_t tick_due;       /* guest time of the next tick (0: none) */
    int64_t check_due;      /* half-period check of the last one (0: none) */
    int64_t tick_now;       /* guest time of the tick being raised */
    bool tick_hold;         /* guest time held: the core owes a tick */
    bool tick_was_held;     /* a hold since the last sample */
    int64_t hold_host_ns;   /* host time the current tick hold started */
    bool src_hold;          /* guest time held until hold_src is taken */
    int64_t src_hold_host_ns;

    uint32_t floor_scale;   /* applied while driven (floor when adaptive) */
    uint32_t live_scale;    /* adaptive scale currently applied */
    bool adaptive;
    double headroom;
    uint32_t idle_scale;    /* the request before the pacer raised it */
    int64_t window_ticks;
    int64_t window_holds;
    int64_t window_task;
    int64_t last_bump_tick;

    Tc4xPacerStats stats;
};

static int64_t host_ns(Tc4xPacer *p)
{
    return p->host->host_ns(p->host_opaque);
}

static int64_t guest_ns(Tc4xPacer *p)
{
    return p->host->guest_ns(p->host_opaque);
}

static int64_t host_ns_for_guest(Tc4xPacer *p, int64_t ns)
{
    return p->host->host_ns_for_guest(p->host_opaque, ns);
}

static void barrier(Tc4xPacer *p, int64_t ns)
{
    p->host->barrier(p->host_opaque, ns);
}

static void timer_set(Tc4xPacer *p, Tc4xPacerTimer t, int64_t ns)
{
    p->host->timer_mod(p->host_opaque, t, ns);
}

static void timer_clear(Tc4xPacer *p, Tc4xPacerTimer t)
{
    p->host->timer_del(p->host_opaque, t);
}

static bool pending(Tc4xPacer *p, uint32_t src)
{
    return p->host->pending(p->host_opaque, src);
}

/* Only an enabled request can be taken: before that, ticks just merge */
static bool tick_owed(Tc4xPacer *p)
{
    for (unsigned i = 0; i < p->cfg.n_srcs; i++) {
        if (pending(p, p->cfg.srcs[i])) {
            return true;
        }
    }
    return false;
}

static bool is_tick_src(Tc4xPacer *p, uint32_t src)
{
    for (unsigned i = 0; i < p->cfg.n_srcs; i++) {
        if (p->cfg.srcs[i] == src) {
            return true;
        }
    }
    return false;
}

static uint32_t driven_scale(Tc4xPacer *p)
{
    return p->adaptive ? (uint32_t)(p->live_scale * p->headroom) :
                         p->live_scale;
}

/*
 * The driven scale, raised during holds, and never below what was requested
 * before board code was driven
 */
static void apply_scale(Tc4xPacer *p)
{
    uint32_t applied = driven_scale(p);

    if (p->src_hold) {
        applied = MAX(applied, p->tune.src_hold_scale);
    }
    if (p->tick_hold) {
        applied = MAX(applied, p->cfg.hold_scale);
    }
    p->host->request_scale(p->host_opaque, MAX(applied, p->idle_scale));
}

static void schedule(Tc4xPacer *p)
{
    if (!p->enabled) {
        timer_clear(p, TC4X_PACER_TIMER_TICK);
        timer_clear(p, TC4X_PACER_TIMER_CHECK);
        return;
    }
    if (!p->tick_due) {
        p->tick_due = guest_ns(p) + p->cfg.period_ns;
    }
    timer_set(p, TC4X_PACER_TIMER_TICK, host_ns_for_guest(p, p->tick_due));
    if (p->check_due) {
        timer_set(p, TC4X_PACER_TIMER_CHECK,
                  host_ns_for_guest(p, p->check_due));
    }
}

/*
 * Next slot of the grid; a tick that ran more than a period late (host
 * stall) restarts the grid from now instead of firing a burst of catch-ups.
 */
static void advance(Tc4xPacer *p, int64_t now)
{
    int64_t period = p->driven ? p->cfg.period_ns : p->cfg.idle_period_ns;

    p->tick_due = p->tick_due && now - p->tick_due < period ?
                  p->tick_due + period : now + period;
}

static void tick_hold_start(Tc4xPacer *p)
{
    p->tick_hold = true;
    p->tick_was_held = true;
    p->hold_host_ns = host_ns(p);
    p->stats.holds++;
    apply_scale(p);
}

static void src_hold_end(Tc4xPacer *p)
{
    timer_clear(p, TC4X_PACER_TIMER_HOLD);
    p->src_hold = false;
    apply_scale(p);
}

void tc4x_pacer_hold(Tc4xPacer *p)
{
    if (!p->adaptive || p->src_hold || !pending(p, p->cfg.hold_src)) {
        return;
    }
    p->src_hold = true;
    p->src_hold_host_ns = host_ns(p);
    p->stats.src_holds++;
    apply_scale(p);
    timer_set(p, TC4X_PACER_TIMER_HOLD, host_ns(p) + p->tune.src_poll_ns);
}

static void src_hold_poll(Tc4xPacer *p)
{
    int64_t now = host_ns(p);

    if (pending(p, p->cfg.hold_src) &&
        now - p->src_hold_host_ns < p->tune.hold_max_ns) {
        timer_set(p, TC4X_PACER_TIMER_HOLD, now + p->tune.src_poll_ns);
        return;
    }
    src_hold_end(p);
}

/*
 * The acknowledge of a held tick releases it, and a tick that waits for it
 * goes out at once rather than at the next poll.  The acknowledge of
 * hold_src ends a tc4x_pacer_hold() hold.
 */
void tc4x_pacer_acked(Tc4xPacer *p, uint32_t src)
{
    if (src == p->cfg.hold_src && p->src_hold) {
        src_hold_end(p);
        return;
    }
    if (!is_tick_src(p, src) || !p->tick_hold || tick_owed(p)) {
        return;
    }
    p->tick_hold = false;
    apply_scale(p);
    schedule(p);
}

/* Half a period after a raise: a tick the core has not taken holds */
static void check(Tc4xPacer *p)
{
    p->check_due = 0;
    if (!p->tick_hold && tick_owed(p)) {
        tick_hold_start(p);
    }
}

void tc4x_pacer_urgent(Tc4xPacer *p)
{
    if (!p->adaptive || !p->driven ||
        p->stats.ticks - p->last_bump_tick < p->tune.urgent_ticks) {
        return;
    }
    p->last_bump_tick = p->stats.ticks;
    p->live_scale = MIN(p->live_scale + p->live_scale / 2 + 1,
                        p->tune.max_scale);
    p->window_ticks = 0;
    p->window_holds = 0;
    p->window_task = 0;
    apply_scale(p);
}

void tc4x_pacer_sample(Tc4xPacer *p)
{
    bool held = p->tick_was_held;
    uint32_t next = p->live_scale;

    p->tick_was_held = false;
    if (!p->adaptive || !p->driven) {
        return;
    }
    p->window_holds += held;
    p->window_task += p->host->cpu_prio(p->host_opaque, p->cfg.core) == 0;
    if (++p->window_ticks < p->tune.window) {
        return;
    }
    if (p->window_holds > p->tune.holds_high ||
        p->window_task < p->tune.task_low) {
        next = MIN(p->live_scale + p->live_scale / 4 + 1, p->tune.max_scale);
    } else if (p->window_holds < p->tune.holds_low &&
               p->window_task > p->tune.task_high) {
        next = MAX(p->live_scale - p->live_scale / 10, p->floor_scale);
    }
    p->stats.task_share = p->window_task * 1000 / p->tune.window;
    p->window_ticks = 0;
    p->window_holds = 0;
    p->window_task = 0;
    if (next != p->live_scale) {
        p->live_scale = next;
        apply_scale(p);
    }
}

void tc4x_pacer_raise(Tc4xPacer *p, uint32_t src)
{
    p->host->raise(p->host_opaque, src);
    p->check_due = p->tick_now + p->cfg.check_ns;
}

static void set_driven(Tc4xPacer *p, bool driven)
{
    p->driven = driven;
    if (p->floor_scale <= 1) {
        return;
    }
    if (driven) {
        p->idle_scale = p->host->requested_scale(p->host_opaque);
        p->live_scale = MAX(p->live_scale, p->floor_scale);
        apply_scale(p);
    } else {
        p->host->request_scale(p->host_opaque, p->idle_scale);
    }
}

static void count_late(Tc4xPacer *p, int64_t due, int64_t now,
                       int64_t host_now)
{
    static const int64_t edges_us[] = { 10, 30, 100, 300, 1000 };
    int64_t late_us;
    unsigned b = 0;

    if (!due) {
        return;
    }
    if (now - due > p->cfg.period_ns / 2) {
        p->stats.late++;
    }
    if (!p->driven) {
        return;
    }
    late_us = (host_now - host_ns_for_guest(p, due)) / 1000;
    while (b < ARRAY_SIZE(edges_us) && late_us >= edges_us[b]) {
        b++;
    }
    p->stats.late_hist[b]++;
}

static void tick(Tc4xPacer *p)
{
    int64_t host_now = host_ns(p);
    int64_t now = guest_ns(p);
    int64_t due;
    bool driven;

    /*
     * A tick that comes due while the core still owes the last one waits
     * for it instead of merging: guest time is held anyway, so this costs
     * only host time and rides out a descheduled vCPU thread.  A core stuck
     * with interrupts masked merges after hold_max_ns.  This also covers a
     * tick raised late, whose half-period check falls after the next slot.
     */
    if (tick_owed(p) &&
        (!p->tick_hold || host_now - p->hold_host_ns < p->tune.hold_max_ns)) {
        if (!p->tick_hold) {
            tick_hold_start(p);
        }
        /* held ticks slow guest time, never stop it (masked busy-waits) */
        barrier(p, INT64_MAX);
        timer_set(p, TC4X_PACER_TIMER_TICK, host_now + p->tune.tick_poll_ns);
        return;
    }
    if (p->tick_hold && !tick_owed(p)) {
        p->tick_hold = false;       /* request disabled meanwhile: no ack */
        apply_scale(p);
    }
    /*
     * Move the barrier to the next slot before the work below: guest time
     * runs on while board code steps and raises interrupts, and a barrier
     * set after that work could lie behind it.
     */
    due = p->tick_due;
    advance(p, now);
    if (p->driven) {
        barrier(p, p->tick_due);
    }
    p->stats.ticks++;
    p->ops->step(p->opaque, now);
    driven = p->ops->driven(p->opaque);
    if (driven != p->driven) {
        set_driven(p, driven);
    }
    for (unsigned i = 0; i < p->cfg.n_srcs; i++) {
        if (pending(p, p->cfg.srcs[i])) {
            /* still not taken despite the hold: merged */
            p->stats.merged++;
            if (p->tick_hold) {
                p->stats.merged_timeout++;
            } else {
                p->stats.merged_unchecked++;
            }
        }
    }
    count_late(p, due, now, host_now);
    p->tick_now = now;
    p->ops->raise(p->opaque);
    schedule(p);
    /* guest time waits at the next tick's slot until it is raised */
    barrier(p, p->driven ? p->tick_due : INT64_MAX);
}

void tc4x_pacer_timer(Tc4xPacer *p, Tc4xPacerTimer t)
{
    switch (t) {
    case TC4X_PACER_TIMER_TICK:
        tick(p);
        break;
    case TC4X_PACER_TIMER_CHECK:
        check(p);
        break;
    case TC4X_PACER_TIMER_HOLD:
        src_hold_poll(p);
        break;
    default:
        g_assert_not_reached();
    }
}

void tc4x_pacer_retime(Tc4xPacer *p)
{
    schedule(p);
}

void tc4x_pacer_set_enabled(Tc4xPacer *p, bool enabled)
{
    p->enabled = enabled;
    if (!enabled) {
        p->tick_hold = false;
        p->src_hold = false;
        timer_clear(p, TC4X_PACER_TIMER_HOLD);
        barrier(p, INT64_MAX);
        if (p->driven) {
            p->driven = false;
            if (p->floor_scale > 1) {
                p->host->request_scale(p->host_opaque, p->idle_scale);
            }
        }
        p->tick_due = 0;
        p->check_due = 0;
    }
    schedule(p);
}

void tc4x_pacer_set_scale(Tc4xPacer *p, uint32_t scale, bool adaptive,
                          double headroom)
{
    p->adaptive = adaptive;
    p->floor_scale = MAX(1, scale);
    p->live_scale = p->floor_scale;
    p->headroom = MAX(1.0, headroom);
}

void tc4x_pacer_stats(Tc4xPacer *p, Tc4xPacerStats *stats)
{
    *stats = p->stats;
    stats->driven = p->driven;
    stats->adaptive = p->adaptive;
    stats->scale = driven_scale(p);
}

static void tune_defaults(Tc4xPacerTuning *t, const Tc4xPacerConfig *cfg)
{
    t->hold_max_ns = t->hold_max_ns ?: 1000000000;     /* 1 s */
    t->tick_poll_ns = t->tick_poll_ns ?: cfg->period_ns;
    t->src_poll_ns = t->src_poll_ns ?: cfg->period_ns;
    t->src_hold_scale = t->src_hold_scale ?: cfg->hold_scale;
    t->max_scale = t->max_scale ?: cfg->hold_scale;
    t->window = t->window ?: 100;
    t->holds_high = t->holds_high ?: t->window / 4;
    t->holds_low = t->holds_low ?: t->window / 10;
    t->task_low = t->task_low ?: t->window / 4;
    t->task_high = t->task_high ?: t->window / 2;
    t->urgent_ticks = t->urgent_ticks ?: t->window;
}

Tc4xPacer *tc4x_pacer_create(const Tc4xPacerHost *host, void *host_opaque,
                             const Tc4xPacerConfig *cfg,
                             const Tc4xPacerOps *ops, void *opaque)
{
    Tc4xPacer *p = g_new0(Tc4xPacer, 1);

    assert(cfg->n_srcs <= TC4X_PACER_SRCS_MAX);
    p->host = host;
    p->host_opaque = host_opaque;
    p->cfg = *cfg;
    p->tune = cfg->tune;
    tune_defaults(&p->tune, cfg);
    p->ops = ops;
    p->opaque = opaque;
    p->floor_scale = 1;
    p->live_scale = 1;
    p->headroom = 1.0;
    return p;
}
