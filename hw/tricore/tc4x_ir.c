/*
 * TC4x interrupt router: service request nodes and their arbitration.
 *
 * Each SRC register routes its request to a service provider (TOS) with a
 * priority (SRPN); a CPU is offered the highest priority among the enabled
 * (SRE) pending (SRR) nodes routed to it, and acknowledging that priority
 * clears the node's SRR.  Register layout: Ifx_SRC_SRCR of the iLLD SFR
 * headers (tc4x_regs.h).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "cpu.h"
#include "hw/tricore/tc4x_ir.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "trace.h"

#define TC4X_IR_ACK_LISTENERS_MAX   8

/* SRC bits software writes store; SETR, CLRR and IOVCLR are triggers */
#define SRC_WRITABLE    (R_SRC_SRCR_SRPN_MASK | R_SRC_SRCR_VM_MASK |    \
                         R_SRC_SRCR_CS_MASK | R_SRC_SRCR_TOS_MASK |     \
                         R_SRC_SRCR_SRE_MASK)

typedef struct Tc4xIrAckListener {
    uint32_t src;
    Tc4xIrAckFn fn;
    void *opaque;
} Tc4xIrAckListener;

/*
 * Locking: cores acknowledge interrupts on their vCPU threads without the
 * BQL, and service requests come from any thread, so src (written),
 * pending, cpu_prio and acked are under lock; src is read atomically
 * without it.  Acknowledges of listened nodes reach their listeners in
 * the main loop (ack_bh), or with "deterministic" from a QEMU_CLOCK_VIRTUAL
 * timer due at once, which -icount runs at a fixed instruction count.
 * listeners and cpu are set up during machine init; watched and
 * watched_sync are under the BQL.
 */
struct Tc4xIr {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    bool imprecise_io;
    bool deterministic;
    TriCoreCPU *cpu[TC4X_IR_NUM_CPUS];

    QemuRecMutex lock;
    uint32_t src[TC4X_IR_NUM_SRC];
    GHashTable *pending;        /* indexes of nodes with SRR set */
    uint32_t cpu_prio[TC4X_IR_NUM_CPUS];
    GArray *acked;              /* indexes acknowledged since ack_bh */
    QEMUBH *ack_bh;
    QEMUTimer ack_timer;        /* deterministic: in place of ack_bh */
    Tc4xIrAckListener listeners[TC4X_IR_ACK_LISTENERS_MAX];
    unsigned n_listeners;

    qemu_irq *src_in;
    unsigned long watched[BITS_TO_LONGS(TC4X_IR_NUM_SRC)];
    unsigned long watched_sync[BITS_TO_LONGS(TC4X_IR_NUM_SRC)];
};

static unsigned src_index(uint32_t src)
{
    return (src - TC4X_IR_SRC_BASE) / 4;
}

static uint32_t src_addr(unsigned n)
{
    return TC4X_IR_SRC_BASE + n * 4;
}

/* Offer each CPU its highest pending priority; under the lock */
static void ir_update(Tc4xIr *ir)
{
    uint32_t best[TC4X_IR_NUM_CPUS] = { 0 };
    GHashTableIter it;
    gpointer key;

    g_hash_table_iter_init(&it, ir->pending);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        uint32_t v = ir->src[GPOINTER_TO_UINT(key)];
        uint32_t tos = FIELD_EX32(v, SRC_SRCR, TOS);
        uint32_t prio = FIELD_EX32(v, SRC_SRCR, SRPN);

        if ((v & R_SRC_SRCR_SRE_MASK) && tos < TC4X_IR_NUM_CPUS &&
            prio > best[tos]) {
            best[tos] = prio;
        }
    }
    for (int c = 0; c < TC4X_IR_NUM_CPUS; c++) {
        if (best[c] != ir->cpu_prio[c]) {
            ir->cpu_prio[c] = best[c];
            tricore_cpu_set_pending_irq(ir->cpu[c], best[c]);
        }
    }
}

