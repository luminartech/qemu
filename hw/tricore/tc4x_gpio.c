/*
 * TC4x GPIO ports P00..P41.
 *
 * OMR/OMSR/OMCR and the per-pin PADCFGn_GPIO SET/CLR/OUT bits update the
 * port's OUT register; IN and PADCFGn_GPIO.IN read back the output levels,
 * except for pins driven from outside (tc4x_gpio_set_input) or by the sim
 * bridge.
 * Output changes go to the listeners of the port (bit-banged buses).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "tc4x_soc.h"

#define PORT_BASE           0xF003A000u
#define PORT_STRIDE         0x400u
#define PORT_OUT            0x20u
#define PORT_IN             0x24u
#define PORT_OMR            0x3Cu
#define PORT_OMCR           0x40u
#define PORT_OMSR           0x44u
#define PORT_PADCFG         0x300u
#define PORT_PADCFG_STRIDE  0x10u
#define PORT_PINS           16u
#define PADCFG_GPIO_OUT     BIT(0)
#define PADCFG_GPIO_SET     BIT(2)
#define PADCFG_GPIO_CLR     BIT(3)
#define PADCFG_GPIO_IN      BIT(8)

#define GPIO_LISTENERS_MAX  4

typedef struct GpioListener {
    unsigned port;
    Tc4xGpioListenerFn fn;
    void *opaque;
} GpioListener;

struct Tc4xGpio {
    Tc4xSoc *soc;
    /* per port: inputs driven from outside, atomic (read without the BQL) */
    uint32_t mask[TC4X_PORT_COUNT];
    uint32_t in[TC4X_PORT_COUNT];
    GpioListener listeners[GPIO_LISTENERS_MAX];
    unsigned n_listeners;
    /* ports with a listener, one bit each */
    uint64_t listened;
};

static uint32_t port_out_addr(unsigned n)
{
    return PORT_BASE + n * PORT_STRIDE + PORT_OUT;
}

static uint32_t port_in(Tc4xGpio *g, unsigned n)
{
    uint32_t mask = qatomic_read(&g->mask[n]);

    return (tc4x_reg_get(g->soc, port_out_addr(n)) & ~mask) |
           (qatomic_read(&g->in[n]) & mask);
}

static bool padcfg_gpio(uint32_t off, unsigned *pin)
{
    if (off < PORT_PADCFG ||
        off >= PORT_PADCFG + PORT_PINS * PORT_PADCFG_STRIDE ||
        (off - PORT_PADCFG) % PORT_PADCFG_STRIDE != 0) {
        return false;
    }
    *pin = (off - PORT_PADCFG) / PORT_PADCFG_STRIDE;
    return true;
}

static bool port_decode(uint32_t addr, unsigned *n, uint32_t *off)
{
    if (addr < PORT_BASE || addr >= PORT_BASE + TC4X_PORT_COUNT * PORT_STRIDE) {
        return false;
    }
    *n = (addr - PORT_BASE) / PORT_STRIDE;
    *off = (addr - PORT_BASE) % PORT_STRIDE;
    return true;
}

bool tc4x_gpio_read(Tc4xGpio *g, uint32_t addr, uint32_t *val)
{
    unsigned n, pin;
    uint32_t off;

    if (!port_decode(addr, &n, &off)) {
        return false;
    }
    if (off == PORT_IN) {
        *val = port_in(g, n);
        return true;
    }
    if (padcfg_gpio(off, &pin)) {
        uint32_t out = tc4x_reg_get(g->soc, port_out_addr(n));

        *val = (tc4x_reg_get(g->soc, addr) &
                ~(PADCFG_GPIO_OUT | PADCFG_GPIO_SET | PADCFG_GPIO_CLR |
                  PADCFG_GPIO_IN)) |
               ((out >> pin) & 1 ? PADCFG_GPIO_OUT : 0) |
               ((port_in(g, n) >> pin) & 1 ? PADCFG_GPIO_IN : 0);
        return true;
    }
    return false;
}

/*
 * OUT = (OUT & ~clr | set) ^ toggle, atomically: ISRs drive pins without
 * the BQL (tc4x_gpio_write_lockless) while other cores write the same port
 * under it.  Returns the old OUT, *out the new one.
 */
static uint32_t gpio_out_update(Tc4xGpio *g, unsigned n, uint32_t clr,
                                uint32_t set, uint32_t toggle, uint32_t *out)
{
    uint32_t out_addr = port_out_addr(n);
    uint32_t old = tc4x_reg_get(g->soc, out_addr), prev;

    do {
        prev = old;
        *out = ((prev & ~clr) | set) ^ toggle;
        old = tc4x_reg_cmpxchg(g->soc, out_addr, prev, *out);
    } while (old != prev);
    return old;
}

