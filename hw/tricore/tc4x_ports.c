/*
 * TC4x GPIO ports P00..P41.
 *
 * OMR/OMSR/OMCR and the per-pin PADCFGn_GPIO SET/CLR/OUT bits update the
 * port's output latch (OUT); IN and PADCFGn_GPIO.IN read back the latch,
 * except for pins driven from outside (input lines, straps).  The other
 * bits of PADCFGn_GPIO and the remaining port registers are plain storage
 * in the SFR store.
 *
 * Register layout: Ifx_P_* of the iLLD SFR headers (tc4x_regs.h).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_ports.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "tc4x_soc.h"
#include "trace.h"

#define PORT_STRIDE         (A_P01_OUT - A_P00_OUT)
#define PORTS_BASE          (A_P00_OUT & ~(PORT_STRIDE - 1))
#define PORT_REG(r)         (A_P00_##r - PORTS_BASE)
#define PORT_OUT            PORT_REG(OUT)
#define PORT_IN             PORT_REG(IN)
#define PORT_OMR            PORT_REG(OMR)
#define PORT_OMCR           PORT_REG(OMCR)
#define PORT_OMSR           PORT_REG(OMSR)
#define PORT_PADCFG         PORT_REG(PADCFG0_GPIO)
#define PORT_PADCFG_STRIDE  (A_P00_PADCFG1_GPIO - A_P00_PADCFG0_GPIO)
#define PADCFG_GPIO_LEVEL   (R_P_PADCFG_GPIO_OUT_MASK | \
                             R_P_PADCFG_GPIO_SET_MASK | \
                             R_P_PADCFG_GPIO_CLR_MASK | \
                             R_P_PADCFG_GPIO_IN_MASK)
/* the PADCFGn_GPIO bits that change OUT */
#define PADCFG_GPIO_DRIVE   (R_P_PADCFG_GPIO_OUT_MASK | \
                             R_P_PADCFG_GPIO_SET_MASK | \
                             R_P_PADCFG_GPIO_CLR_MASK)

/* The input lines of one port, for their handlers */
typedef struct Tc4xPortLines {
    Tc4xPorts *ports;
    unsigned n;
} Tc4xPortLines;

typedef struct Tc4xPortListener {
    unsigned port;
    Tc4xGpioListenerFn fn;
    void *opaque;
} Tc4xPortListener;

/*
 * Locking: out, in and driven are atomic, changed by any thread.
 * listeners and listened are set during machine init only.
 */
struct Tc4xPorts {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    Tc4xSfrStore *sfr;
    bool imprecise_io;
    uint32_t num_straps;
    uint32_t *straps;
    Tc4xPortLines lines[TC4X_PORT_COUNT];
    qemu_irq out_line[TC4X_PORT_COUNT][TC4X_PORT_PINS];

    uint32_t out[TC4X_PORT_COUNT];      /* output latch */
    uint32_t in[TC4X_PORT_COUNT];       /* levels driven from outside */
    uint32_t driven[TC4X_PORT_COUNT];   /* pins driven from outside */
    GArray *listeners;                  /* Tc4xPortListener */
    uint64_t listened;                  /* ports with a listener */
};

uint32_t tc4x_ports_base(void)
{
    return PORTS_BASE;
}

uint32_t tc4x_ports_window(void)
{
    return TC4X_PORT_COUNT * PORT_STRIDE;
}

bool tc4x_ports_out(Tc4xPorts *p, unsigned port, unsigned pin)
{
    return (qatomic_read(&p->out[port]) >> pin) & 1;
}

void tc4x_ports_drive(Tc4xPorts *p, unsigned port, unsigned pin, int level)
{
    if (level < 0) {
        qatomic_and(&p->driven[port], ~BIT(pin));
        return;
    }
    if (level) {
        qatomic_or(&p->in[port], BIT(pin));
    } else {
        qatomic_and(&p->in[port], ~BIT(pin));
    }
    qatomic_or(&p->driven[port], BIT(pin));
}

