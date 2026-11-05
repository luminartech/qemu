/*
 * TC4x QSPI modules (QSPI0..7).
 *
 * A transfer completes at once: a BACONENTRY write starts a frame on the
 * chip select it names and every DATAENTRY word is clocked through the
 * selected peripheral, its reply pushed to the receive FIFO.  A word under
 * a BACON entry with LAST set ends the frame and releases the chip select;
 * the next word starts a frame with the same entry.  The transmit FIFO is
 * therefore always empty and the phase transitions done.  A frame driven
 * by a CPU raises PT at its end when GLOBALCON1 enables a phase transition
 * event that the frame passes.  With
 * TXEN / RXEN set and the service request routed to a CPU, each word raises
 * TX (FIFO empty again) and RX (word received), as an interrupt-driven SPI
 * master expects.
 *
 * DMA-driven jobs: transmit and receive DMA channels wait for the module's
 * hardware requests, and the BACONENTRY write starts the job.  The DMA runs
 * the transmit lists (feeding BACONENTRY and DATAENTRY), then the receive
 * lists (draining RXEXIT), and the job ends with phase transition 2 or,
 * for frame-based jobs, the move counter (MCCON.IALF).
 *
 * Register layout: Ifx_QSPI_* of the iLLD SFR headers (tc4x_regs.h).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_ir.h"
#include "hw/tricore/tc4x_qspi.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "tc4x_soc.h"
#include "trace.h"

#define QSPI_REG(r)         (A_QSPI0_##r - A_QSPI0_CLC)
#define QSPI_STRIDE         (A_QSPI1_CLC - A_QSPI0_CLC)
#define QSPI_SRC_STRIDE     (A_SRC_QSPI1_TX - A_SRC_QSPI0_TX)
#define QSPI_RXLEVEL_MAX    15u

/*
 * Locking: the state below the lock is changed under it.  rx_level is
 * also read without it (STATUS).  dma and dma_opaque are set during
 * realize of the SoC only.
 */
struct Tc4xQspi {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    SSIBus *bus;
    qemu_irq cs[TC4X_QSPI_CS_COUNT];
    qemu_irq irq[TC4X_QSPI_IRQ_COUNT];
    Tc4xSfrStore *sfr;
    Tc4xIr *ir;
    uint32_t index;
    bool imprecise_io;
    const Tc4xQspiDmaOps *dma;
    void *dma_opaque;
    /* DMA jobs run under the BQL: kicks from writes without it wait here */
    QEMUBH *kick_bh;

    /* the bus lock */
    QemuRecMutex lock;
    GQueue *rx;             /* receive FIFO */
    uint32_t rx_level;      /* length of rx */
    int sel;                /* chip select of the frame, -1: none */
    uint32_t bacon;         /* BACON entry of the frames */
    bool bacon_valid;       /* a BACON entry was written since reset */
    bool rx_fed;            /* receive data fed by board code */
    bool dma_busy;          /* hardware-request DMA job in progress */
};

/*
 * The Tc4xQspiSlaveOps on the bus: frame() and transfer() call the
 * board's frame and word ops
 */
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xQspiLegacy, TC4X_QSPI_LEGACY)

struct Tc4xQspiLegacy {
    Tc4xQspiPeripheral parent_obj;

    const Tc4xQspiSlaveOps *ops;
    void *opaque;
};

uint32_t tc4x_qspi_base(unsigned n)
{
    return A_QSPI0_CLC + n * QSPI_STRIDE;
}

uint32_t tc4x_qspi_window(void)
{
    return QSPI_STRIDE;
}

SSIBus *tc4x_qspi_bus(Tc4xQspi *q)
{
    return q->bus;
}

static Tc4xQspi *qspi_of_bus(SSIBus *bus)
{
    return TC4X_QSPI(BUS(bus)->parent);
}

void tc4x_qspi_bus_lock(SSIBus *bus)
{
    qemu_rec_mutex_lock(&qspi_of_bus(bus)->lock);
}

void tc4x_qspi_bus_unlock(SSIBus *bus)
{
    qemu_rec_mutex_unlock(&qspi_of_bus(bus)->lock);
}

void tc4x_qspi_set_dma(Tc4xQspi *q, const Tc4xQspiDmaOps *ops, void *opaque)
{
    q->dma = ops;
    q->dma_opaque = opaque;
}

