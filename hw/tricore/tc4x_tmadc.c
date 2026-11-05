/*
 * TC4x TMADC kernels (TMADC0..7).
 *
 * Reading AW0_RESk returns the 12-bit right-aligned conversion of the
 * enabled channel whose CHn_CFG.RSEL selects result k.  MODSTAT follows
 * the kernel's clock enable and MODCFG.RUN; the power-on calibration
 * completes at once (CALPH reads "completed" as soon as the clock runs,
 * which IfxAdc_Tmadc_runModule() of the iLLD waits for).
 *
 * Register layout: Ifx_ADC_TMADC_* of the iLLD SFR headers (tc4x_regs.h).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "hw/tricore/tc4x_timebase.h"
#include "hw/tricore/tc4x_tmadc.h"
#include "tc4x_soc.h"
#include "trace.h"

#define TMADC_STRIDE        (A_ADC_TMADC1_MODCFG - A_ADC_TMADC0_MODCFG)
#define TMADC_REG(r)        (A_ADC_TMADC0_##r - A_ADC_TMADC0_MODCFG)
#define TMADC_MODCFG        TMADC_REG(MODCFG)
#define TMADC_MODSTAT       TMADC_REG(MODSTAT)
#define TMADC_RESF          TMADC_REG(RESF)
#define TMADC_CH_STRIDE     (A_ADC_TMADC0_CH1_CFG - A_ADC_TMADC0_CH0_CFG)
#define TMADC_SR_STRIDE     (A_ADC_TMADC0_SR1_CFG - A_ADC_TMADC0_SR0_CFG)
#define TMADC_RES_STRIDE    (A_ADC_TMADC0_AW0_RES1 - A_ADC_TMADC0_AW0_RES0)
#define TMADC_CH_CFG(n)     (TMADC_REG(CH0_CFG) + (n) * TMADC_CH_STRIDE)
#define TMADC_SR_CFG(i)     (TMADC_REG(SR0_CFG) + (i) * TMADC_SR_STRIDE)
#define TMADC_RES(k)        (TMADC_REG(AW0_RES0) + (k) * TMADC_RES_STRIDE)
#define TMADC_RES_END       TMADC_RES(TC4X_TMADC_CHANNELS)
#define MODSTAT_CALPH_DONE  2u

#define TMADC_FULL_SCALE    4096.0
#define TMADC_MAX_COUNT     4095u

/*
 * Conversion rounds are paced far below silicon rates, since every round
 * raises the result interrupts
 */
#define TMADC_DEFAULT_PERIOD_NS 10000000

/* What one channel converts */
typedef struct Tc4xTmadcInput {
    uint64_t volts;                 /* the "volts" property, double bits */
    Tc4xAnalogSource *src;          /* NULL: volts */
    double (*src_volts)(Tc4xAnalogSource *src, unsigned line);
    unsigned line;
} Tc4xTmadcInput;

/* The conversion rounds of one service request line (SRi of a kernel) */
typedef struct Tc4xTmadcTrigger {
    struct Tc4xTmadc *adc;
    unsigned k, sr;
    Tc4xGuestTimer timer;
    uint64_t period_ns;
    int64_t due_ns;         /* guest time of the next round (0: not armed) */
} Tc4xTmadcTrigger;

/*
 * Locking: in[].volts is atomic; the sources are connected during machine
 * init; the triggers change under the BQL.
 */
struct Tc4xTmadc {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    Tc4xSfrStore *sfr;
    Tc4xTimebase *timebase;
    bool imprecise_io;
    qemu_irq sr[TC4X_TMADC_COUNT * TC4X_TMADC_SR_COUNT];

    Tc4xTmadcInput in[TC4X_TMADC_COUNT][TC4X_TMADC_CHANNELS];
    Tc4xTmadcTrigger trig[TC4X_TMADC_COUNT][TC4X_TMADC_SR_COUNT];
};

/*
 * The analog source of the transitional Tc4xAdcInputFn (tc4x.h): output
 * k * TC4X_TMADC_CHANNELS + ch is channel ch of kernel k
 */
#define TYPE_TC4X_ANALOG_LEGACY "tc4x-analog-legacy"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xAnalogLegacy, TC4X_ANALOG_LEGACY)

struct Tc4xAnalogLegacy {
    Object parent_obj;

    Tc4xAdcInputFn fn;
    void *opaque;
};

uint32_t tc4x_tmadc_base(void)
{
    return A_ADC_TMADC0_MODCFG;
}

uint32_t tc4x_tmadc_window(void)
{
    return TC4X_TMADC_COUNT * TMADC_STRIDE;
}

