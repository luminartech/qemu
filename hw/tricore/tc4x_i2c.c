/*
 * TC4x I2C controllers (I2C0..2).
 *
 * The model follows the register sequences of a FIFO-driven I2C driver:
 * a transfer completes instantly, the FIFO is serviced through the
 * single/last-single request bits in RIS, the end of a transfer is reported
 * in PIRQSS (RX, TX_END, NACK) and the bus is always idle in between.
 *
 *   write: TPSCTRL=N, then words to TXD (address byte first), each followed
 *          by an ICR clear of the request bits; slave gets N-1 bytes, then
 *          TX_END (+NACK when no slave answers the address).
 *   read:  TPSCTRL=1, MRPSCTRL=M, address|1 to TXD -> RX; every ICR clear
 *          re-arms a request until all M bytes were read from RXD, then
 *          TX_END.
 *
 * Slaves are attached by board code per bus and address (tc4x_i2c_attach);
 * an address without one NACKs.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "tc4x_soc.h"
#include "trace.h"

#define I2C_COUNT           3u
#define I2C_SLAVES_MAX      16u

#define I2C_BUSSTAT         0x24u
#define I2C_FIFOCFG         0x28u
#define I2C_MRPSCTRL        0x2Cu
#define I2C_RPSSTAT         0x30u
#define I2C_TPSCTRL         0x34u
#define I2C_FFSSTAT         0x38u
#define I2C_ERRIRQSM        0x60u
#define I2C_ERRIRQSS        0x64u
#define I2C_ERRIRQSC        0x68u
#define I2C_PIRQSM          0x70u
#define I2C_PIRQSS          0x74u
#define I2C_PIRQSC          0x78u
#define I2C_RIS             0x80u
#define I2C_MIS             0x88u
#define I2C_ICR             0x8Cu
#define I2C_TXD             0x8000u
#define I2C_RXD             0xC000u
#define I2C_WINDOW          0x10000u

#define RIS_LSREQ           BIT(0)
#define RIS_SREQ            BIT(1)
#define RIS_DTR_MASK        0x0Fu
#define RIS_ERR             BIT(4)
#define RIS_P               BIT(5)

#define P_NACK              BIT(4)
#define P_TX_END            BIT(5)
#define P_RX                BIT(6)

#define TPS_MASK            0x3FFFu
#define I2C_XFER_MAX        512u

static const uint32_t i2c_base[I2C_COUNT] = {
    0xF44C0000u, 0xF44E0000u, 0xF4500000u
};

typedef struct I2cSlave {
    uint8_t addr;
    const Tc4xI2cSlaveOps *ops;
    void *opaque;
} I2cSlave;

typedef enum {
    XFER_IDLE,
    XFER_TX,        /* collecting TPS bytes from TXD */
    XFER_RX,        /* M bytes ready in rx[] */
} XferPhase;

typedef struct I2cBus {
    XferPhase phase;
    uint32_t dtr;           /* RIS request bits */
    uint32_t pirq;          /* PIRQSS */
    uint32_t errirq;        /* ERRIRQSS */
    uint32_t tps, mrps;
    uint8_t buf[I2C_XFER_MAX];
    uint32_t len;           /* TX bytes collected / RX bytes available */
    uint32_t pos;           /* RX bytes handed out */
    I2cSlave slaves[I2C_SLAVES_MAX];
    unsigned nslaves;
} I2cBus;

struct Tc4xI2c {
    Tc4xSoc *soc;
    I2cBus bus[I2C_COUNT];
};

static bool i2c_decode(uint32_t addr, unsigned *n, uint32_t *off)
{
    for (unsigned i = 0; i < I2C_COUNT; i++) {
        if (addr >= i2c_base[i] && addr < i2c_base[i] + I2C_WINDOW) {
            *n = i;
            *off = addr - i2c_base[i];
            return true;
        }
    }
    return false;
}

bool tc4x_i2c_owns(uint32_t addr)
{
    unsigned n;
    uint32_t off;

    return i2c_decode(addr, &n, &off);
}