static uint32_t qspi_store_get(Tc4xQspi *q, uint32_t off)
{
    return tc4x_sfr_store_get(q->sfr, tc4x_qspi_base(q->index) + off);
}

static void qspi_store_set(Tc4xQspi *q, uint32_t off, uint32_t val)
{
    tc4x_sfr_store_set(q->sfr, tc4x_qspi_base(q->index) + off, val);
}

/* Service request @irq if its node is enabled and routed to a CPU */
static void qspi_cpu_event(Tc4xQspi *q, unsigned irq)
{
    uint32_t src = tc4x_ir_src_get(q->ir, A_SRC_QSPI0_TX +
                                   q->index * QSPI_SRC_STRIDE + 4 * irq);

    if ((src & R_SRC_SRCR_SRE_MASK) &&
        FIELD_EX32(src, SRC_SRCR, TOS) < TC4X_IR_NUM_CPUS) {
        qemu_irq_raise(q->irq[irq]);
    }
}

/* FIFO events of a word as CPU interrupts; DMA requests are left to kick */
static void qspi_word_events(Tc4xQspi *q)
{
    uint32_t con1 = qspi_store_get(q, QSPI_REG(GLOBALCON1));

    if (FIELD_EX32(con1, QSPI_GLOBALCON1, TXEN)) {
        qspi_cpu_event(q, TC4X_QSPI_IRQ_TX);
    }
    if (FIELD_EX32(con1, QSPI_GLOBALCON1, RXEN)) {
        qspi_cpu_event(q, TC4X_QSPI_IRQ_RX);
    }
}

static bool qspi_dma_armed(Tc4xQspi *q)
{
    return q->dma && q->dma->armed(q->dma_opaque, q->index);
}

/* A DMA job of the module's hardware requests, under the BQL */
static void qspi_dma_kick(Tc4xQspi *q)
{
    uint32_t mccon;
    bool ran;

    if (q->dma_busy || q->rx_fed || !q->dma) {
        return;     /* written by the transmit list itself / fed by a board */
    }
    if (!bql_locked()) {
        if (qspi_dma_armed(q)) {
            qemu_bh_schedule(q->kick_bh);
        }
        return;
    }
    q->dma_busy = true;
    ran = q->dma->run(q->dma_opaque, q->index);
    trace_tc4x_qspi_dma_job(q->index, ran);
    if (ran) {
        mccon = qspi_store_get(q, QSPI_REG(MCCON));
        if (FIELD_EX32(qspi_store_get(q, QSPI_REG(GLOBALCON1)),
                       QSPI_GLOBALCON1, PT2EN)) {
            qemu_irq_raise(q->irq[TC4X_QSPI_IRQ_PT]);
        } else if (FIELD_EX32(mccon, QSPI_MCCON, IALEN)) {
            qspi_store_set(q, QSPI_REG(MCCON),
                           mccon | R_QSPI_MCCON_IALF_MASK);
            qemu_irq_raise(q->irq[TC4X_QSPI_IRQ_U]);
        }
    }
    q->dma_busy = false;
}

static void qspi_kick_bh(void *opaque)
{
    Tc4xQspi *q = opaque;

    QEMU_LOCK_GUARD(&q->lock);
    qspi_dma_kick(q);
}

/* A frame starts on the chip select the BACON entry names */
static void qspi_frame_start(Tc4xQspi *q)
{
    uint32_t bacon = q->bacon;
    int cs = FIELD_EX32(bacon, QSPI_BACONENTRY, CS);
    DeviceState *dev;
    Tc4xQspiPeripheral *p;
    Tc4xQspiPeripheralClass *pc;

    if (q->sel != cs) {
        if (q->sel >= 0) {
            qemu_irq_raise(q->cs[q->sel]);
        }
        q->sel = cs;
        qemu_irq_lower(q->cs[cs]);
    }
    trace_tc4x_qspi_frame(q->index, cs, bacon);
    dev = ssi_get_cs(q->bus, cs);
    p = dev ? (Tc4xQspiPeripheral *)object_dynamic_cast(
                  OBJECT(dev), TYPE_TC4X_QSPI_PERIPHERAL) : NULL;
    pc = p ? TC4X_QSPI_PERIPHERAL_GET_CLASS(p) : NULL;
    if (pc && pc->frame) {
        pc->frame(p, bacon);
    }
}

/* BACONENTRY starts a frame; its entry holds for the frames that follow */
static void qspi_bacon(Tc4xQspi *q, uint32_t bacon)
{
    q->bacon = bacon;
    q->bacon_valid = true;
    qspi_frame_start(q);
}