static uint32_t kreg_get(Tc4xTmadc *adc, unsigned k, uint32_t off)
{
    return tc4x_sfr_store_get(adc->sfr, A_ADC_TMADC0_MODCFG +
                              k * TMADC_STRIDE + off);
}

static bool kernel_clocked(Tc4xTmadc *adc, unsigned k)
{
    return tc4x_sfr_store_get(adc->sfr, A_ADC_CLKEN_TMADC) & BIT(k);
}

static bool kernel_running(Tc4xTmadc *adc, unsigned k)
{
    return kernel_clocked(adc, k) &&
           FIELD_EX32(kreg_get(adc, k, TMADC_MODCFG), ADC_TMADC_MODCFG, RUN);
}

static double input_volts(Tc4xTmadcInput *in)
{
    uint64_t bits;
    double v;

    if (in->src) {
        return CLAMP(in->src_volts(in->src, in->line), 0,
                     TC4X_TMADC_VREF);
    }
    bits = qatomic_read(&in->volts);
    memcpy(&v, &bits, sizeof(v));
    return v;
}

double tc4x_tmadc_volts(Tc4xTmadc *adc, unsigned k, unsigned ch)
{
    assert(k < TC4X_TMADC_COUNT && ch < TC4X_TMADC_CHANNELS);
    return input_volts(&adc->in[k][ch]);
}

void tc4x_tmadc_set_volts(Tc4xTmadc *adc, unsigned k, unsigned ch,
                          double volts)
{
    double v = CLAMP(volts, 0, TC4X_TMADC_VREF);
    uint64_t bits;

    assert(k < TC4X_TMADC_COUNT && ch < TC4X_TMADC_CHANNELS);
    memcpy(&bits, &v, sizeof(bits));
    qatomic_set(&adc->in[k][ch].volts, bits);
}

bool tc4x_tmadc_connect(Tc4xTmadc *adc, unsigned k, unsigned ch,
                        Tc4xAnalogSource *src, unsigned line, Error **errp)
{
    Tc4xTmadcInput *in;

    if (k >= TC4X_TMADC_COUNT || ch >= TC4X_TMADC_CHANNELS) {
        error_setg(errp, "tc4x-tmadc: no channel %u of kernel %u", ch, k);
        return false;
    }
    in = &adc->in[k][ch];
    in->src = src;
    in->src_volts = src ? TC4X_ANALOG_SOURCE_GET_CLASS(src)->volts : NULL;
    in->line = line;
    return true;
}

bool tc4x_tmadc_sr_enabled(Tc4xTmadc *adc, unsigned k, unsigned sr)
{
    assert(k < TC4X_TMADC_COUNT && sr < TC4X_TMADC_SR_COUNT);
    return kernel_running(adc, k) &&
           FIELD_EX32(kreg_get(adc, k, TMADC_SR_CFG(sr)), ADC_TMADC_SR_CFG,
                      EVEN);
}

static uint32_t tmadc_counts(double volts)
{
    double c = volts * TMADC_FULL_SCALE / TC4X_TMADC_VREF;

    return (uint32_t)CLAMP(c + 0.5, 0, TMADC_MAX_COUNT);
}

/* Result @r of kernel @k: the enabled channel that selects it, if any */
static uint32_t tmadc_result(Tc4xTmadc *adc, unsigned k, unsigned r)
{
    for (unsigned ch = 0; ch < TC4X_TMADC_CHANNELS; ch++) {
        uint32_t cfg = kreg_get(adc, k, TMADC_CH_CFG(ch));
        uint32_t res = 0;

        if (FIELD_EX32(cfg, ADC_TMADC_CH_CFG, EN) &&
            FIELD_EX32(cfg, ADC_TMADC_CH_CFG, RSEL) == r) {
            res = FIELD_DP32(res, ADC_RES, RESULT,
                             tmadc_counts(input_volts(&adc->in[k][ch])));
            res = FIELD_DP32(res, ADC_RES, CHNR, ch);
            return FIELD_DP32(res, ADC_RES, DR, 1);
        }
    }
    return 0;
}

static uint32_t tmadc_modstat(Tc4xTmadc *adc, unsigned k)
{
    uint32_t v = 0;

    if (!kernel_clocked(adc, k)) {
        return FIELD_DP32(v, ADC_TMADC_MODSTAT, DIS, 1);
    }
    v = FIELD_DP32(v, ADC_TMADC_MODSTAT, CALPH, MODSTAT_CALPH_DONE);
    return FIELD_DP32(v, ADC_TMADC_MODSTAT, RUN, kernel_running(adc, k));
}