static void ports_in_line(void *opaque, int pin, int level)
{
    Tc4xPortLines *l = opaque;

    if (level) {
        qatomic_or(&l->ports->in[l->n], BIT(pin));
    } else {
        qatomic_and(&l->ports->in[l->n], ~BIT(pin));
    }
}

static void ports_drive_line(void *opaque, int pin, int level)
{
    Tc4xPortLines *l = opaque;

    if (level) {
        qatomic_or(&l->ports->driven[l->n], BIT(pin));
    } else {
        qatomic_and(&l->ports->driven[l->n], ~BIT(pin));
    }
}

static uint32_t port_in(Tc4xPorts *p, unsigned n)
{
    uint32_t driven = qatomic_read(&p->driven[n]);

    return (qatomic_read(&p->out[n]) & ~driven) |
           (qatomic_read(&p->in[n]) & driven);
}

/* Changes of port @n's latch go to output lines or listeners */
static bool port_needs_bql(Tc4xPorts *p, unsigned n)
{
    if (p->listened & BIT_ULL(n)) {
        return true;
    }
    for (unsigned pin = 0; pin < TC4X_PORT_PINS; pin++) {
        if (p->out_line[n][pin]) {
            return true;
        }
    }
    return false;
}

static bool padcfg_pin(hwaddr off, unsigned *pin)
{
    if (off < PORT_PADCFG ||
        off >= PORT_PADCFG + TC4X_PORT_PINS * PORT_PADCFG_STRIDE ||
        (off - PORT_PADCFG) % PORT_PADCFG_STRIDE != 0) {
        return false;
    }
    *pin = (off - PORT_PADCFG) / PORT_PADCFG_STRIDE;
    return true;
}

static bool ports_serves(hwaddr off)
{
    unsigned pin;

    return off == PORT_OUT || off == PORT_IN || off == PORT_OMR ||
           off == PORT_OMCR || off == PORT_OMSR || padcfg_pin(off, &pin);
}

/* The word at offset @off of port @n, one ports_serves() accepts */
static uint32_t port_read_word(Tc4xPorts *p, unsigned n, hwaddr off,
                               uint32_t addr)
{
    unsigned pin;

    if (off == PORT_OUT) {
        return qatomic_read(&p->out[n]);
    }
    if (off == PORT_IN) {
        return port_in(p, n);
    }
    if (padcfg_pin(off, &pin)) {
        uint32_t v = tc4x_sfr_store_get(p->sfr, addr) & ~PADCFG_GPIO_LEVEL;

        v = FIELD_DP32(v, P_PADCFG_GPIO, OUT,
                       (qatomic_read(&p->out[n]) >> pin) & 1);
        return FIELD_DP32(v, P_PADCFG_GPIO, IN, (port_in(p, n) >> pin) & 1);
    }
    return 0;   /* OMR, OMSR, OMCR: write only */
}

/*
 * OUT = ((OUT & ~clr) | set) ^ toggle, atomically: cores write a port
 * without the BQL.  Returns the old OUT.
 */
static uint32_t port_out_update(Tc4xPorts *p, unsigned n, uint32_t clr,
                                uint32_t set, uint32_t toggle)
{
    uint32_t old = qatomic_read(&p->out[n]), prev;

    do {
        prev = old;
        old = qatomic_cmpxchg(&p->out[n], prev,
                              ((prev & ~clr) | set) ^ toggle);
    } while (old != prev);
    return old;
}