/*
 * Phase transition events of GLOBALCON1.PT1/PT2 that every frame passes
 * (IfxQspi_PhaseTransitionEvent of the iLLD: end of wait, start of frame,
 * transmit buffer emptied, receive buffer filled, end of frame); the
 * others (clock polarity change, expect phase) have no frame-end meaning
 */
static bool qspi_pt_in_frame(uint32_t event)
{
    return event == 0 || (event >= 2 && event <= 5);
}

/*
 * The last word of a frame (BACON.LAST) releases the chip select.  On the
 * CPU path (no hardware-request DMA job, whose end raises PT itself) the
 * enabled phase transition events of the frame raise PT.
 */
static void qspi_frame_end(Tc4xQspi *q)
{
    uint32_t con1 = qspi_store_get(q, QSPI_REG(GLOBALCON1));
    bool pt = (FIELD_EX32(con1, QSPI_GLOBALCON1, PT1EN) &&
               qspi_pt_in_frame(FIELD_EX32(con1, QSPI_GLOBALCON1, PT1))) ||
              (FIELD_EX32(con1, QSPI_GLOBALCON1, PT2EN) &&
               qspi_pt_in_frame(FIELD_EX32(con1, QSPI_GLOBALCON1, PT2)));

    trace_tc4x_qspi_frame_end(q->index, q->sel, pt);
    qemu_irq_raise(q->cs[q->sel]);
    q->sel = -1;
    if (pt && !q->dma_busy && !q->rx_fed && !qspi_dma_armed(q)) {
        qspi_cpu_event(q, TC4X_QSPI_IRQ_PT);
    }
}

static void qspi_transmit(Tc4xQspi *q, uint32_t val)
{
    uint32_t reply;

    if (q->sel < 0 && q->bacon_valid) {
        qspi_frame_start(q);
    }
    reply = ssi_transfer(q->bus, val);
    g_queue_push_tail(q->rx, GUINT_TO_POINTER(reply));
    qatomic_set(&q->rx_level, g_queue_get_length(q->rx));
    qspi_word_events(q);
    if (q->sel >= 0 && FIELD_EX32(q->bacon, QSPI_BACONENTRY, LAST)) {
        qspi_frame_end(q);
    }
}

static uint32_t qspi_rx_pop(Tc4xQspi *q)
{
    uint32_t v;

    QEMU_LOCK_GUARD(&q->lock);
    v = g_queue_is_empty(q->rx) ? 0 : GPOINTER_TO_UINT(g_queue_pop_head(q->rx));
    qatomic_set(&q->rx_level, g_queue_get_length(q->rx));
    return v;
}

static uint32_t qspi_status(Tc4xQspi *q)
{
    uint32_t level = MIN(qatomic_read(&q->rx_level), QSPI_RXLEVEL_MAX);
    uint32_t v = qspi_store_get(q, QSPI_REG(STATUS));

    v = FIELD_DP32(v, QSPI_STATUS, TXFIFOLEVEL, 0);
    v = FIELD_DP32(v, QSPI_STATUS, RXFIFOLEVEL, level);
    v = FIELD_DP32(v, QSPI_STATUS, RXF, level != 0);
    return v | R_QSPI_STATUS_TXF_MASK | R_QSPI_STATUS_PT1F_MASK |
           R_QSPI_STATUS_PT2F_MASK;
}

static bool qspi_serves_write(hwaddr off)
{
    return off == QSPI_REG(BACONENTRY) || off == QSPI_REG(MCCON) ||
           (off >= QSPI_REG(DATAENTRY0) && off < QSPI_REG(RXEXIT0));
}

/* A full-word write, under the bus lock */
static void qspi_write_word(Tc4xQspi *q, hwaddr off, uint32_t val)
{
    uint32_t flags;

    if (off >= QSPI_REG(DATAENTRY0) && off < QSPI_REG(RXEXIT0)) {
        qspi_transmit(q, val);
        return;
    }
    if (off == QSPI_REG(MCCON)) {
        flags = qspi_store_get(q, off) & R_QSPI_MCCON_IALF_MASK;
        if (val & R_QSPI_MCCON_IALC_MASK) {
            flags = 0;
        }
        if (val & R_QSPI_MCCON_IALS_MASK) {
            flags = R_QSPI_MCCON_IALF_MASK;
        }
        qspi_store_set(q, off, (val & ~(R_QSPI_MCCON_IALF_MASK |
                                        R_QSPI_MCCON_IALC_MASK |
                                        R_QSPI_MCCON_IALS_MASK)) | flags);
        return;
    }
    if (off == QSPI_REG(BACONENTRY)) {
        qspi_bacon(q, val);
        qspi_dma_kick(q);
    }
    tc4x_sfr_store_write(q->sfr, tc4x_qspi_base(q->index) + off, val);
}

