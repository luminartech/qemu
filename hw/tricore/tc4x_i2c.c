/*
 * TC4x I2C controllers (I2C0..2).
 *
 * The model follows the register sequences of a FIFO-driven I2C driver:
 * a transfer completes instantly, the FIFO is serviced through the
 * single/last-single request bits in RIS, the end of a transfer is reported
 * in PIRQSS (RX, TX_END, NACK) and the bus is always idle in between.
 *
 *   write: TPSCTRL=N, then words to TXD (address byte first), each followed
 *          by an ICR clear of the request bits; the slave gets N-1 bytes,
 *          then TX_END (+NACK when no slave answers the address or a byte).
 *   read:  TPSCTRL=1, MRPSCTRL=M, address|1 to TXD -> RX; every ICR clear
 *          re-arms a request until all M bytes were read from RXD, then
 *          TX_END.
 *
 * Register layout: Ifx_I2C_* of the iLLD SFR headers (tc4x_regs.h).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_i2c.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "tc4x_soc.h"
#include "trace.h"

#define I2C_REG(r)          (A_I2C0_##r - A_I2C0_CLC1)
#define I2C_TXD             I2C_REG(TXD)
#define I2C_RXD             I2C_REG(RXD)

#define RIS_DTR_MASK        0x0Fu
#define TPS_MASK            R_I2C_TPSCTRL_TPS_MASK

#define I2C_XFER_MAX        512u

typedef enum {
    XFER_IDLE,
    XFER_TX,        /* collecting TPS bytes from TXD */
    XFER_RX,        /* M bytes ready in buf[] */
} XferPhase;

/*
 * Locking: the registers are served under the BQL (the region is not
 * lockless), and so is every field.
 */
struct Tc4xI2c {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    I2CBus *bus;
    Tc4xSfrStore *sfr;
    uint32_t index;
    bool imprecise_io;

    XferPhase phase;
    uint32_t dtr;           /* RIS request bits */
    uint32_t pirq;          /* PIRQSS */
    uint32_t errirq;        /* ERRIRQSS */
    uint32_t tps, mrps;
    uint8_t buf[I2C_XFER_MAX];
    uint32_t len;           /* TX bytes collected / RX bytes available */
    uint32_t pos;           /* RX bytes handed out */
};

/*
 * The whole-transfer Tc4xI2cSlaveOps on the bus: written bytes are
 * collected until the transfer ends; the TC4x controller reads a whole
 * transfer through the ops at once, as it knows its length.
 */
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xI2cLegacy, TC4X_I2C_LEGACY)

struct Tc4xI2cLegacy {
    I2CSlave parent_obj;

    const Tc4xI2cSlaveOps *ops;
    void *opaque;
    bool started;
    bool sending;
    uint8_t buf[I2C_XFER_MAX];
    unsigned len;
};

static const uint32_t i2c_bases[TC4X_I2C_COUNT] = {
    A_I2C0_CLC1, A_I2C1_CLC1, A_I2C2_CLC1,
};

uint32_t tc4x_i2c_base(unsigned n)
{
    return i2c_bases[n];
}

I2CBus *tc4x_i2c_bus(Tc4xI2c *c)
{
    return c->bus;
}

static uint32_t i2c_store_get(Tc4xI2c *c, uint32_t off)
{
    return tc4x_sfr_store_get(c->sfr, i2c_bases[c->index] + off);
}

/* Request the next FIFO word (single or last single request) */
static void i2c_request(Tc4xI2c *c, uint32_t remaining)
{
    uint32_t fifocfg = i2c_store_get(c, I2C_REG(FIFOCFG));
    unsigned align = c->phase == XFER_RX ?
                     FIELD_EX32(fifocfg, I2C_FIFOCFG, RXFA) :
                     FIELD_EX32(fifocfg, I2C_FIFOCFG, TXFA);

    c->dtr |= remaining <= (4u >> align) ? R_I2C_RIS_LSREQ_INT_MASK :
                                            R_I2C_RIS_SREQ_INT_MASK;
}