/* Set or clear SRR of node @n; under the lock */
static void ir_set_srr(Tc4xIr *ir, unsigned n, bool set)
{
    uint32_t v = ir->src[n];

    if (set) {
        if (v & R_SRC_SRCR_SRR_MASK) {
            v |= R_SRC_SRCR_IOV_MASK;
        }
        v |= R_SRC_SRCR_SRR_MASK;
        g_hash_table_add(ir->pending, GUINT_TO_POINTER(n));
    } else {
        v &= ~R_SRC_SRCR_SRR_MASK;
        g_hash_table_remove(ir->pending, GUINT_TO_POINTER(n));
    }
    qatomic_set(&ir->src[n], v);
    ir_update(ir);
}

/* "src" gpio-in: a hardware service request */
static void ir_src_request(void *opaque, int n, int level)
{
    Tc4xIr *ir = opaque;

    if (level) {
        QEMU_LOCK_GUARD(&ir->lock);
        ir_set_srr(ir, n, true);
    }
}

/* "nmi" gpio-in */
static void ir_nmi(void *opaque, int n, int level)
{
    Tc4xIr *ir = opaque;

    if (level) {
        tricore_cpu_raise_nmi(ir->cpu[n]);
    }
}

qemu_irq tc4x_ir_src(Tc4xIr *ir, uint32_t src)
{
    assert(tc4x_ir_is_src(src));
    return ir->src_in[src_index(src)];
}

uint32_t tc4x_ir_src_get(Tc4xIr *ir, uint32_t src)
{
    assert(tc4x_ir_is_src(src));
    return qatomic_read(&ir->src[src_index(src)]);
}

void tc4x_ir_src_set(Tc4xIr *ir, uint32_t src, uint32_t val)
{
    unsigned n;

    assert(tc4x_ir_is_src(src));
    n = src_index(src);
    QEMU_LOCK_GUARD(&ir->lock);
    if (val & R_SRC_SRCR_SRR_MASK) {
        g_hash_table_add(ir->pending, GUINT_TO_POINTER(n));
    } else {
        g_hash_table_remove(ir->pending, GUINT_TO_POINTER(n));
    }
    qatomic_set(&ir->src[n], val);
    ir_update(ir);
}

static bool ir_listened(Tc4xIr *ir, uint32_t src)
{
    for (unsigned i = 0; i < ir->n_listeners; i++) {
        if (ir->listeners[i].src == src) {
            return true;
        }
    }
    return false;
}

static void ir_ack_bh(void *opaque)
{
    Tc4xIr *ir = opaque;
    g_autoptr(GArray) acked = g_array_new(false, false, sizeof(uint32_t));

    WITH_QEMU_LOCK_GUARD(&ir->lock) {
        g_array_append_vals(acked, ir->acked->data, ir->acked->len);
        g_array_set_size(ir->acked, 0);
    }
    for (guint i = 0; i < acked->len; i++) {
        uint32_t src = g_array_index(acked, uint32_t, i);

        for (unsigned k = 0; k < ir->n_listeners; k++) {
            if (ir->listeners[k].src == src) {
                ir->listeners[k].fn(ir->listeners[k].opaque, src);
            }
        }
    }
}

void tc4x_ir_add_ack_listener(Tc4xIr *ir, uint32_t src, Tc4xIrAckFn fn,
                              void *opaque)
{
    assert(tc4x_ir_is_src(src));
    QEMU_LOCK_GUARD(&ir->lock);
    assert(ir->n_listeners < TC4X_IR_ACK_LISTENERS_MAX);
    ir->listeners[ir->n_listeners++] = (Tc4xIrAckListener) {
        .src = src, .fn = fn, .opaque = opaque,
    };
}

/* A core takes the interrupt of priority @prio, without the BQL */
static void ir_ack(void *opaque, uint32_t core, uint32_t prio)
{
    Tc4xIr *ir = opaque;
    GHashTableIter it;
    gpointer key;

    QEMU_LOCK_GUARD(&ir->lock);
    g_hash_table_iter_init(&it, ir->pending);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        unsigned n = GPOINTER_TO_UINT(key);
        uint32_t v = ir->src[n];

        if ((v & R_SRC_SRCR_SRE_MASK) &&
            FIELD_EX32(v, SRC_SRCR, SRPN) == prio &&
            FIELD_EX32(v, SRC_SRCR, TOS) == core) {
            trace_tc4x_ir_ack(core, prio, src_addr(n));
            ir_set_srr(ir, n, false);
            if (ir_listened(ir, src_addr(n))) {
                uint32_t src = src_addr(n);

                g_array_append_val(ir->acked, src);
                if (ir->deterministic) {
                    timer_mod_ns(&ir->ack_timer, 0);
                } else {
                    qemu_bh_schedule(ir->ack_bh);
                }
            }
            return;
        }
    }
}