static uint64_t tc4x_qspi_read(void *opaque, hwaddr off, unsigned size)
{
    Tc4xQspi *q = opaque;
    hwaddr word = off & ~3u;
    uint32_t val;

    if (word == QSPI_REG(STATUS)) {
        val = qspi_status(q);
    } else if (word >= QSPI_REG(RXEXIT0) && word < QSPI_REG(RXEXITD)) {
        val = qspi_rx_pop(q);
    } else {
        return tc4x_sfr_store_access_read(q->sfr,
                                          tc4x_qspi_base(q->index) + off,
                                          size);
    }
    return extract32(val, (off & 3) * 8, size * 8);
}

/*
 * Writes run on the vCPU thread under the bus lock, so that a core may
 * drive a peripheral word by word without the BQL.  They take the BQL
 * when a hardware-request DMA job may run on the module (the DMA needs
 * it), and leave registers the store watches or hooks to the store.
 */
static void tc4x_qspi_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    Tc4xQspi *q = opaque;
    uint32_t addr = tc4x_qspi_base(q->index) + off;
    bool served = qspi_serves_write(off & ~3u);

    if (size != 4 || (off & 3)) {
        if (served) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tc4x-qspi%u: %u-byte write at 0x%" HWADDR_PRIx
                          " ignored\n", q->index, size, off);
        } else {
            tc4x_sfr_store_access_write(q->sfr, addr, val, size);
        }
        return;
    }
    if (!served && tc4x_sfr_store_bql_only(q->sfr, addr)) {
        tc4x_sfr_store_access_write(q->sfr, addr, val, size);
        return;
    }
    if (!bql_locked()) {
        WITH_QEMU_LOCK_GUARD(&q->lock) {
            if (!q->dma_busy && !qspi_dma_armed(q)) {
                qspi_write_word(q, off, val);
                return;
            }
        }
        bql_lock();
        WITH_QEMU_LOCK_GUARD(&q->lock) {
            qspi_write_word(q, off, val);
        }
        bql_unlock();
        return;
    }
    QEMU_LOCK_GUARD(&q->lock);
    qspi_write_word(q, off, val);
}

static const MemoryRegionOps tc4x_qspi_ops = {
    .read = tc4x_qspi_read,
    .write = tc4x_qspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void tc4x_qspi_reset_hold(Object *obj, ResetType type)
{
    Tc4xQspi *q = TC4X_QSPI(obj);

    QEMU_LOCK_GUARD(&q->lock);
    g_queue_clear(q->rx);
    qatomic_set(&q->rx_level, 0);
    q->sel = -1;
    q->bacon = 0;
    q->bacon_valid = false;
    q->dma_busy = false;
    for (unsigned cs = 0; cs < TC4X_QSPI_CS_COUNT; cs++) {
        qemu_irq_raise(q->cs[cs]);
    }
}

/*
 * The bus runs frame(), transfer() and the chip select lines on vCPU
 * threads without the BQL: only peripherals that keep the bus lock
 * contract of TYPE_TC4X_QSPI_PERIPHERAL may be on it
 */
static bool tc4x_qspi_bus_check_address(BusState *b, DeviceState *dev,
                                        Error **errp)
{
    BusClass *ssi = BUS_CLASS(object_class_by_name(TYPE_SSI_BUS));

    if (!object_dynamic_cast(OBJECT(dev), TYPE_TC4X_QSPI_PERIPHERAL)) {
        error_setg(errp, "%s: %s is not a %s; the bus runs without the BQL",
                   b->name, object_get_typename(OBJECT(dev)),
                   TYPE_TC4X_QSPI_PERIPHERAL);
        return false;
    }
    return ssi->check_address(b, dev, errp);
}

static void tc4x_qspi_bus_class_init(ObjectClass *oc, const void *data)
{
    BUS_CLASS(oc)->check_address = tc4x_qspi_bus_check_address;
}

static void tc4x_qspi_init(Object *obj)
{
    Tc4xQspi *q = TC4X_QSPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&q->mmio, obj, &tc4x_qspi_ops, q, "qspi.regs",
                          QSPI_STRIDE);
    sysbus_init_mmio(sbd, &q->mmio);
    for (unsigned i = 0; i < TC4X_QSPI_IRQ_COUNT; i++) {
        sysbus_init_irq(sbd, &q->irq[i]);
    }
    qdev_init_gpio_out_named(DEVICE(obj), q->cs, "cs", TC4X_QSPI_CS_COUNT);
    q->bus = SSI_BUS(qbus_new(TYPE_TC4X_QSPI_BUS, DEVICE(obj), "ssi"));
    qemu_rec_mutex_init(&q->lock);
    q->rx = g_queue_new();
    q->sel = -1;
}