/* The slave the bus addresses, if it is a Tc4xI2cSlaveOps one */
static Tc4xI2cLegacy *i2c_legacy_current(I2CBus *bus)
{
    I2CNode *node = QLIST_FIRST(&bus->current_devs);

    return node ? (Tc4xI2cLegacy *)object_dynamic_cast(OBJECT(node->elt),
                                                       TYPE_TC4X_I2C_LEGACY) :
                  NULL;
}

static void i2c_finish_read(Tc4xI2c *c, unsigned addr)
{
    Tc4xI2cLegacy *l = i2c_legacy_current(c->bus);

    c->len = MIN(c->mrps, I2C_XFER_MAX);
    c->pos = 0;
    if (l) {
        if (c->len) {
            l->ops->read(l->opaque, c->buf, c->len);
        }
    } else {
        for (uint32_t i = 0; i < c->len; i++) {
            c->buf[i] = i2c_recv(c->bus);
        }
    }
    i2c_end_transfer(c->bus);
    trace_tc4x_i2c_transfer(c->index, addr, true, c->len, true);
    if (c->len) {
        c->phase = XFER_RX;
        c->pirq |= R_I2C_PIRQSS_RX_MASK;
    } else {
        c->phase = XFER_IDLE;
        c->pirq |= R_I2C_PIRQSS_TX_END_MASK;
    }
}

static void i2c_finish_tx(Tc4xI2c *c)
{
    unsigned addr = c->buf[0] >> 1;
    bool read = c->buf[0] & 1;
    bool ack = true;

    if (i2c_start_transfer(c->bus, addr, read)) {
        trace_tc4x_i2c_transfer(c->index, addr, read, c->len - 1, false);
        c->pirq |= R_I2C_PIRQSS_NACK_MASK | R_I2C_PIRQSS_TX_END_MASK;
        c->phase = XFER_IDLE;
        return;
    }
    if (read) {
        i2c_finish_read(c, addr);
        return;
    }
    for (uint32_t i = 1; i < c->len && ack; i++) {
        ack = i2c_send(c->bus, c->buf[i]) == 0;
    }
    i2c_end_transfer(c->bus);
    trace_tc4x_i2c_transfer(c->index, addr, false, c->len - 1, ack);
    c->pirq |= R_I2C_PIRQSS_TX_END_MASK | (ack ? 0 : R_I2C_PIRQSS_NACK_MASK);
    c->phase = XFER_IDLE;
}

static void i2c_txd(Tc4xI2c *c, uint32_t val)
{
    uint32_t fifocfg = i2c_store_get(c, I2C_REG(FIFOCFG));
    unsigned step = 1u << FIELD_EX32(fifocfg, I2C_FIFOCFG, TXFA);

    if (c->phase != XFER_TX) {
        qemu_log_mask(LOG_GUEST_ERROR, "tc4x-i2c%u: TXD outside a transfer\n",
                      c->index);
        return;
    }
    for (unsigned i = 0; i < 4 && c->len < c->tps; i += step) {
        if (c->len < I2C_XFER_MAX) {
            c->buf[c->len] = val >> (8 * i);
        }
        c->len++;
    }
    if (c->len >= c->tps) {
        c->len = MIN(c->len, I2C_XFER_MAX);
        i2c_finish_tx(c);
    }
}

static uint32_t i2c_rxd(Tc4xI2c *c)
{
    uint32_t fifocfg = i2c_store_get(c, I2C_REG(FIFOCFG));
    unsigned step = 1u << FIELD_EX32(fifocfg, I2C_FIFOCFG, RXFA);
    uint32_t word = 0;

    if (c->phase != XFER_RX) {
        return 0;
    }
    for (unsigned i = 0; i < 4 && c->pos < c->len; i += step) {
        word |= (uint32_t)c->buf[c->pos++] << (8 * i);
    }
    return word;
}

