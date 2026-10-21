/*
 * Unit tests for the tc4x periodic interrupt pacer (hw/tricore/tc4x_pacer.c)
 *
 * The pacer runs against a fake host: host and guest time are the same
 * clock (guest time stops at the barrier), timers are deadlines the test
 * fires in order, and the paced core takes a raised interrupt only when the
 * test acknowledges it.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/tricore/tc4x_pacer.h"

#define PERIOD_NS       50000
#define IDLE_PERIOD_NS  (10 * PERIOD_NS)
#define CHECK_NS        (PERIOD_NS / 2)
#define HOLD_SCALE      100u
#define WINDOW          100         /* the default adaptation window */
#define SRC_A           0x100u
#define SRC_B           0x104u
#define SRC_HOLD        0x200u
#define NO_TIMER        INT64_MIN

typedef struct Fake {
    Tc4xPacer *p;
    int64_t host;
    int64_t barrier;
    uint32_t scale;
    uint32_t cpu_prio;
    bool pending[3];            /* SRC_A, SRC_B, SRC_HOLD */
    unsigned raises[3];
    int64_t timer[TC4X_PACER_TIMERS];
    bool driven;
    bool take_at_once;
    bool sample;
    unsigned steps;
    int64_t stepped_to;
} Fake;

static int src_index(uint32_t src)
{
    switch (src) {
    case SRC_A:
        return 0;
    case SRC_B:
        return 1;
    case SRC_HOLD:
        return 2;
    }
    g_assert_not_reached();
}

static int64_t fake_guest_ns(void *opaque)
{
    Fake *f = opaque;

    return MIN(f->host, f->barrier);
}

static int64_t fake_host_ns(void *opaque)
{
    Fake *f = opaque;

    return f->host;
}

static int64_t fake_host_ns_for_guest(void *opaque, int64_t ns)
{
    return ns;
}

static void fake_barrier(void *opaque, int64_t ns)
{
    Fake *f = opaque;

    f->barrier = ns;
}

static void fake_request_scale(void *opaque, uint32_t scale)
{
    Fake *f = opaque;

    f->scale = scale;
}

static uint32_t fake_requested_scale(void *opaque)
{
    Fake *f = opaque;

    return f->scale;
}

static bool fake_pending(void *opaque, uint32_t src)
{
    Fake *f = opaque;

    return f->pending[src_index(src)];
}

static void fake_raise(void *opaque, uint32_t src)
{
    Fake *f = opaque;

    f->raises[src_index(src)]++;
    f->pending[src_index(src)] = !f->take_at_once;
}

static uint32_t fake_cpu_prio(void *opaque, unsigned core)
{
    Fake *f = opaque;

    g_assert_cmpuint(core, ==, 0);
    return f->cpu_prio;
}

static void fake_timer_mod(void *opaque, Tc4xPacerTimer t, int64_t ns)
{
    Fake *f = opaque;

    f->timer[t] = ns;
}

static void fake_timer_del(void *opaque, Tc4xPacerTimer t)
{
    Fake *f = opaque;

    f->timer[t] = NO_TIMER;
}

static const Tc4xPacerHost fake_host = {
    .guest_ns = fake_guest_ns,
    .host_ns = fake_host_ns,
    .host_ns_for_guest = fake_host_ns_for_guest,
    .barrier = fake_barrier,
    .request_scale = fake_request_scale,
    .requested_scale = fake_requested_scale,
    .pending = fake_pending,
    .raise = fake_raise,
    .cpu_prio = fake_cpu_prio,
    .timer_mod = fake_timer_mod,
    .timer_del = fake_timer_del,
};

static void model_step(void *opaque, int64_t now)
{
    Fake *f = opaque;

    f->steps++;
    f->stepped_to = now;
}

static bool model_driven(void *opaque)
{
    Fake *f = opaque;

    return f->driven;
}

static void model_raise(void *opaque)
{
    Fake *f = opaque;

    tc4x_pacer_raise(f->p, SRC_A);
    if (f->sample) {
        tc4x_pacer_sample(f->p);
    }
    tc4x_pacer_raise(f->p, SRC_B);
}

static const Tc4xPacerOps model_ops = {
    .step = model_step,
    .driven = model_driven,
    .raise = model_raise,
};

static const Tc4xPacerConfig config = {
    .period_ns = PERIOD_NS,
    .idle_period_ns = IDLE_PERIOD_NS,
    .check_ns = CHECK_NS,
    .hold_scale = HOLD_SCALE,
    .core = 0,
    .srcs = { SRC_A, SRC_B },
    .n_srcs = 2,
    .hold_src = SRC_HOLD,
};