/* A full word written to offset @off of port @n; returns the old OUT */
static uint32_t port_write_word(Tc4xPorts *p, unsigned n, hwaddr off,
                                uint32_t addr, uint32_t val)
{
    uint32_t set = 0, clr = 0;
    unsigned pin;

    if (padcfg_pin(off, &pin)) {
        /* SET / CLR win over the OUT bit written back by read-modify-write */
        bool level = FIELD_EX32(val, P_PADCFG_GPIO, SET) ? true :
                     FIELD_EX32(val, P_PADCFG_GPIO, CLR) ? false :
                     FIELD_EX32(val, P_PADCFG_GPIO, OUT);

        tc4x_sfr_store_set(p->sfr, addr, val & ~PADCFG_GPIO_LEVEL);
        return port_out_update(p, n, BIT(pin), level ? BIT(pin) : 0, 0);
    }
    switch (off) {
    case PORT_OUT:
        return port_out_update(p, n, UINT32_MAX, val & 0xFFFF, 0);
    case PORT_OMR:
        set = val & 0xFFFF;
        clr = val >> 16;
        break;
    case PORT_OMSR:
        set = val & 0xFFFF;
        break;
    case PORT_OMCR:
        clr = val >> 16;
        break;
    default:
        return qatomic_read(&p->out[n]);     /* IN: read only */
    }
    /* PS and PCL both set toggle the pin */
    return port_out_update(p, n, clr & ~set, set & ~clr, set & clr);
}

/*
 * @size bytes of @val written at byte @shift of the word at offset @off of
 * port @n; returns the old OUT.  The bytes not written keep their value
 * even against another core's concurrent write: OUT updates only the
 * written bits, and a PADCFG write without its level byte leaves OUT alone
 * and changes its configuration bits atomically.
 */
static uint32_t port_write_part(Tc4xPorts *p, unsigned n, hwaddr off,
                                uint32_t addr, unsigned shift, unsigned size,
                                uint32_t val)
{
    uint32_t mask = MAKE_64BIT_MASK(shift, size * 8);
    uint32_t bits = (val << shift) & mask;
    unsigned pin;

    if (off == PORT_OUT) {
        return port_out_update(p, n, mask & 0xFFFF, bits & 0xFFFF, 0);
    }
    if (padcfg_pin(off, &pin) && !(mask & PADCFG_GPIO_DRIVE)) {
        uint32_t old = tc4x_sfr_store_get(p->sfr, addr), prev;

        mask &= ~PADCFG_GPIO_LEVEL;
        do {
            prev = old;
            old = tc4x_sfr_store_cmpxchg(p->sfr, addr, prev,
                                         (prev & ~mask) | (bits & mask));
        } while (old != prev);
        return qatomic_read(&p->out[n]);
    }
    return port_write_word(p, n, off, addr,
                           deposit32(port_read_word(p, n, off, addr), shift,
                                     size * 8, val));
}

static void port_changed(Tc4xPorts *p, unsigned n, uint32_t old)
{
    uint32_t out = qatomic_read(&p->out[n]);
    uint32_t diff = old ^ out;

    if (!diff) {
        return;
    }
    trace_tc4x_ports_out(n, old, out);
    for (unsigned i = 0; i < p->listeners->len; i++) {
        Tc4xPortListener *l = &g_array_index(p->listeners, Tc4xPortListener,
                                             i);

        if (l->port == n) {
            l->fn(l->opaque, n, old, out);
        }
    }
    for (unsigned pin = 0; pin < TC4X_PORT_PINS; pin++) {
        if (diff & BIT(pin)) {
            qemu_set_irq(p->out_line[n][pin], (out >> pin) & 1);
        }
    }
}

static MemTxResult tc4x_ports_read(void *opaque, hwaddr off, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    Tc4xPorts *p = opaque;
    unsigned n = off / PORT_STRIDE;
    hwaddr reg = off % PORT_STRIDE;
    uint32_t addr = PORTS_BASE + off;

    if (tc4x_master_refused(attrs, "tc4x-ports", addr, false)) {
        return MEMTX_ERROR;
    }
    if (!ports_serves(reg & ~3u)) {
        *data = tc4x_sfr_store_access_read(p->sfr, addr, size, attrs);
    } else {
        *data = extract32(port_read_word(p, n, reg & ~3u, addr & ~3u),
                          (off & 3) * 8, size * 8);
    }
    return MEMTX_OK;
}

/*
 * Writes run on the vCPU thread without the BQL, unless the port's latch
 * changes go to output lines or listeners.  A write of less than a word
 * changes only its bytes (port_write_part); into the write-only registers
 * it merges into 0.
 */