/* The driver acknowledged a FIFO request: ask for the next word or finish */
static void i2c_icr(Tc4xI2c *c, uint32_t val)
{
    c->dtr &= ~(val & RIS_DTR_MASK);
    if (c->phase == XFER_TX && c->len < c->tps) {
        i2c_request(c, c->tps - c->len);
    } else if (c->phase == XFER_RX) {
        if (c->pos < c->len) {
            i2c_request(c, c->len - c->pos);
        } else {
            c->phase = XFER_IDLE;
            c->pirq |= R_I2C_PIRQSS_TX_END_MASK;
        }
    }
}

/* A word read of a register the controller serves */
static bool i2c_reg_read(Tc4xI2c *c, uint32_t off, uint32_t *val)
{
    switch (off) {
    case I2C_REG(RIS):
    case I2C_REG(MIS):
        /*
         * P_INT / ERR_INT are the PIRQSS / ERRIRQSS bits enabled in their
         * masks
         */
        *val = c->dtr |
               (c->errirq & i2c_store_get(c, I2C_REG(ERRIRQSM)) ?
                R_I2C_RIS_I2C_ERR_INT_MASK : 0) |
               (c->pirq & i2c_store_get(c, I2C_REG(PIRQSM)) ?
                R_I2C_RIS_I2C_P_INT_MASK : 0);
        return true;
    case I2C_REG(PIRQSS):
        *val = c->pirq;
        return true;
    case I2C_REG(ERRIRQSS):
        *val = c->errirq;
        return true;
    case I2C_REG(BUSSTAT):
        *val = 0;   /* idle */
        return true;
    case I2C_REG(RPSSTAT):
        *val = c->phase == XFER_RX ? c->pos : 0;
        return true;
    case I2C_REG(FFSSTAT):
        *val = c->phase == XFER_RX ? c->len - c->pos : 0;
        return true;
    default:
        if (off >= I2C_RXD) {
            *val = i2c_rxd(c);
            return true;
        }
        if (off >= I2C_TXD) {
            *val = 0;
            return true;
        }
        return false;
    }
}

static bool i2c_serves_write(uint32_t off)
{
    switch (off) {
    case I2C_REG(TPSCTRL):
    case I2C_REG(MRPSCTRL):
    case I2C_REG(ICR):
    case I2C_REG(PIRQSC):
    case I2C_REG(ERRIRQSC):
        return true;
    default:
        return off >= I2C_TXD;
    }
}

/* The registers i2c_reg_read() or i2c_reg_write() serve */
static bool tc4x_i2c_serves(void *opaque, hwaddr off)
{
    switch (off & ~3u) {
    case I2C_REG(RIS):
    case I2C_REG(MIS):
    case I2C_REG(PIRQSS):
    case I2C_REG(ERRIRQSS):
    case I2C_REG(BUSSTAT):
    case I2C_REG(RPSSTAT):
    case I2C_REG(FFSSTAT):
        return true;
    default:
        return i2c_serves_write(off & ~3u);
    }
}

/* A word write of a register i2c_serves_write() accepts */
static void i2c_reg_write(Tc4xI2c *c, uint32_t off, uint32_t val)
{
    switch (off) {
    case I2C_REG(TPSCTRL):
        c->tps = val & TPS_MASK;
        c->len = 0;
        c->pos = 0;
        c->phase = c->tps ? XFER_TX : XFER_IDLE;
        c->dtr = 0;
        if (c->tps) {
            i2c_request(c, c->tps);
        }
        tc4x_sfr_store_set(c->sfr, i2c_bases[c->index] + off, val);
        break;
    case I2C_REG(MRPSCTRL):
        c->mrps = val & TPS_MASK;
        tc4x_sfr_store_set(c->sfr, i2c_bases[c->index] + off, val);
        break;
    case I2C_REG(ICR):
        i2c_icr(c, val);
        break;
    case I2C_REG(PIRQSC):
        c->pirq &= ~val;
        break;
    case I2C_REG(ERRIRQSC):
        c->errirq &= ~val;
        break;
    default:
        if (off < I2C_RXD) {
            i2c_txd(c, val);
        }
        break;
    }
}