static void tc4x_qspi_finalize(Object *obj)
{
    Tc4xQspi *q = TC4X_QSPI(obj);

    g_queue_free(q->rx);
    qemu_rec_mutex_destroy(&q->lock);
}

static void tc4x_qspi_realize(DeviceState *dev, Error **errp)
{
    Tc4xQspi *q = TC4X_QSPI(dev);

    if (q->index >= TC4X_QSPI_COUNT) {
        error_setg(errp, "tc4x-qspi: index %u is not below %u", q->index,
                   TC4X_QSPI_COUNT);
        return;
    }
    if (!q->sfr || !q->ir) {
        error_setg(errp, "tc4x-qspi: links sfr-store and ir must be set");
        return;
    }
    memory_region_enable_lockless_io(&q->mmio);
    memory_region_set_imprecise_io(&q->mmio, q->imprecise_io);
    tc4x_sfr_store_add_passthrough(q->sfr, &q->mmio);
    q->kick_bh = qemu_bh_new_guarded(qspi_kick_bh, q,
                                     &dev->mem_reentrancy_guard);
}

static void tc4x_qspi_unrealize(DeviceState *dev)
{
    Tc4xQspi *q = TC4X_QSPI(dev);

    qemu_bh_delete(q->kick_bh);
}

static const Property tc4x_qspi_properties[] = {
    DEFINE_PROP_UINT32("index", Tc4xQspi, index, UINT32_MAX),
    DEFINE_PROP_BOOL("imprecise-io", Tc4xQspi, imprecise_io, false),
    DEFINE_PROP_LINK("sfr-store", Tc4xQspi, sfr, TYPE_TC4X_SFR_STORE,
                     Tc4xSfrStore *),
    DEFINE_PROP_LINK("ir", Tc4xQspi, ir, TYPE_TC4X_IR, Tc4xIr *),
};

static const VMStateDescription vmstate_tc4x_qspi = {
    .name = TYPE_TC4X_QSPI,
    .unmigratable = 1,
};

static void tc4x_qspi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "TC4x QSPI module";
    dc->realize = tc4x_qspi_realize;
    dc->unrealize = tc4x_qspi_unrealize;
    dc->vmsd = &vmstate_tc4x_qspi;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_qspi_reset_hold;
    device_class_set_props(dc, tc4x_qspi_properties);
}

bool tc4x_qspi_realize_peripheral(Tc4xQspi *q, DeviceState *dev,
                                  Error **errp)
{
    uint8_t cs = SSI_PERIPHERAL(dev)->cs_index;

    if (cs >= TC4X_QSPI_CS_COUNT) {
        error_setg(errp, "tc4x-qspi%u: no chip select %u", q->index, cs);
        return false;
    }
    if (!ssi_realize_and_unref(dev, q->bus, errp)) {
        return false;
    }
    qdev_connect_gpio_out_named(DEVICE(q), "cs", cs,
                                qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0));
    QEMU_LOCK_GUARD(&q->lock);
    qemu_set_irq(q->cs[cs], q->sel != cs);
    return true;
}

static void tc4x_qspi_peripheral_realize(SSIPeripheral *s, Error **errp)
{
}

static void tc4x_qspi_peripheral_class_init(ObjectClass *oc, const void *data)
{
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);

    sc->realize = tc4x_qspi_peripheral_realize;
    sc->cs_polarity = SSI_CS_LOW;
}

static void tc4x_qspi_legacy_frame(Tc4xQspiPeripheral *p, uint32_t bacon)
{
    Tc4xQspiLegacy *l = TC4X_QSPI_LEGACY(p);

    l->ops->frame(l->opaque, bacon);
}