/* PADCFG GPIO write: the configuration is stored, the level goes to OUT */
static uint32_t gpio_padcfg_write(Tc4xGpio *g, uint32_t addr, unsigned n,
                                  unsigned pin, uint32_t val, uint32_t *out)
{
    /* SET / CLR win over the OUT bit written back by read-modify-write */
    bool level = val & PADCFG_GPIO_SET ? true :
                 val & PADCFG_GPIO_CLR ? false : val & PADCFG_GPIO_OUT;

    tc4x_reg_set(g->soc, addr, val & ~(PADCFG_GPIO_OUT | PADCFG_GPIO_SET |
                                       PADCFG_GPIO_CLR | PADCFG_GPIO_IN));
    return gpio_out_update(g, n, BIT(pin), level ? BIT(pin) : 0, 0, out);
}

static void gpio_changed(Tc4xGpio *g, unsigned n, uint32_t old, uint32_t out)
{
    for (unsigned i = 0; i < g->n_listeners; i++) {
        if (g->listeners[i].port == n) {
            g->listeners[i].fn(g->listeners[i].opaque, n, old, out);
        }
    }
}

bool tc4x_gpio_write(Tc4xGpio *g, uint32_t addr, uint32_t val)
{
    unsigned n, pin;
    uint32_t off, old, out, set = 0, clr = 0;

    if (!port_decode(addr, &n, &off)) {
        return false;
    }
    if (padcfg_gpio(off, &pin)) {
        old = gpio_padcfg_write(g, addr, n, pin, val, &out);
        gpio_changed(g, n, old, out);
        return true;
    }
    switch (off) {
    case PORT_OUT:
        clr = UINT32_MAX;
        set = val;
        break;
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
        return false;
    }
    if (off == PORT_OUT) {
        old = gpio_out_update(g, n, clr, set, 0, &out);
    } else {
        /* PS and PCL both set toggles the pin */
        old = gpio_out_update(g, n, clr & ~set, set & ~clr, set & clr, &out);
    }
    gpio_changed(g, n, old, out);
    return true;
}

bool tc4x_gpio_write_lockless(Tc4xGpio *g, uint32_t addr, uint32_t val)
{
    unsigned n, pin;
    uint32_t off, out;

    if (!port_decode(addr, &n, &off) || !padcfg_gpio(off, &pin) ||
        (g->listened & BIT_ULL(n))) {
        return false;
    }
    gpio_padcfg_write(g, addr, n, pin, val, &out);
    return true;
}

bool tc4x_gpio_out(Tc4xSoc *soc, unsigned port, unsigned pin)
{
    return (tc4x_reg_get(soc, port_out_addr(port)) >> pin) & 1;
}

void tc4x_gpio_set_input(Tc4xSoc *soc, unsigned port, unsigned pin,
                         int level)
{
    Tc4xGpio *g = tc4x_soc_gpio(soc);

    assert(port < TC4X_PORT_COUNT && pin < PORT_PINS);
    if (level < 0) {
        qatomic_and(&g->mask[port], ~BIT(pin));
        return;
    }
    if (level & 1) {
        qatomic_or(&g->in[port], BIT(pin));
    } else {
        qatomic_and(&g->in[port], ~BIT(pin));
    }
    qatomic_or(&g->mask[port], BIT(pin));
}

void tc4x_gpio_add_listener(Tc4xSoc *soc, unsigned port,
                            Tc4xGpioListenerFn fn, void *opaque)
{
    Tc4xGpio *g = tc4x_soc_gpio(soc);

    assert(port < TC4X_PORT_COUNT && g->n_listeners < GPIO_LISTENERS_MAX);
    g->listeners[g->n_listeners++] = (GpioListener) {
        .port = port, .fn = fn, .opaque = opaque,
    };
    g->listened |= BIT_ULL(port);
}

static bool gpio_command(void *opaque, int argc, char **argv, GString *out)
{
    Tc4xGpio *g = opaque;
    unsigned n, pin;

    if (strcmp(argv[0], "gpio") != 0) {
        return false;
    }
    if (argc < 4) {
        g_string_append(out, "err usage: gpio PORT PIN 0|1|release\n");
        return true;
    }
    n = g_ascii_strtoull(argv[1], NULL, 10);
    pin = g_ascii_strtoull(argv[2], NULL, 0);
    if (n >= TC4X_PORT_COUNT || pin >= PORT_PINS) {
        g_string_append(out, "err bad port/pin\n");
        return true;
    }
    tc4x_gpio_set_input(g->soc, n, pin, !strcmp(argv[3], "release") ? -1 :
                        (int)(g_ascii_strtoull(argv[3], NULL, 0) & 1));
    g_string_append(out, "ok\n");
    return true;
}

Tc4xGpio *tc4x_gpio_new(Tc4xSoc *soc)
{
    Tc4xGpio *g = g_new0(Tc4xGpio, 1);

    g->soc = soc;
    tc4x_sim_add_command(soc, "gpio", gpio_command, g, false);
    return g;
}