static MemTxResult tc4x_i2c_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    Tc4xI2c *c = opaque;
    uint32_t val;

    if (tc4x_master_refused(attrs, "tc4x-i2c", i2c_bases[c->index] + off,
                            false)) {
        return MEMTX_ERROR;
    }
    if (i2c_reg_read(c, off & ~3u, &val)) {
        *data = extract32(val, (off & 3) * 8, size * 8);
    } else {
        *data = tc4x_sfr_store_access_read(c->sfr, i2c_bases[c->index] + off,
                                           size, attrs);
    }
    return MEMTX_OK;
}

static MemTxResult tc4x_i2c_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    Tc4xI2c *c = opaque;

    if (tc4x_master_refused(attrs, "tc4x-i2c", i2c_bases[c->index] + off,
                            true)) {
        return MEMTX_ERROR;
    }
    if (!i2c_serves_write(off & ~3u)) {
        tc4x_sfr_store_access_write(c->sfr, i2c_bases[c->index] + off, val,
                                    size, attrs);
    } else if (size != 4 || (off & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tc4x-i2c%u: %u-byte write at 0x%" HWADDR_PRIx
                      " ignored\n", c->index, size, off);
    } else {
        i2c_reg_write(c, off, val);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps tc4x_i2c_ops = {
    .read_with_attrs = tc4x_i2c_read,
    .write_with_attrs = tc4x_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void tc4x_i2c_reset_hold(Object *obj, ResetType type)
{
    Tc4xI2c *c = TC4X_I2C(obj);

    c->phase = XFER_IDLE;
    c->dtr = 0;
    c->pirq = 0;
    c->errirq = 0;
    c->tps = 0;
    c->mrps = 0;
    c->len = 0;
    c->pos = 0;
}

static void tc4x_i2c_init(Object *obj)
{
    Tc4xI2c *c = TC4X_I2C(obj);

    memory_region_init_io(&c->mmio, obj, &tc4x_i2c_ops, c, "i2c.regs",
                          TC4X_I2C_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &c->mmio);
    c->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void tc4x_i2c_realize(DeviceState *dev, Error **errp)
{
    Tc4xI2c *c = TC4X_I2C(dev);

    if (c->index >= TC4X_I2C_COUNT) {
        error_setg(errp, "tc4x-i2c: index %u is not below %u", c->index,
                   TC4X_I2C_COUNT);
        return;
    }
    if (!c->sfr) {
        error_setg(errp, "tc4x-i2c: link sfr-store is not set");
        return;
    }
    memory_region_set_imprecise_io(&c->mmio, c->imprecise_io);
    tc4x_sfr_store_add_passthrough(c->sfr, &c->mmio, tc4x_i2c_serves, c);
}

static const Property tc4x_i2c_properties[] = {
    DEFINE_PROP_UINT32("index", Tc4xI2c, index, UINT32_MAX),
    DEFINE_PROP_BOOL("imprecise-io", Tc4xI2c, imprecise_io, false),
    DEFINE_PROP_LINK("sfr-store", Tc4xI2c, sfr, TYPE_TC4X_SFR_STORE,
                     Tc4xSfrStore *),
};

static const VMStateDescription vmstate_tc4x_i2c = {
    .name = TYPE_TC4X_I2C,
    .unmigratable = 1,
};

static void tc4x_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "TC4x I2C controller";
    dc->realize = tc4x_i2c_realize;
    dc->vmsd = &vmstate_tc4x_i2c;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_i2c_reset_hold;
    device_class_set_props(dc, tc4x_i2c_properties);
}

static int tc4x_i2c_legacy_event(I2CSlave *s, enum i2c_event event)
{
    Tc4xI2cLegacy *l = TC4X_I2C_LEGACY(s);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        if (l->ops->start && !l->ops->start(l->opaque,
                                            event == I2C_START_RECV)) {
            return -1;
        }
        l->started = true;
        l->sending = event == I2C_START_SEND;
        l->len = 0;
        return 0;
    case I2C_FINISH:
        if (l->started) {
            if (l->sending) {
                l->ops->write(l->opaque, l->buf, l->len);
            }
            if (l->ops->stop) {
                l->ops->stop(l->opaque);
            }
            l->started = false;
        }
        return 0;
    default:
        return 0;
    }
}

static int tc4x_i2c_legacy_send(I2CSlave *s, uint8_t data)
{
    Tc4xI2cLegacy *l = TC4X_I2C_LEGACY(s);

    if (l->len >= sizeof(l->buf)) {
        return -1;
    }
    l->buf[l->len++] = data;
    return 0;
}

/* Masters other than the TC4x controller read byte by byte */
static uint8_t tc4x_i2c_legacy_recv(I2CSlave *s)
{
    Tc4xI2cLegacy *l = TC4X_I2C_LEGACY(s);
    uint8_t data;

    l->ops->read(l->opaque, &data, 1);
    return data;
}

static const VMStateDescription vmstate_tc4x_i2c_legacy = {
    .name = TYPE_TC4X_I2C_LEGACY,
    .unmigratable = 1,
};

static void tc4x_i2c_legacy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "TC4x board I2C slave (Tc4xI2cSlaveOps)";
    /* created by tc4x_i2c_attach() */
    dc->user_creatable = false;
    dc->vmsd = &vmstate_tc4x_i2c_legacy;
    sc->event = tc4x_i2c_legacy_event;
    sc->send = tc4x_i2c_legacy_send;
    sc->recv = tc4x_i2c_legacy_recv;
}

