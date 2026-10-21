/*
 * TC4x TMADC conversion results from simulated analog inputs.
 *
 * Reading AWx_RESk returns the 12-bit right-aligned conversion of the
 * enabled channel whose CHn_CFG.RSEL selects result k.  While a kernel runs,
 * a periodic guest-time timer per service request line raises SRi when its
 * SRi_CFG.EVEN enables any result.  Board code sets the pin voltages
 * (0..VREF) or supplies a channel's voltage at each conversion.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "tc4x_soc.h"

#define TMADC_CHANNELS      16u
#define TMADC_CH_CFG(n)     (0x01Cu + 0xCu * (n))
#define TMADC_SR_CFG(i)     (0x1B0u + 4u * (i))
#define TMADC_SR_COUNT      6u
#define TMADC_RES0          0x240u
#define TMADC_RES_END       0x280u
#define TMADC_RESF          0x194u
#define CH_CFG_RSEL_SHIFT   16
#define CH_CFG_EN           BIT(21)
#define SR_CFG_EVEN_MASK    0x3FFFFu
#define RES_CHNR_SHIFT      16
#define RES_DR              BIT(30)

#define SRC_TMADC_BASE      0xF4432E10u
#define SRC_TMADC_STRIDE    0x18u

#define ADC_VREF            3.3            /* V */
#define ADC_FULL_SCALE      4096.0
#define ADC_MAX_COUNT       4095
/*
 * Conversion rounds are paced far below silicon rates, since every round
 * raises the result interrupts.  Board code that triggers conversions
 * itself turns the periodic rounds of that line off (tc4x_adc_set_period).
 */
#define ADC_DEFAULT_PERIOD_NS 10000000

typedef struct AdcInput {
    double volts;
    Tc4xAdcInputFn fn;              /* NULL: volts */
    void *opaque;
} AdcInput;

/* Conversion trigger of one service request line (SRi of a kernel) */
typedef struct AdcTrigger {
    struct Tc4xAdc *adc;
    unsigned n, sr;
    QEMUTimer *timer;
    int64_t period_ns;
    int64_t due_ns;         /* guest time of the next round (0: not armed) */
} AdcTrigger;

struct Tc4xAdc {
    Tc4xSoc *soc;
    AdcInput in[TMADC_COUNT][TMADC_CHANNELS];
    AdcTrigger trig[TMADC_COUNT][TMADC_SR_COUNT];
};

static uint32_t kreg(unsigned n, uint32_t off)
{
    return TMADC_BASE + n * TMADC_STRIDE + off;
}

static bool kernel_running(Tc4xAdc *a, unsigned n)
{
    return (tc4x_reg_get(a->soc, ADC_CLKEN_TMADC) & BIT(n)) &&
           (tc4x_reg_get(a->soc, kreg(n, TMADC_MODCFG)) & TMADC_RUN);
}

static double adc_volts(Tc4xAdc *a, unsigned n, unsigned ch)
{
    AdcInput *in = &a->in[n % TMADC_COUNT][ch % TMADC_CHANNELS];

    return in->fn ? CLAMP(in->fn(in->opaque, n, ch), 0, ADC_VREF) : in->volts;
}

double tc4x_adc_volts(Tc4xSoc *soc, unsigned kernel, unsigned ch)
{
    return adc_volts(tc4x_soc_adc(soc), kernel, ch);
}

void tc4x_adc_set_volts(Tc4xSoc *soc, unsigned kernel, unsigned ch,
                        double volts)
{
    Tc4xAdc *a = tc4x_soc_adc(soc);

    a->in[kernel % TMADC_COUNT][ch % TMADC_CHANNELS].volts =
        CLAMP(volts, 0, ADC_VREF);
}

void tc4x_adc_set_input(Tc4xSoc *soc, unsigned kernel, unsigned ch,
                        Tc4xAdcInputFn fn, void *opaque)
{
    AdcInput *in = &tc4x_soc_adc(soc)->in[kernel % TMADC_COUNT]
                                         [ch % TMADC_CHANNELS];

    in->fn = fn;
    in->opaque = opaque;
}

bool tc4x_adc_sr_enabled(Tc4xSoc *soc, unsigned kernel, unsigned sr)
{
    Tc4xAdc *a = tc4x_soc_adc(soc);

    return kernel_running(a, kernel) &&
           (tc4x_reg_get(soc, kreg(kernel, TMADC_SR_CFG(sr))) &
            SR_CFG_EVEN_MASK);
}

static uint32_t adc_counts(Tc4xAdc *a, double volts)
{
    double c = volts * ADC_FULL_SCALE / ADC_VREF;

    return (uint32_t)CLAMP(c + 0.5, 0, ADC_MAX_COUNT);
}

static uint32_t adc_result(Tc4xAdc *a, unsigned n, unsigned k)
{
    for (unsigned ch = 0; ch < TMADC_CHANNELS; ch++) {
        uint32_t cfg = tc4x_reg_get(a->soc, kreg(n, TMADC_CH_CFG(ch)));

        if ((cfg & CH_CFG_EN) && extract32(cfg, CH_CFG_RSEL_SHIFT, 4) == k) {
            return adc_counts(a, adc_volts(a, n, ch)) |
                   (ch << RES_CHNR_SHIFT) | RES_DR;
        }
    }
    return 0;
}