static Fake *fake_new(void)
{
    Fake *f = g_new0(Fake, 1);

    f->barrier = INT64_MAX;
    f->scale = 1;
    f->driven = true;
    f->take_at_once = true;
    for (int t = 0; t < TC4X_PACER_TIMERS; t++) {
        f->timer[t] = NO_TIMER;
    }
    f->p = tc4x_pacer_create(&fake_host, f, &config, &model_ops, f);
    tc4x_pacer_set_enabled(f->p, true);
    return f;
}

static void fake_free(Fake *f)
{
    g_free(f->p);
    g_free(f);
}

static Tc4xPacerTimer fire_next(Fake *f)
{
    Tc4xPacerTimer next = TC4X_PACER_TIMERS;

    for (int t = 0; t < TC4X_PACER_TIMERS; t++) {
        if (f->timer[t] != NO_TIMER &&
            (next == TC4X_PACER_TIMERS || f->timer[t] < f->timer[next])) {
            next = t;
        }
    }
    g_assert_cmpint(next, !=, TC4X_PACER_TIMERS);
    f->host = MAX(f->host, f->timer[next]);
    f->timer[next] = NO_TIMER;
    tc4x_pacer_timer(f->p, next);
    return next;
}

static void fire_until(Fake *f, Tc4xPacerTimer t)
{
    while (fire_next(f) != t) {
        /* fire the timers due before it */
    }
}

static void ack(Fake *f, uint32_t src)
{
    f->pending[src_index(src)] = false;
    tc4x_pacer_acked(f->p, src);
}

static Tc4xPacerStats stats(Fake *f)
{
    Tc4xPacerStats st;

    tc4x_pacer_stats(f->p, &st);
    return st;
}

/*
 * The first tick finds the fake driven: the pacer ticked at the idle
 * period until then, so the driven grid starts at T0
 */
#define T0              (PERIOD_NS + IDLE_PERIOD_NS)

static void first_tick(Fake *f)
{
    fire_until(f, TC4X_PACER_TIMER_TICK);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], ==, T0);
}

/* Ticks keep an exact grid and guest time waits at the next slot */
static void test_grid(void)
{
    Fake *f = fake_new();

    first_tick(f);
    for (int k = 0; k < 5; k++) {
        g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], ==,
                        T0 + k * PERIOD_NS);
        fire_until(f, TC4X_PACER_TIMER_TICK);
        g_assert_cmpint(f->stepped_to, ==, T0 + k * PERIOD_NS);
        g_assert_cmpint(f->barrier, ==, T0 + (k + 1) * PERIOD_NS);
    }
    g_assert_cmpuint(f->raises[0], ==, 6);
    g_assert_cmpuint(f->raises[1], ==, 6);
    g_assert_cmpint(stats(f).merged, ==, 0);
    fake_free(f);
}

/* A tick still pending half a period later holds guest time at hold_scale */
static void test_check_holds(void)
{
    Fake *f = fake_new();

    first_tick(f);
    f->take_at_once = false;
    fire_until(f, TC4X_PACER_TIMER_TICK);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_CHECK], ==, T0 + CHECK_NS);
    fire_until(f, TC4X_PACER_TIMER_CHECK);
    g_assert_cmpint(stats(f).holds, ==, 1);
    g_assert_cmpuint(f->scale, ==, HOLD_SCALE);

    ack(f, SRC_A);
    g_assert_cmpuint(f->scale, ==, HOLD_SCALE);     /* SRC_B still owed */
    ack(f, SRC_B);
    g_assert_cmpuint(f->scale, ==, 1);
    g_assert_cmpint(stats(f).merged, ==, 0);
    fake_free(f);
}

/* A tick due while the last one is owed waits for the acknowledge */
static void test_never_merges(void)
{
    Fake *f = fake_new();

    first_tick(f);
    f->take_at_once = false;
    fire_until(f, TC4X_PACER_TIMER_TICK);
    fire_until(f, TC4X_PACER_TIMER_CHECK);
    fire_until(f, TC4X_PACER_TIMER_TICK);   /* due at T0 + period: held */
    g_assert_cmpuint(f->raises[0], ==, 2);
    g_assert_cmpint(f->barrier, ==, INT64_MAX);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], >, f->host);

    ack(f, SRC_A);
    ack(f, SRC_B);
    /* the waiting tick goes out at its slot, now due */
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], ==, T0 + PERIOD_NS);
    fire_until(f, TC4X_PACER_TIMER_TICK);
    g_assert_cmpuint(f->raises[0], ==, 3);
    g_assert_cmpint(stats(f).merged, ==, 0);
    fake_free(f);
}