void tc4x_i2c_attach(Tc4xSoc *soc, unsigned bus, uint8_t addr,
                     const Tc4xI2cSlaveOps *ops, void *opaque)
{
    BusChild *kid;
    Tc4xI2c *c;
    I2CBus *b;
    I2CSlave *dev;
    Tc4xI2cLegacy *l;

    if (bus >= TC4X_I2C_COUNT || addr > 0x7F) {
        error_setg(&error_fatal, "tc4x-i2c: no slave address 0x%02x on I2C%u",
                   addr, bus);
        return;
    }
    c = tc4x_soc_i2c(soc, bus);
    b = c->bus;
    QTAILQ_FOREACH(kid, &BUS(b)->children, sibling) {
        I2CSlave *s = I2C_SLAVE(kid->child);

        if (s->address != addr) {
            continue;
        }
        l = (Tc4xI2cLegacy *)object_dynamic_cast(OBJECT(s),
                                                 TYPE_TC4X_I2C_LEGACY);
        if (!l) {
            g_autofree char *path = object_get_canonical_path(OBJECT(s));

            error_setg(&error_fatal, "tc4x-i2c: I2C%u address 0x%02x is "
                       "taken by %s", bus, addr, path);
            return;
        }
        l->ops = ops;
        l->opaque = opaque;
        return;
    }
    dev = i2c_slave_new(TYPE_TC4X_I2C_LEGACY, addr);
    l = TC4X_I2C_LEGACY(dev);
    l->ops = ops;
    l->opaque = opaque;
    object_property_add_child(OBJECT(c), "legacy[*]", OBJECT(dev));
    i2c_slave_realize_and_unref(dev, b, &error_fatal);
}

static const TypeInfo tc4x_i2c_types[] = {
    {
        .name = TYPE_TC4X_I2C,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xI2c),
        .instance_init = tc4x_i2c_init,
        .class_init = tc4x_i2c_class_init,
    }, {
        .name = TYPE_TC4X_I2C_LEGACY,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Tc4xI2cLegacy),
        .class_init = tc4x_i2c_legacy_class_init,
    },
};

DEFINE_TYPES(tc4x_i2c_types)