/* Rounds are due at absolute guest times */
static void adc_arm(AdcTrigger *t)
{
    timer_mod(t->timer, tc4x_host_ns_for_guest(t->adc->soc, t->due_ns));
}

/* Next round one period after the last one (or from now) */
static void adc_schedule(AdcTrigger *t)
{
    int64_t now = tc4x_guest_ns(t->adc->soc);

    if (!t->period_ns) {
        timer_del(t->timer);
        t->due_ns = 0;
        return;
    }
    t->due_ns = t->due_ns ? t->due_ns + t->period_ns : now + t->period_ns;
    if (t->due_ns <= now) {
        t->due_ns = now + t->period_ns;
    }
    adc_arm(t);
}

/* One conversion round: the service request sees new results */
static void adc_tick(void *opaque)
{
    AdcTrigger *t = opaque;
    Tc4xAdc *a = t->adc;

    if (kernel_running(a, t->n)) {
        uint32_t even = tc4x_reg_get(a->soc, kreg(t->n, TMADC_SR_CFG(t->sr))) &
                        SR_CFG_EVEN_MASK;

        if (even) {
            tc4x_reg_set(a->soc, kreg(t->n, TMADC_RESF),
                    tc4x_reg_get(a->soc, kreg(t->n, TMADC_RESF)) | even);
            tc4x_irq_raise(a->soc, SRC_TMADC_BASE + t->n * SRC_TMADC_STRIDE +
                            4 * t->sr);
        }
    }
    adc_schedule(t);
}

static void adc_start(Tc4xAdc *a, unsigned n)
{
    for (unsigned i = 0; i < TMADC_SR_COUNT; i++) {
        if (!timer_pending(a->trig[n][i].timer)) {
            adc_schedule(&a->trig[n][i]);
        }
    }
}

bool tc4x_adc_read(Tc4xAdc *a, uint32_t addr, uint32_t *val)
{
    uint32_t off;
    unsigned n;

    if (addr < TMADC_BASE || addr >= TMADC_BASE + TMADC_COUNT * TMADC_STRIDE) {
        return false;
    }
    n = (addr - TMADC_BASE) / TMADC_STRIDE;
    off = (addr - TMADC_BASE) % TMADC_STRIDE;
    if (off >= TMADC_RES0 && off < TMADC_RES_END) {
        *val = adc_result(a, n, (off - TMADC_RES0) / 4);
        return true;
    }
    return false;
}

bool tc4x_adc_write(Tc4xAdc *a, uint32_t addr, uint32_t val)
{
    uint32_t off;
    unsigned n;

    if (addr < TMADC_BASE || addr >= TMADC_BASE + TMADC_COUNT * TMADC_STRIDE) {
        return false;
    }
    n = (addr - TMADC_BASE) / TMADC_STRIDE;
    off = (addr - TMADC_BASE) % TMADC_STRIDE;
    if (off == TMADC_MODCFG && (val & TMADC_RUN)) {
        adc_start(a, n);
    }
    return false;   /* the register file stores the value */
}

void tc4x_adc_set_period(Tc4xSoc *soc, unsigned kernel, unsigned sr,
                         int64_t period_ns)
{
    AdcTrigger *t = &tc4x_soc_adc(soc)->trig[kernel % TMADC_COUNT]
                                            [sr % TMADC_SR_COUNT];

    t->period_ns = period_ns;
    t->due_ns = 0;
    adc_schedule(t);
}

static bool adc_command(void *opaque, int argc, char **argv, GString *out)
{
    Tc4xAdc *a = opaque;
    unsigned long n, ch;

    if (strcmp(argv[0], "adc") != 0 || argc < 4 ||
        qemu_strtoul(argv[1], NULL, 0, &n) < 0 ||
        qemu_strtoul(argv[2], NULL, 0, &ch) < 0) {
        return false;
    }
    if (n >= TMADC_COUNT || ch >= TMADC_CHANNELS) {
        g_string_append(out, "err usage: adc K CH VOLTS\n");
        return true;
    }
    tc4x_adc_set_volts(a->soc, n, ch, g_ascii_strtod(argv[3], NULL));
    g_string_append(out, "ok\n");
    return true;
}

Tc4xAdc *tc4x_adc_new(Tc4xSoc *soc)
{
    Tc4xAdc *a = g_new0(Tc4xAdc, 1);

    a->soc = soc;
    for (unsigned n = 0; n < TMADC_COUNT; n++) {
        for (unsigned i = 0; i < TMADC_SR_COUNT; i++) {
            AdcTrigger *t = &a->trig[n][i];

            t->adc = a;
            t->n = n;
            t->sr = i;
            t->period_ns = ADC_DEFAULT_PERIOD_NS;
            t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adc_tick, t);
        }
    }
    tc4x_sim_add_command(soc, "adc", adc_command, a, false);
    return a;
}