/* A core that never takes the tick merges it after a second of host time */
static void test_merges_after_timeout(void)
{
    Fake *f = fake_new();

    first_tick(f);
    f->take_at_once = false;
    fire_until(f, TC4X_PACER_TIMER_TICK);
    while (f->raises[0] == 2) {
        fire_next(f);
    }
    g_assert_cmpint(f->host, >=, T0 + NANOSECONDS_PER_SECOND);
    g_assert_cmpint(stats(f).merged, ==, 2);
    g_assert_cmpint(stats(f).merged_timeout, ==, 2);
    fake_free(f);
}

/* A tick over a period late restarts the grid from its time */
static void test_late_tick_restarts_grid(void)
{
    Fake *f = fake_new();

    f->driven = false;      /* no barrier: guest time runs past the slot */
    fire_until(f, TC4X_PACER_TIMER_TICK);
    f->host = T0 + 5 * IDLE_PERIOD_NS + 1234;
    fire_until(f, TC4X_PACER_TIMER_TICK);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], ==,
                    f->host + IDLE_PERIOD_NS);
    g_assert_cmpint(stats(f).late, ==, 1);
    fake_free(f);
}

/* An idle model ticks at idle_period_ns without a barrier */
static void test_idle(void)
{
    Fake *f = fake_new();

    f->driven = false;
    fire_until(f, TC4X_PACER_TIMER_TICK);
    fire_until(f, TC4X_PACER_TIMER_TICK);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], ==,
                    PERIOD_NS + 2 * IDLE_PERIOD_NS);
    g_assert_cmpint(f->barrier, ==, INT64_MAX);
    fake_free(f);
}

/* The adaptive scale rises with the hold rate and falls without holds */
static void test_adaptive_scale(void)
{
    Fake *f = fake_new();

    tc4x_pacer_set_scale(f->p, 10, true, 1.0);
    f->sample = true;
    first_tick(f);
    g_assert_cmpuint(stats(f).scale, ==, 10);
    g_assert_cmpuint(f->scale, ==, 10);
    /* every tick held: the window rises by a quarter */
    f->take_at_once = false;
    for (int k = 0; k < WINDOW; k++) {
        fire_until(f, TC4X_PACER_TIMER_TICK);
        fire_until(f, TC4X_PACER_TIMER_CHECK);
        ack(f, SRC_A);
        ack(f, SRC_B);
    }
    g_assert_cmpuint(stats(f).scale, ==, 13);
    /* no holds, the core at task level: the window falls by a tenth */
    f->take_at_once = true;
    for (int k = 0; k < WINDOW; k++) {
        fire_until(f, TC4X_PACER_TIMER_TICK);
    }
    g_assert_cmpuint(stats(f).scale, ==, 12);
    g_assert_cmpint(stats(f).task_share, ==, 1000);
    fake_free(f);
}

/* tc4x_pacer_hold() slows guest time until hold_src is taken */
static void test_src_hold(void)
{
    Fake *f = fake_new();

    tc4x_pacer_set_scale(f->p, 1, true, 1.0);
    f->pending[2] = true;
    tc4x_pacer_hold(f->p);
    g_assert_cmpuint(f->scale, ==, HOLD_SCALE);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_HOLD], >, f->host);
    ack(f, SRC_HOLD);
    g_assert_cmpuint(f->scale, ==, 1);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_HOLD], ==, NO_TIMER);
    g_assert_cmpint(stats(f).src_holds, ==, 1);
    fake_free(f);
}

/* A stopped pacer releases guest time */
static void test_disable_releases(void)
{
    Fake *f = fake_new();

    tc4x_pacer_set_scale(f->p, 10, false, 1.0);
    first_tick(f);
    g_assert_cmpint(f->barrier, ==, T0);
    g_assert_cmpuint(f->scale, ==, 10);
    tc4x_pacer_set_enabled(f->p, false);
    g_assert_cmpint(f->barrier, ==, INT64_MAX);
    g_assert_cmpuint(f->scale, ==, 1);
    g_assert_cmpint(f->timer[TC4X_PACER_TIMER_TICK], ==, NO_TIMER);
    fake_free(f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tc4x-pacer/grid", test_grid);
    g_test_add_func("/tc4x-pacer/check-holds", test_check_holds);
    g_test_add_func("/tc4x-pacer/never-merges", test_never_merges);
    g_test_add_func("/tc4x-pacer/merges-after-timeout",
                    test_merges_after_timeout);
    g_test_add_func("/tc4x-pacer/late-tick-restarts-grid",
                    test_late_tick_restarts_grid);
    g_test_add_func("/tc4x-pacer/idle", test_idle);
    g_test_add_func("/tc4x-pacer/adaptive-scale", test_adaptive_scale);
    g_test_add_func("/tc4x-pacer/src-hold", test_src_hold);
    g_test_add_func("/tc4x-pacer/disable-releases", test_disable_releases);
    return g_test_run();
}