static I2cSlave *i2c_slave(I2cBus *b, unsigned addr)
{
    for (unsigned i = 0; i < b->nslaves; i++) {
        if (b->slaves[i].addr == addr) {
            return &b->slaves[i];
        }
    }
    return NULL;
}

static unsigned bytes_per_word(uint32_t fifocfg, unsigned shift)
{
    return 4u >> extract32(fifocfg, shift, 2);
}

/* Request the next FIFO word (single or last single request) */
static void i2c_request(Tc4xI2c *c, unsigned n, uint32_t remaining)
{
    I2cBus *b = &c->bus[n];
    uint32_t fifocfg = tc4x_reg_get(c->soc, i2c_base[n] + I2C_FIFOCFG);
    unsigned per_word = bytes_per_word(fifocfg,
                                       b->phase == XFER_RX ? 8 : 12);

    b->dtr |= remaining <= per_word ? RIS_LSREQ : RIS_SREQ;
}

static void i2c_finish_tx(Tc4xI2c *c, unsigned n)
{
    I2cBus *b = &c->bus[n];
    unsigned addr = b->buf[0] >> 1;
    I2cSlave *sl = i2c_slave(b, addr);
    bool read = b->buf[0] & 1;

    if (!sl || (sl->ops->start && !sl->ops->start(sl->opaque, read))) {
        trace_tc4x_i2c_transfer(n, addr, read, b->len - 1, false);
        b->pirq |= P_NACK | P_TX_END;
        b->phase = XFER_IDLE;
        return;
    }
    if (read && b->mrps) {
        b->len = MIN(b->mrps, I2C_XFER_MAX);
        b->pos = 0;
        sl->ops->read(sl->opaque, b->buf, b->len);
        if (sl->ops->stop) {
            sl->ops->stop(sl->opaque);
        }
        trace_tc4x_i2c_transfer(n, addr, true, b->len, true);
        b->phase = XFER_RX;
        b->pirq |= P_RX;
        return;
    }
    if (!read) {
        sl->ops->write(sl->opaque, b->buf + 1, b->len - 1);
        trace_tc4x_i2c_transfer(n, addr, false, b->len - 1, true);
    }
    if (sl->ops->stop) {
        sl->ops->stop(sl->opaque);
    }
    b->pirq |= P_TX_END;
    b->phase = XFER_IDLE;
}

static void i2c_txd(Tc4xI2c *c, unsigned n, uint32_t val)
{
    I2cBus *b = &c->bus[n];
    uint32_t fifocfg = tc4x_reg_get(c->soc, i2c_base[n] + I2C_FIFOCFG);
    unsigned step = 1u << extract32(fifocfg, 12, 2);

    if (b->phase != XFER_TX) {
        qemu_log_mask(LOG_GUEST_ERROR, "tc4x-i2c%u: TXD outside a transfer\n",
                      n);
        return;
    }
    for (unsigned i = 0; i < 4 && b->len < b->tps; i += step) {
        if (b->len < I2C_XFER_MAX) {
            b->buf[b->len] = val >> (8 * i);
        }
        b->len++;
    }
    if (b->len >= b->tps) {
        b->len = MIN(b->len, I2C_XFER_MAX);
        i2c_finish_tx(c, n);
    }
}

static uint32_t i2c_rxd(Tc4xI2c *c, unsigned n)
{
    I2cBus *b = &c->bus[n];
    uint32_t fifocfg = tc4x_reg_get(c->soc, i2c_base[n] + I2C_FIFOCFG);
    unsigned step = 1u << extract32(fifocfg, 8, 2);
    uint32_t word = 0;

    if (b->phase != XFER_RX) {
        return 0;
    }
    for (unsigned i = 0; i < 4 && b->pos < b->len; i += step) {
        word |= (uint32_t)b->buf[b->pos++] << (8 * i);
    }
    return word;
}