static MemTxResult tc4x_ports_write(void *opaque, hwaddr off, uint64_t val,
                                    unsigned size, MemTxAttrs attrs)
{
    Tc4xPorts *p = opaque;
    unsigned n = off / PORT_STRIDE;
    hwaddr reg = off % PORT_STRIDE & ~3u;
    uint32_t addr = (PORTS_BASE + off) & ~3u;
    unsigned shift = (off & 3) * 8;
    uint32_t old;

    if (tc4x_master_refused(attrs, "tc4x-ports", PORTS_BASE + off, true)) {
        return MEMTX_ERROR;
    }
    if (!ports_serves(reg)) {
        tc4x_sfr_store_access_write(p->sfr, PORTS_BASE + off, val,
                                    size, attrs);
        return MEMTX_OK;
    }
    if (!port_needs_bql(p, n)) {
        port_write_part(p, n, reg, addr, shift, size, val);
        return MEMTX_OK;
    }
    BQL_LOCK_GUARD();
    old = port_write_part(p, n, reg, addr, shift, size, val);
    port_changed(p, n, old);
    return MEMTX_OK;
}

static const MemoryRegionOps tc4x_ports_ops = {
    .read_with_attrs = tc4x_ports_read,
    .write_with_attrs = tc4x_ports_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void tc4x_ports_add_listener(Tc4xPorts *p, unsigned port,
                             Tc4xGpioListenerFn fn, void *opaque)
{
    Tc4xPortListener l = { .port = port, .fn = fn, .opaque = opaque };

    if (port >= TC4X_PORT_COUNT) {
        error_setg(&error_fatal, "tc4x-ports: no port P%02u", port);
        return;
    }
    g_array_append_val(p->listeners, l);
    p->listened |= BIT_ULL(port);
}

bool tc4x_gpio_out(Tc4xSoc *soc, unsigned port, unsigned pin)
{
    return tc4x_ports_out(tc4x_soc_ports(soc), port, pin);
}

void tc4x_gpio_set_input(Tc4xSoc *soc, unsigned port, unsigned pin,
                         int level)
{
    assert(port < TC4X_PORT_COUNT && pin < TC4X_PORT_PINS);
    tc4x_ports_drive(tc4x_soc_ports(soc), port, pin, level);
}

void tc4x_gpio_add_listener(Tc4xSoc *soc, unsigned port,
                            Tc4xGpioListenerFn fn, void *opaque)
{
    tc4x_ports_add_listener(tc4x_soc_ports(soc), port, fn, opaque);
}

/* gpio PORT PIN 0|1|release: drive or let go of an input */
bool tc4x_ports_command(void *opaque, int argc, char **argv, GString *out)
{
    Tc4xPorts *p = opaque;
    unsigned n, pin, level = 0;
    bool release;

    if (strcmp(argv[0], "gpio") != 0) {
        return false;
    }
    release = argc == 4 && !strcmp(argv[3], "release");
    if (argc != 4 || qemu_strtoui(argv[1], NULL, 10, &n) < 0 ||
        qemu_strtoui(argv[2], NULL, 0, &pin) < 0 ||
        (!release && (qemu_strtoui(argv[3], NULL, 0, &level) < 0 ||
                      level > 1))) {
        g_string_append(out, "err usage: gpio PORT PIN 0|1|release\n");
        return true;
    }
    if (n >= TC4X_PORT_COUNT || pin >= TC4X_PORT_PINS) {
        g_string_append(out, "err bad port/pin\n");
        return true;
    }
    tc4x_ports_drive(p, n, pin, release ? -1 : (int)level);
    g_string_append(out, "ok\n");
    return true;
}

/* The latch resets to 0; the input lines keep what drives them */
static void tc4x_ports_reset_hold(Object *obj, ResetType type)
{
    Tc4xPorts *p = TC4X_PORTS(obj);

    for (unsigned n = 0; n < TC4X_PORT_COUNT; n++) {
        qatomic_set(&p->out[n], 0);
    }
}

static void tc4x_ports_reset_exit(Object *obj, ResetType type)
{
    Tc4xPorts *p = TC4X_PORTS(obj);

    for (unsigned n = 0; n < TC4X_PORT_COUNT; n++) {
        for (unsigned pin = 0; pin < TC4X_PORT_PINS; pin++) {
            qemu_set_irq(p->out_line[n][pin], 0);
        }
    }
}

static void tc4x_ports_init(Object *obj)
{
    Tc4xPorts *p = TC4X_PORTS(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&p->mmio, obj, &tc4x_ports_ops, p, "ports.regs",
                          tc4x_ports_window());
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
    for (unsigned n = 0; n < TC4X_PORT_COUNT; n++) {
        g_autofree char *out = g_strdup_printf("p%02u-out", n);
        g_autofree char *in = g_strdup_printf("p%02u-in", n);
        g_autofree char *drive = g_strdup_printf("p%02u-drive", n);

        p->lines[n] = (Tc4xPortLines) { .ports = p, .n = n };
        qdev_init_gpio_out_named(dev, p->out_line[n], out, TC4X_PORT_PINS);
        qdev_init_gpio_in_named_with_opaque(dev, ports_in_line, &p->lines[n],
                                            in, TC4X_PORT_PINS);
        qdev_init_gpio_in_named_with_opaque(dev, ports_drive_line,
                                            &p->lines[n], drive,
                                            TC4X_PORT_PINS);
    }
    p->listeners = g_array_new(false, false, sizeof(Tc4xPortListener));
}

static void tc4x_ports_finalize(Object *obj)
{
    g_array_free(TC4X_PORTS(obj)->listeners, true);
}

static void tc4x_ports_realize(DeviceState *dev, Error **errp)
{
    Tc4xPorts *p = TC4X_PORTS(dev);

    if (!p->sfr) {
        error_setg(errp, "tc4x-ports: link sfr-store is not set");
        return;
    }
    for (uint32_t i = 0; i < p->num_straps; i++) {
        uint32_t s = p->straps[i];
        unsigned port = extract32(s, 16, 16), pin = extract32(s, 8, 8);
        unsigned level = extract32(s, 0, 8);

        if (port >= TC4X_PORT_COUNT || pin >= TC4X_PORT_PINS || level > 1) {
            error_setg(errp, "tc4x-ports: strap 0x%08x is not a port pin at "
                       "level 0 or 1", s);
            return;
        }
        tc4x_ports_drive(p, port, pin, level);
    }
    memory_region_enable_lockless_io(&p->mmio);
    memory_region_set_imprecise_io(&p->mmio, p->imprecise_io);
    tc4x_sfr_store_add_passthrough(p->sfr, &p->mmio);
}

static const Property tc4x_ports_properties[] = {
    DEFINE_PROP_BOOL("imprecise-io", Tc4xPorts, imprecise_io, false),
    DEFINE_PROP_LINK("sfr-store", Tc4xPorts, sfr, TYPE_TC4X_SFR_STORE,
                     Tc4xSfrStore *),
    DEFINE_PROP_ARRAY("straps", Tc4xPorts, num_straps, straps,
                      qdev_prop_uint32, uint32_t),
};

static const VMStateDescription vmstate_tc4x_ports = {
    .name = TYPE_TC4X_PORTS,
    .unmigratable = 1,
};

static void tc4x_ports_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "TC4x GPIO ports";
    dc->realize = tc4x_ports_realize;
    dc->vmsd = &vmstate_tc4x_ports;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_ports_reset_hold;
    rc->phases.exit = tc4x_ports_reset_exit;
    device_class_set_props(dc, tc4x_ports_properties);
}

static const TypeInfo tc4x_ports_types[] = {
    {
        .name = TYPE_TC4X_PORTS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xPorts),
        .instance_init = tc4x_ports_init,
        .instance_finalize = tc4x_ports_finalize,
        .class_init = tc4x_ports_class_init,
    },
};

DEFINE_TYPES(tc4x_ports_types)