/* Next round one period after the last one (or from now) */
static void tmadc_schedule(Tc4xTmadcTrigger *t)
{
    int64_t now = tc4x_timebase_ns(t->adc->timebase);

    if (!t->period_ns) {
        tc4x_guest_timer_del(&t->timer);
        t->due_ns = 0;
        return;
    }
    t->due_ns = t->due_ns ? t->due_ns + t->period_ns : now + t->period_ns;
    if (t->due_ns <= now) {
        t->due_ns = now + t->period_ns;
    }
    tc4x_guest_timer_mod(&t->timer, t->due_ns);
}

/* One conversion round: the service request sees new results */
static void tmadc_round(void *opaque)
{
    Tc4xTmadcTrigger *t = opaque;
    Tc4xTmadc *adc = t->adc;

    if (kernel_running(adc, t->k)) {
        uint32_t even = FIELD_EX32(kreg_get(adc, t->k, TMADC_SR_CFG(t->sr)),
                                   ADC_TMADC_SR_CFG, EVEN);

        if (even) {
            tc4x_sfr_store_or(adc->sfr, A_ADC_TMADC0_MODCFG +
                              t->k * TMADC_STRIDE + TMADC_RESF, even);
            qemu_irq_pulse(adc->sr[t->k * TC4X_TMADC_SR_COUNT + t->sr]);
        }
    }
    tmadc_schedule(t);
}

/* MODCFG.RUN set: the rounds of the kernel's lines start */
static void tmadc_start(Tc4xTmadc *adc, unsigned k)
{
    for (unsigned i = 0; i < TC4X_TMADC_SR_COUNT; i++) {
        if (!tc4x_guest_timer_pending(&adc->trig[k][i].timer)) {
            tmadc_schedule(&adc->trig[k][i]);
        }
    }
}

static MemTxResult tc4x_tmadc_read(void *opaque, hwaddr off, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    Tc4xTmadc *adc = opaque;
    unsigned k = off / TMADC_STRIDE;
    hwaddr reg = off % TMADC_STRIDE & ~3u;
    uint32_t val;

    if (tc4x_master_refused(attrs, "tc4x-tmadc", tc4x_tmadc_base() + off,
                            false)) {
        return MEMTX_ERROR;
    }
    if (reg >= TMADC_RES(0) && reg < TMADC_RES_END) {
        val = tmadc_result(adc, k, (reg - TMADC_RES(0)) / TMADC_RES_STRIDE);
    } else if (reg == TMADC_MODSTAT) {
        val = tmadc_modstat(adc, k);
    } else {
        *data = tc4x_sfr_store_access_read(adc->sfr, tc4x_tmadc_base() + off,
                                           size, attrs);
        return MEMTX_OK;
    }
    *data = extract32(val, (off & 3) * 8, size * 8);
    return MEMTX_OK;
}