static uint32_t tc4x_qspi_legacy_transfer(SSIPeripheral *s, uint32_t val)
{
    Tc4xQspiLegacy *l = TC4X_QSPI_LEGACY(s);

    return l->ops->word(l->opaque, val);
}

static void tc4x_qspi_legacy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    Tc4xQspiPeripheralClass *pc = TC4X_QSPI_PERIPHERAL_CLASS(oc);

    dc->desc = "TC4x board QSPI slave (Tc4xQspiSlaveOps)";
    /* created by tc4x_qspi_attach() */
    dc->user_creatable = false;
    sc->transfer = tc4x_qspi_legacy_transfer;
    pc->frame = tc4x_qspi_legacy_frame;
}

void tc4x_qspi_attach(Tc4xSoc *soc, unsigned module, unsigned cs,
                      const Tc4xQspiSlaveOps *ops, void *opaque)
{
    Tc4xQspi *q;
    DeviceState *dev;
    Tc4xQspiLegacy *l;

    if (module >= TC4X_QSPI_COUNT || cs >= TC4X_QSPI_CS_COUNT) {
        error_setg(&error_fatal, "tc4x-qspi: no chip select %u on QSPI%u",
                   cs, module);
        return;
    }
    q = tc4x_soc_qspi(soc, module);
    dev = ssi_get_cs(q->bus, cs);
    if (dev) {
        l = (Tc4xQspiLegacy *)object_dynamic_cast(OBJECT(dev),
                                                  TYPE_TC4X_QSPI_LEGACY);
        if (!l) {
            g_autofree char *path = object_get_canonical_path(OBJECT(dev));

            error_setg(&error_fatal, "tc4x-qspi: chip select %u of QSPI%u "
                       "is taken by %s", cs, module, path);
            return;
        }
        QEMU_LOCK_GUARD(&q->lock);
        l->ops = ops;
        l->opaque = opaque;
        return;
    }
    dev = qdev_new(TYPE_TC4X_QSPI_LEGACY);
    l = TC4X_QSPI_LEGACY(dev);
    l->ops = ops;
    l->opaque = opaque;
    qdev_prop_set_uint8(dev, "cs", cs);
    object_property_add_child(OBJECT(q), "legacy[*]", OBJECT(dev));
    tc4x_qspi_realize_peripheral(q, dev, &error_fatal);
}

void tc4x_qspi_set_rx_fed(Tc4xSoc *soc, unsigned module, bool fed)
{
    Tc4xQspi *q;

    if (module >= TC4X_QSPI_COUNT) {
        error_setg(&error_fatal, "tc4x-qspi: no QSPI%u", module);
        return;
    }
    q = tc4x_soc_qspi(soc, module);
    QEMU_LOCK_GUARD(&q->lock);
    q->rx_fed = fed;
}

void tc4x_qspi_lock(Tc4xSoc *soc)
{
    for (unsigned n = 0; n < TC4X_QSPI_COUNT; n++) {
        qemu_rec_mutex_lock(&tc4x_soc_qspi(soc, n)->lock);
    }
}

void tc4x_qspi_unlock(Tc4xSoc *soc)
{
    for (unsigned n = TC4X_QSPI_COUNT; n-- > 0;) {
        qemu_rec_mutex_unlock(&tc4x_soc_qspi(soc, n)->lock);
    }
}

static const TypeInfo tc4x_qspi_types[] = {
    {
        .name = TYPE_TC4X_QSPI_BUS,
        .parent = TYPE_SSI_BUS,
        .class_init = tc4x_qspi_bus_class_init,
    }, {
        .name = TYPE_TC4X_QSPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xQspi),
        .instance_init = tc4x_qspi_init,
        .instance_finalize = tc4x_qspi_finalize,
        .class_init = tc4x_qspi_class_init,
    }, {
        .name = TYPE_TC4X_QSPI_PERIPHERAL,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(Tc4xQspiPeripheral),
        .class_size = sizeof(Tc4xQspiPeripheralClass),
        .class_init = tc4x_qspi_peripheral_class_init,
        .abstract = true,
    }, {
        .name = TYPE_TC4X_QSPI_LEGACY,
        .parent = TYPE_TC4X_QSPI_PERIPHERAL,
        .instance_size = sizeof(Tc4xQspiLegacy),
        .class_init = tc4x_qspi_legacy_class_init,
    },
};

DEFINE_TYPES(tc4x_qspi_types)