/* The driver acknowledged a FIFO request: ask for the next word or finish */
static void i2c_icr(Tc4xI2c *c, unsigned n, uint32_t val)
{
    I2cBus *b = &c->bus[n];

    b->dtr &= ~(val & RIS_DTR_MASK);
    if (b->phase == XFER_TX && b->len < b->tps) {
        i2c_request(c, n, b->tps - b->len);
    } else if (b->phase == XFER_RX) {
        if (b->pos < b->len) {
            i2c_request(c, n, b->len - b->pos);
        } else {
            b->phase = XFER_IDLE;
            b->pirq |= P_TX_END;
        }
    }
}

bool tc4x_i2c_read(Tc4xI2c *c, uint32_t addr, uint32_t *val)
{
    unsigned n;
    uint32_t off;
    I2cBus *b;

    if (!i2c_decode(addr, &n, &off)) {
        return false;
    }
    b = &c->bus[n];
    switch (off) {
    case I2C_RIS:
    case I2C_MIS:
        /*
         * P_INT / ERR_INT are the PIRQSS / ERRIRQSS bits enabled in their
         * masks
         */
        *val = b->dtr |
               (b->errirq & tc4x_reg_get(c->soc, i2c_base[n] + I2C_ERRIRQSM) ?
                RIS_ERR : 0) |
               (b->pirq & tc4x_reg_get(c->soc, i2c_base[n] + I2C_PIRQSM) ?
                RIS_P : 0);
        return true;
    case I2C_PIRQSS:
        *val = b->pirq;
        return true;
    case I2C_ERRIRQSS:
        *val = b->errirq;
        return true;
    case I2C_BUSSTAT:
        *val = 0;   /* idle */
        return true;
    case I2C_RPSSTAT:
        *val = b->phase == XFER_RX ? b->pos : 0;
        return true;
    case I2C_FFSSTAT:
        *val = b->phase == XFER_RX ? b->len - b->pos : 0;
        return true;
    default:
        if (off >= I2C_RXD) {
            *val = i2c_rxd(c, n);
            return true;
        }
        if (off >= I2C_TXD) {
            *val = 0;
            return true;
        }
        return false;
    }
}

bool tc4x_i2c_write(Tc4xI2c *c, uint32_t addr, uint32_t val)
{
    unsigned n;
    uint32_t off;
    I2cBus *b;

    if (!i2c_decode(addr, &n, &off)) {
        return false;
    }
    b = &c->bus[n];
    switch (off) {
    case I2C_TPSCTRL:
        b->tps = val & TPS_MASK;
        b->len = 0;
        b->pos = 0;
        b->phase = b->tps ? XFER_TX : XFER_IDLE;
        b->dtr = 0;
        if (b->tps) {
            i2c_request(c, n, b->tps);
        }
        tc4x_reg_set(c->soc, addr, val);
        return true;
    case I2C_MRPSCTRL:
        b->mrps = val & TPS_MASK;
        tc4x_reg_set(c->soc, addr, val);
        return true;
    case I2C_ICR:
        i2c_icr(c, n, val);
        return true;
    case I2C_PIRQSC:
        b->pirq &= ~val;
        return true;
    case I2C_ERRIRQSC:
        b->errirq &= ~val;
        return true;
    default:
        if (off >= I2C_TXD && off < I2C_RXD) {
            i2c_txd(c, n, val);
            return true;
        }
        if (off >= I2C_RXD) {
            return true;
        }
        return false;
    }
}

void tc4x_i2c_attach(Tc4xSoc *soc, unsigned bus, uint8_t addr,
                     const Tc4xI2cSlaveOps *ops, void *opaque)
{
    I2cBus *b = &tc4x_soc_i2c(soc)->bus[bus % I2C_COUNT];
    I2cSlave *sl = i2c_slave(b, addr);

    if (!sl) {
        assert(b->nslaves < I2C_SLAVES_MAX);
        sl = &b->slaves[b->nslaves++];
    }
    sl->addr = addr;
    sl->ops = ops;
    sl->opaque = opaque;
}

Tc4xI2c *tc4x_i2c_new(Tc4xSoc *soc)
{
    Tc4xI2c *c = g_new0(Tc4xI2c, 1);

    c->soc = soc;
    return c;
}