/* SRC writes are under the BQL already */
static bool tc4x_ir_sim_watch(Tc4xSimCommands *obj, uint32_t addr,
                              Tc4xSimWatch mode, Error **errp)
{
    Tc4xIr *ir = TC4X_IR(obj);
    unsigned n = src_index(addr);

    assert(tc4x_ir_is_src(addr));
    if (mode == TC4X_SIM_WATCH_OFF) {
        clear_bit(n, ir->watched);
    } else {
        set_bit(n, ir->watched);
    }
    if (mode == TC4X_SIM_WATCH_SYNC) {
        set_bit(n, ir->watched_sync);
    } else {
        clear_bit(n, ir->watched_sync);
    }
    return true;
}

static MemTxResult ir_read(void *opaque, hwaddr off, uint64_t *data,
                           unsigned size, MemTxAttrs attrs)
{
    Tc4xIr *ir = opaque;

    if (tc4x_master_refused(attrs, "tc4x-ir", src_addr(off / 4) + (off & 3),
                            false)) {
        return MEMTX_ERROR;
    }
    *data = extract32(qatomic_read(&ir->src[off / 4]), (off & 3) * 8,
                      size * 8);
    return MEMTX_OK;
}

/* Software writes: the stored bits, then the SETR/CLRR/IOVCLR triggers */
static MemTxResult ir_write(void *opaque, hwaddr off, uint64_t val64,
                            unsigned size, MemTxAttrs attrs)
{
    Tc4xIr *ir = opaque;
    unsigned n = off / 4;
    uint32_t val, v;

    if (tc4x_master_refused(attrs, "tc4x-ir", src_addr(n) + (off & 3),
                            true)) {
        return MEMTX_ERROR;
    }
    BQL_LOCK_GUARD();
    val = deposit32(qatomic_read(&ir->src[n]), (off & 3) * 8, size * 8,
                    val64);
    WITH_QEMU_LOCK_GUARD(&ir->lock) {
        v = (ir->src[n] & ~SRC_WRITABLE) | (val & SRC_WRITABLE);
        if (val & R_SRC_SRCR_IOVCLR_MASK) {
            v &= ~R_SRC_SRCR_IOV_MASK;
        }
        qatomic_set(&ir->src[n], v);
        if (val & R_SRC_SRCR_SETR_MASK) {
            ir_set_srr(ir, n, true);
        } else if (val & R_SRC_SRCR_CLRR_MASK) {
            ir_set_srr(ir, n, false);
        } else {
            ir_update(ir);
        }
    }
    if (test_bit(n, ir->watched)) {
        tc4x_sim_watch_event(src_addr(n), val, size,
                             test_bit(n, ir->watched_sync) ?
                             TC4X_SIM_WATCH_SYNC : TC4X_SIM_WATCH_ON);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ir_ops = {
    .read_with_attrs = ir_read,
    .write_with_attrs = ir_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void tc4x_ir_reset_hold(Object *obj, ResetType type)
{
    Tc4xIr *ir = TC4X_IR(obj);

    QEMU_LOCK_GUARD(&ir->lock);
    memset(ir->src, 0, sizeof(ir->src));
    g_hash_table_remove_all(ir->pending);
    g_array_set_size(ir->acked, 0);
    ir_update(ir);
}

static void tc4x_ir_init(Object *obj)
{
    Tc4xIr *ir = TC4X_IR(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&ir->mmio, obj, &ir_ops, ir, "ir.src",
                          TC4X_IR_SRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &ir->mmio);
    qdev_init_gpio_in_named(dev, ir_src_request, "src", TC4X_IR_NUM_SRC);
    qdev_init_gpio_in_named(dev, ir_nmi, "nmi", TC4X_IR_NUM_CPUS);
    qemu_rec_mutex_init(&ir->lock);
    ir->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
    ir->acked = g_array_new(false, false, sizeof(uint32_t));
}

static void tc4x_ir_realize(DeviceState *dev, Error **errp)
{
    Tc4xIr *ir = TC4X_IR(dev);

    for (int c = 0; c < TC4X_IR_NUM_CPUS; c++) {
        if (!ir->cpu[c]) {
            error_setg(errp, "tc4x-ir: link cpu%d is not set", c);
            return;
        }
    }
    ir->src_in = g_new(qemu_irq, TC4X_IR_NUM_SRC);
    for (int n = 0; n < TC4X_IR_NUM_SRC; n++) {
        ir->src_in[n] = qdev_get_gpio_in_named(dev, "src", n);
    }
    for (int c = 0; c < TC4X_IR_NUM_CPUS; c++) {
        ir->cpu[c]->irq_ack = ir_ack;
        ir->cpu[c]->irq_ack_opaque = ir;
    }
    ir->ack_bh = qemu_bh_new(ir_ack_bh, ir);
    timer_init_ns(&ir->ack_timer, QEMU_CLOCK_VIRTUAL, ir_ack_bh, ir);
    /* SRC reads have no side effects; writes take the BQL */
    memory_region_enable_lockless_io(&ir->mmio);
    memory_region_set_imprecise_io(&ir->mmio, ir->imprecise_io);
}

static const Property tc4x_ir_properties[] = {
    DEFINE_PROP_BOOL("imprecise-io", Tc4xIr, imprecise_io, false),
    DEFINE_PROP_BOOL("deterministic", Tc4xIr, deterministic, false),
    DEFINE_PROP_LINK("cpu0", Tc4xIr, cpu[0], TYPE_TRICORE_CPU, TriCoreCPU *),
    DEFINE_PROP_LINK("cpu1", Tc4xIr, cpu[1], TYPE_TRICORE_CPU, TriCoreCPU *),
    DEFINE_PROP_LINK("cpu2", Tc4xIr, cpu[2], TYPE_TRICORE_CPU, TriCoreCPU *),
    DEFINE_PROP_LINK("cpu3", Tc4xIr, cpu[3], TYPE_TRICORE_CPU, TriCoreCPU *),
    DEFINE_PROP_LINK("cpu4", Tc4xIr, cpu[4], TYPE_TRICORE_CPU, TriCoreCPU *),
    DEFINE_PROP_LINK("cpu5", Tc4xIr, cpu[5], TYPE_TRICORE_CPU, TriCoreCPU *),
    DEFINE_PROP_LINK("cpu6", Tc4xIr, cpu[6], TYPE_TRICORE_CPU, TriCoreCPU *),
};

static const VMStateDescription vmstate_tc4x_ir = {
    .name = TYPE_TC4X_IR,
    .unmigratable = 1,
};

/* irq SRC: a hardware service request of the node at SRC */
static bool tc4x_ir_sim_run(Tc4xSimCommands *obj, int argc, char **argv,
                            GString *out)
{
    Tc4xIr *ir = TC4X_IR(obj);
    uint64_t src;

    if (argc != 2 || !tc4x_sim_parse_uint(argv[1], UINT32_MAX, &src)) {
        g_string_append(out, "err usage: irq SRC\n");
    } else if (!tc4x_ir_is_src(src)) {
        g_string_append_printf(out, "err 0x%08" PRIx64 " is not a service "
                               "request node\n", src);
    } else {
        qemu_irq_pulse(tc4x_ir_src(ir, src & ~3u));
        g_string_append(out, "ok\n");
    }
    return true;
}

static const char *const tc4x_ir_sim_commands[] = { "irq", NULL };

static void tc4x_ir_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    Tc4xSimCommandsClass *sc = TC4X_SIM_COMMANDS_CLASS(oc);

    dc->desc = "TC4x interrupt router";
    dc->realize = tc4x_ir_realize;
    dc->vmsd = &vmstate_tc4x_ir;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_ir_reset_hold;
    sc->commands = tc4x_ir_sim_commands;
    sc->run = tc4x_ir_sim_run;
    sc->watch = tc4x_ir_sim_watch;
    device_class_set_props(dc, tc4x_ir_properties);
}

static const TypeInfo tc4x_ir_types[] = {
    {
        .name = TYPE_TC4X_IR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xIr),
        .instance_init = tc4x_ir_init,
        .class_init = tc4x_ir_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_SIM_COMMANDS },
            { }
        },
    },
};

DEFINE_TYPES(tc4x_ir_types)