/* The store keeps every write; MODCFG.RUN starts the rounds */
static MemTxResult tc4x_tmadc_write(void *opaque, hwaddr off, uint64_t val,
                                    unsigned size, MemTxAttrs attrs)
{
    Tc4xTmadc *adc = opaque;
    unsigned k = off / TMADC_STRIDE;

    if (tc4x_master_refused(attrs, "tc4x-tmadc", tc4x_tmadc_base() + off,
                            true)) {
        return MEMTX_ERROR;
    }
    tc4x_sfr_store_access_write(adc->sfr, tc4x_tmadc_base() + off, val, size,
                                attrs);
    if ((off % TMADC_STRIDE & ~3u) == TMADC_MODCFG &&
        FIELD_EX32(kreg_get(adc, k, TMADC_MODCFG), ADC_TMADC_MODCFG, RUN)) {
        BQL_LOCK_GUARD();
        tmadc_start(adc, k);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps tc4x_tmadc_ops = {
    .read_with_attrs = tc4x_tmadc_read,
    .write_with_attrs = tc4x_tmadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* volts[K][CH] and period-ns[K][SR] */
static void tmadc_get_volts(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    double volts = input_volts(opaque);

    visit_type_number(v, name, &volts, errp);
}

static void tmadc_set_volts(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    Tc4xTmadcInput *in = opaque;
    double volts, clamped;
    uint64_t bits;

    if (!visit_type_number(v, name, &volts, errp)) {
        return;
    }
    if (isnan(volts)) {
        error_setg(errp, "tc4x-tmadc: %s is not a number", name);
        return;
    }
    clamped = CLAMP(volts, 0, TC4X_TMADC_VREF);
    memcpy(&bits, &clamped, sizeof(bits));
    qatomic_set(&in->volts, bits);
}

static void tmadc_get_period(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    Tc4xTmadcTrigger *t = opaque;

    visit_type_uint64(v, name, &t->period_ns, errp);
}

static void tmadc_set_period(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    Tc4xTmadcTrigger *t = opaque;
    uint64_t ns;

    if (!visit_type_uint64(v, name, &ns, errp)) {
        return;
    }
    if (ns > INT64_MAX / 2) {
        error_setg(errp, "tc4x-tmadc: %s %" PRIu64 " ns is too long", name,
                   ns);
        return;
    }
    t->period_ns = ns;
    t->due_ns = 0;
    if (DEVICE(obj)->realized) {
        tmadc_schedule(t);
    }
}

/*
 * adc K CH VOLTS: set the "volts" property of a channel.  Board commands
 * "adc" with a word other than a number are not this one.
 */
static bool tc4x_tmadc_sim_run(Tc4xSimCommands *obj, int argc, char **argv,
                               GString *out)
{
    Tc4xTmadc *adc = TC4X_TMADC(obj);
    unsigned k, ch;
    double volts;

    if (argc > 1 && !g_ascii_isdigit(argv[1][0])) {
        return false;
    }
    if (argc != 4 || qemu_strtoui(argv[1], NULL, 0, &k) < 0 ||
        qemu_strtoui(argv[2], NULL, 0, &ch) < 0 ||
        qemu_strtod(argv[3], NULL, &volts) < 0 || isnan(volts) ||
        k >= TC4X_TMADC_COUNT || ch >= TC4X_TMADC_CHANNELS) {
        g_string_append(out, "err usage: adc K CH VOLTS\n");
        return true;
    }
    tc4x_tmadc_set_volts(adc, k, ch, volts);
    g_string_append(out, "ok\n");
    return true;
}

/* No rounds until a kernel runs again */
static void tc4x_tmadc_reset_hold(Object *obj, ResetType type)
{
    Tc4xTmadc *adc = TC4X_TMADC(obj);

    for (unsigned k = 0; k < TC4X_TMADC_COUNT; k++) {
        for (unsigned i = 0; i < TC4X_TMADC_SR_COUNT; i++) {
            tc4x_guest_timer_del(&adc->trig[k][i].timer);
            adc->trig[k][i].due_ns = 0;
        }
    }
}

static void tc4x_tmadc_init(Object *obj)
{
    Tc4xTmadc *adc = TC4X_TMADC(obj);

    memory_region_init_io(&adc->mmio, obj, &tc4x_tmadc_ops, adc,
                          "tmadc.regs", tc4x_tmadc_window());
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &adc->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), adc->sr, "sr",
                             TC4X_TMADC_COUNT * TC4X_TMADC_SR_COUNT);
    for (unsigned k = 0; k < TC4X_TMADC_COUNT; k++) {
        for (unsigned ch = 0; ch < TC4X_TMADC_CHANNELS; ch++) {
            g_autofree char *name = g_strdup_printf("volts[%u][%u]", k, ch);

            object_property_add(obj, name, "number", tmadc_get_volts,
                                tmadc_set_volts, NULL, &adc->in[k][ch]);
        }
        for (unsigned i = 0; i < TC4X_TMADC_SR_COUNT; i++) {
            g_autofree char *name = g_strdup_printf("period-ns[%u][%u]", k,
                                                    i);
            Tc4xTmadcTrigger *t = &adc->trig[k][i];

            *t = (Tc4xTmadcTrigger) {
                .adc = adc, .k = k, .sr = i,
                .period_ns = TMADC_DEFAULT_PERIOD_NS,
            };
            object_property_add(obj, name, "uint64", tmadc_get_period,
                                tmadc_set_period, NULL, t);
        }
    }
}

static void tc4x_tmadc_realize(DeviceState *dev, Error **errp)
{
    Tc4xTmadc *adc = TC4X_TMADC(dev);

    if (!adc->sfr || !adc->timebase) {
        error_setg(errp, "tc4x-tmadc: links sfr-store and timebase must be "
                   "set");
        return;
    }
    for (unsigned k = 0; k < TC4X_TMADC_COUNT; k++) {
        for (unsigned i = 0; i < TC4X_TMADC_SR_COUNT; i++) {
            tc4x_guest_timer_init(&adc->trig[k][i].timer, adc->timebase,
                                  tmadc_round, &adc->trig[k][i]);
        }
    }
    memory_region_enable_lockless_io(&adc->mmio);
    memory_region_set_imprecise_io(&adc->mmio, adc->imprecise_io);
    tc4x_sfr_store_add_passthrough(adc->sfr, &adc->mmio);
}

static const Property tc4x_tmadc_properties[] = {
    DEFINE_PROP_BOOL("imprecise-io", Tc4xTmadc, imprecise_io, false),
    DEFINE_PROP_LINK("sfr-store", Tc4xTmadc, sfr, TYPE_TC4X_SFR_STORE,
                     Tc4xSfrStore *),
    DEFINE_PROP_LINK("timebase", Tc4xTmadc, timebase, TYPE_TC4X_TIMEBASE,
                     Tc4xTimebase *),
};

static const VMStateDescription vmstate_tc4x_tmadc = {
    .name = TYPE_TC4X_TMADC,
    .unmigratable = 1,
};

static const char *const tc4x_tmadc_sim_commands[] = { "adc", NULL };

static void tc4x_tmadc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    Tc4xSimCommandsClass *sc = TC4X_SIM_COMMANDS_CLASS(oc);

    dc->desc = "TC4x TMADC kernels";
    dc->realize = tc4x_tmadc_realize;
    dc->vmsd = &vmstate_tc4x_tmadc;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_tmadc_reset_hold;
    sc->commands = tc4x_tmadc_sim_commands;
    sc->run = tc4x_tmadc_sim_run;
    device_class_set_props(dc, tc4x_tmadc_properties);
}

static double tc4x_analog_legacy_volts(Tc4xAnalogSource *src, unsigned line)
{
    Tc4xAnalogLegacy *l = TC4X_ANALOG_LEGACY(src);

    return l->fn(l->opaque, line / TC4X_TMADC_CHANNELS,
                 line % TC4X_TMADC_CHANNELS);
}

static void tc4x_analog_legacy_class_init(ObjectClass *oc, const void *data)
{
    Tc4xAnalogSourceClass *ac = TC4X_ANALOG_SOURCE_CLASS(oc);

    ac->volts = tc4x_analog_legacy_volts;
}

double tc4x_adc_volts(Tc4xSoc *soc, unsigned kernel, unsigned ch)
{
    return tc4x_tmadc_volts(tc4x_soc_tmadc(soc), kernel, ch);
}

void tc4x_adc_set_volts(Tc4xSoc *soc, unsigned kernel, unsigned ch,
                        double volts)
{
    tc4x_tmadc_set_volts(tc4x_soc_tmadc(soc), kernel, ch, volts);
}

void tc4x_adc_set_input(Tc4xSoc *soc, unsigned kernel, unsigned ch,
                        Tc4xAdcInputFn fn, void *opaque)
{
    Tc4xTmadc *adc = tc4x_soc_tmadc(soc);
    Tc4xAnalogLegacy *l = NULL;

    if (fn) {
        l = TC4X_ANALOG_LEGACY(object_new(TYPE_TC4X_ANALOG_LEGACY));
        l->fn = fn;
        l->opaque = opaque;
        object_property_add_child(OBJECT(adc), "legacy[*]", OBJECT(l));
        object_unref(OBJECT(l));
    }
    tc4x_tmadc_connect(adc, kernel, ch, l ? TC4X_ANALOG_SOURCE(l) : NULL,
                       kernel * TC4X_TMADC_CHANNELS + ch, &error_fatal);
}

bool tc4x_adc_sr_enabled(Tc4xSoc *soc, unsigned kernel, unsigned sr)
{
    return tc4x_tmadc_sr_enabled(tc4x_soc_tmadc(soc), kernel, sr);
}

void tc4x_adc_set_period(Tc4xSoc *soc, unsigned kernel, unsigned sr,
                         int64_t period_ns)
{
    g_autofree char *name = g_strdup_printf("period-ns[%u][%u]", kernel, sr);

    object_property_set_uint(OBJECT(tc4x_soc_tmadc(soc)), name, period_ns,
                             &error_fatal);
}

static const TypeInfo tc4x_tmadc_types[] = {
    {
        .name = TYPE_TC4X_ANALOG_SOURCE,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(Tc4xAnalogSourceClass),
    }, {
        .name = TYPE_TC4X_TMADC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xTmadc),
        .instance_init = tc4x_tmadc_init,
        .class_init = tc4x_tmadc_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_SIM_COMMANDS },
            { }
        },
    }, {
        .name = TYPE_TC4X_ANALOG_LEGACY,
        .parent = TYPE_OBJECT,
        .instance_size = sizeof(Tc4xAnalogLegacy),
        .class_init = tc4x_analog_legacy_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_ANALOG_SOURCE },
            { }
        },
    },
};

DEFINE_TYPES(tc4x_tmadc_types)
