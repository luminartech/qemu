/*
 * TC4x SFR store: the plain registers of the SoC's SFR space.
 *
 * Registers are 4 KiB pages of 32-bit words in a two-level table, allocated
 * on first use.  Values and per-word flags are atomic, so that accesses
 * without side effects are served on the vCPU thread without the BQL (the
 * regions are lockless_io); everything else takes the BQL.
 *
 * After machine init the store keeps a copy of every value (the cold
 * image: reset values and what board code set up), which each system
 * reset restores.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "trace.h"

#define SFR_PAGE_BITS       12
#define SFR_PAGE_WORDS      (1u << (SFR_PAGE_BITS - 2))
#define SFR_L2_BITS         10
#define SFR_L2_SIZE         (1u << SFR_L2_BITS)
#define SFR_L1_SIZE         (1u << (32 - SFR_PAGE_BITS - SFR_L2_BITS))

/* Bits pinned for reads whatever the register holds */
typedef struct Tc4xSfrForces {
    uint32_t value[SFR_PAGE_WORDS];
    uint32_t mask[SFR_PAGE_WORDS];
} Tc4xSfrForces;

typedef struct Tc4xSfrPage {
    uint32_t val[SFR_PAGE_WORDS];
    uint32_t reads[SFR_PAGE_WORDS];     /* since the last hot query */
    /* written, or its first read logged as unimplemented */
    unsigned long known[BITS_TO_LONGS(SFR_PAGE_WORDS)];
    /* full-word writes may skip the BQL once found plain */
    unsigned long lockless_write[BITS_TO_LONGS(SFR_PAGE_WORDS)];
    /* found plain: such a write changes the bits of plain_persist */
    unsigned long plain_write[BITS_TO_LONGS(SFR_PAGE_WORDS)];
    uint32_t plain_persist[SFR_PAGE_WORDS];
    /* accesses take the BQL (hooked or watched) */
    unsigned long bql_only[BITS_TO_LONGS(SFR_PAGE_WORDS)];
    /* reads call a hook, under the BQL */
    unsigned long read_hooked[BITS_TO_LONGS(SFR_PAGE_WORDS)];
    Tc4xSfrForces *forces;
    uint32_t *cold;                     /* the cold image of val, or NULL */
} Tc4xSfrPage;

typedef struct Tc4xSfrWriteHook {
    Tc4xSfrWriteFn fn;
    void *opaque;
} Tc4xSfrWriteHook;

typedef struct Tc4xSfrReadHook {
    Tc4xSfrReadFn fn;
    void *opaque;
} Tc4xSfrReadHook;

typedef struct Tc4xSfrPassthrough {
    MemoryRegion *mr;
    Tc4xSfrServesFn serves;
    void *opaque;
} Tc4xSfrPassthrough;

/*
 * Locking: sfr_l1 and the pages are published atomically and read on any
 * thread; values and flags in a page are atomic.  The tables (semantics,
 * kernel resets, hooks) are filled during machine init and read on any
 * thread afterwards.  watches, models and the cold image: BQL.
 */
struct Tc4xSfrStore {
    SysBusDevice parent_obj;

    MemoryRegion seg[2];
    bool imprecise_io;

    Tc4xSfrPage **sfr_l1[SFR_L1_SIZE];
    GHashTable *semantics;      /* addr -> const Tc4xRegSemantics */
    GHashTable *kresets;        /* RST_CTRLB addr -> const Tc4xKernelReset */
    GHashTable *write_hooks;    /* addr -> Tc4xSfrWriteHook */
    GHashTable *read_hooks;     /* addr -> Tc4xSfrReadHook */
    GHashTable *watches;        /* addr -> Tc4xSimWatch */
    GArray *passthrough;        /* Tc4xSfrPassthrough */

    const Tc4xSfrModelOps *models;
    void *models_opaque;
    /* the attributes of the access whose hooks and models run (BQL) */
    MemTxAttrs hook_attrs;

    Notifier machine_done;
};

static void *sfr_publish(void **slot, size_t size)
{
    void *fresh = g_malloc0(size);
    void *old = qatomic_cmpxchg(slot, NULL, fresh);

    if (old) {
        g_free(fresh);
        return old;
    }
    return fresh;
}

static Tc4xSfrPage *sfr_page(Tc4xSfrStore *st, uint32_t addr, bool alloc)
{
    Tc4xSfrPage ***l1 = &st->sfr_l1[addr >> (SFR_PAGE_BITS + SFR_L2_BITS)];
    Tc4xSfrPage **l2 = qatomic_load_acquire(l1);
    Tc4xSfrPage **slot;
    Tc4xSfrPage *pg;

    if (!l2) {
        if (!alloc) {
            return NULL;
        }
        l2 = sfr_publish((void **)l1, SFR_L2_SIZE * sizeof(*l2));
    }
    slot = &l2[(addr >> SFR_PAGE_BITS) & (SFR_L2_SIZE - 1)];
    pg = qatomic_load_acquire(slot);
    if (!pg && alloc) {
        pg = sfr_publish((void **)slot, sizeof(*pg));
    }
    return pg;
}

static unsigned sfr_word(uint32_t addr)
{
    return (addr >> 2) & (SFR_PAGE_WORDS - 1);
}

typedef void (*SfrPageFn)(Tc4xSfrPage *pg, uint32_t base, void *opaque);

/* @fn for every page, with the address of its first word */
static void sfr_foreach_page(Tc4xSfrStore *st, SfrPageFn fn, void *opaque)
{
    for (uint32_t i1 = 0; i1 < SFR_L1_SIZE; i1++) {
        Tc4xSfrPage **l2 = qatomic_load_acquire(&st->sfr_l1[i1]);

        for (uint32_t i2 = 0; l2 && i2 < SFR_L2_SIZE; i2++) {
            Tc4xSfrPage *pg = qatomic_load_acquire(&l2[i2]);

            if (pg) {
                fn(pg, (i1 << (SFR_PAGE_BITS + SFR_L2_BITS)) |
                       (i2 << SFR_PAGE_BITS), opaque);
            }
        }
    }
}

uint32_t tc4x_sfr_store_get(Tc4xSfrStore *st, uint32_t addr)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, false);

    return pg ? qatomic_read(&pg->val[sfr_word(addr)]) : 0;
}

void tc4x_sfr_store_set(Tc4xSfrStore *st, uint32_t addr, uint32_t val)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, true);

    qatomic_set(&pg->val[sfr_word(addr)], val);
    set_bit_atomic(sfr_word(addr), pg->known);
}

void tc4x_sfr_store_or(Tc4xSfrStore *st, uint32_t addr, uint32_t bits)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, true);

    qatomic_or(&pg->val[sfr_word(addr)], bits);
    set_bit_atomic(sfr_word(addr), pg->known);
}

uint32_t tc4x_sfr_store_cmpxchg(Tc4xSfrStore *st, uint32_t addr, uint32_t old,
                                uint32_t val)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, true);
    uint32_t found = qatomic_cmpxchg(&pg->val[sfr_word(addr)], old, val);

    if (found == old) {
        set_bit_atomic(sfr_word(addr), pg->known);
    }
    return found;
}

void tc4x_sfr_store_force(Tc4xSfrStore *st, uint32_t addr, uint32_t value,
                          uint32_t mask)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, true);
    Tc4xSfrForces *f = sfr_publish((void **)&pg->forces, sizeof(*f));

    qatomic_set(&f->value[sfr_word(addr)], value);
    qatomic_set(&f->mask[sfr_word(addr)], mask);
}

static uint32_t sfr_apply_force(Tc4xSfrPage *pg, unsigned w, uint32_t v)
{
    Tc4xSfrForces *f = pg ? qatomic_load_acquire(&pg->forces) : NULL;
    uint32_t mask = f ? qatomic_read(&f->mask[w]) : 0;

    return mask ? (v & ~mask) | (qatomic_read(&f->value[w]) & mask) : v;
}

void tc4x_sfr_store_add_semantics(Tc4xSfrStore *st, const Tc4xRegSemantics *sem,
                                  size_t n_sem, const Tc4xKernelReset *kr,
                                  size_t n_kr)
{
    for (size_t i = 0; i < n_sem; i++) {
        g_hash_table_insert(st->semantics, GUINT_TO_POINTER(sem[i].addr),
                            (gpointer)&sem[i]);
    }
    for (size_t i = 0; i < n_kr; i++) {
        g_hash_table_insert(st->kresets, GUINT_TO_POINTER(kr[i].ctrlb),
                            (gpointer)&kr[i]);
    }
}

/*
 * Store @val through the register's write semantics.  Returns whether the
 * register is plain storage, with the bits a write changes in @persist.
 */
static bool sfr_store(Tc4xSfrStore *st, uint32_t addr, uint32_t val,
                      uint32_t *persist)
{
    const Tc4xRegSemantics *m =
        g_hash_table_lookup(st->semantics, GUINT_TO_POINTER(addr));
    uint32_t v;

    *persist = UINT32_MAX;
    if (!m) {
        tc4x_sfr_store_set(st, addr, val);
        return true;
    }
    v = (tc4x_sfr_store_get(st, addr) & ~m->persist) | (val & m->persist);
    v &= ~(val & m->w1c);
    v &= ~(~val & m->w0c);
    v |= val & m->w1s;
    tc4x_sfr_store_set(st, addr, v);
    *persist = m->persist;
    return !m->w1c && !m->w0c && !m->w1s;
}

/*
 * A write to a kernel reset CTRLB completes at once; false for others.
 * The iLLD resetModule() functions wait for RST_STAT.KRST to set.
 */
#define RST_KRST            BIT(0)
#define RST_CTRLB_STATCLR   BIT(31)

static bool sfr_kernel_reset(Tc4xSfrStore *st, uint32_t addr, uint32_t val)
{
    const Tc4xKernelReset *k =
        g_hash_table_lookup(st->kresets, GUINT_TO_POINTER(addr));

    if (!k) {
        return false;
    }
    if (val & RST_KRST) {
        tc4x_sfr_store_or(st, k->stat, RST_KRST);
        tc4x_sfr_store_set(st, k->ctrla,
                           tc4x_sfr_store_get(st, k->ctrla) & ~RST_KRST);
        tc4x_sfr_store_set(st, addr, tc4x_sfr_store_get(st, addr) & ~RST_KRST);
    }
    if (val & RST_CTRLB_STATCLR) {
        tc4x_sfr_store_set(st, k->stat,
                           tc4x_sfr_store_get(st, k->stat) & ~RST_KRST);
        tc4x_sfr_store_set(st, addr,
                           tc4x_sfr_store_get(st, addr) & ~RST_CTRLB_STATCLR);
    }
    return true;
}

void tc4x_sfr_store_write(Tc4xSfrStore *st, uint32_t addr, uint32_t val)
{
    uint32_t persist;
    bool plain = sfr_store(st, addr, val, &persist);
    Tc4xSfrPage *pg;
    unsigned w;

    if (sfr_kernel_reset(st, addr, val) || !plain) {
        return;
    }
    pg = sfr_page(st, addr, true);
    w = sfr_word(addr);
    if (test_bit(w, pg->lockless_write) && !test_bit(w, pg->bql_only)) {
        qatomic_set(&pg->plain_persist[w], persist);
        set_bit_atomic(w, pg->plain_write);
    }
}

void tc4x_sfr_store_allow_lockless_write(Tc4xSfrStore *st, uint32_t addr)
{
    set_bit_atomic(sfr_word(addr), sfr_page(st, addr, true)->lockless_write);
}

MemTxAttrs tc4x_sfr_store_hook_attrs(Tc4xSfrStore *st)
{
    /* hooks run under the BQL: a thread without it runs none */
    return bql_locked() ? st->hook_attrs : MEMTXATTRS_UNSPECIFIED;
}

bool tc4x_sfr_store_bql_only(Tc4xSfrStore *st, uint32_t addr)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, false);

    return pg && test_bit(sfr_word(addr), pg->bql_only);
}

static void sfr_set_bql_only(Tc4xSfrStore *st, uint32_t addr)
{
    Tc4xSfrPage *pg = sfr_page(st, addr, true);

    set_bit_atomic(sfr_word(addr), pg->bql_only);
    clear_bit_atomic(sfr_word(addr), pg->plain_write);
}

/* Whether @sec is a passthrough region at an offset it leaves to the store */
static bool sfr_passed_through(Tc4xSfrStore *st, MemoryRegionSection *sec)
{
    for (guint i = 0; i < st->passthrough->len; i++) {
        Tc4xSfrPassthrough *pt = &g_array_index(st->passthrough,
                                                Tc4xSfrPassthrough, i);

        if (pt->mr == sec->mr) {
            return !pt->serves(pt->opaque, sec->offset_within_region);
        }
    }
    return false;
}

Object *tc4x_sfr_store_owner(Tc4xSfrStore *st, uint32_t addr)
{
    MemoryRegionSection sec = memory_region_find(get_system_memory(), addr, 4);
    Object *owner = sec.mr ? memory_region_owner(sec.mr) : NULL;
    bool store = owner == OBJECT(st) ||
                 (sec.mr && sfr_passed_through(st, &sec));

    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    if (store) {
        return NULL;
    }
    return owner ? owner : OBJECT(get_system_memory());
}

void tc4x_sfr_store_add_passthrough(Tc4xSfrStore *st, MemoryRegion *mr,
                                    Tc4xSfrServesFn serves, void *opaque)
{
    Tc4xSfrPassthrough pt = { .mr = mr, .serves = serves, .opaque = opaque };

    g_array_append_val(st->passthrough, pt);
}

/* Guest writes of a watched register take the BQL and report themselves */
static bool tc4x_sfr_store_sim_watch(Tc4xSimCommands *obj, uint32_t addr,
                                     Tc4xSimWatch mode, Error **errp)
{
    Tc4xSfrStore *st = TC4X_SFR_STORE(obj);

    if (mode != TC4X_SIM_WATCH_OFF) {
        g_hash_table_insert(st->watches, GUINT_TO_POINTER(addr),
                            GUINT_TO_POINTER(mode));
        sfr_set_bql_only(st, addr);
    } else {
        Tc4xSfrPage *pg = sfr_page(st, addr, false);

        g_hash_table_remove(st->watches, GUINT_TO_POINTER(addr));
        if (pg && !g_hash_table_contains(st->write_hooks,
                                         GUINT_TO_POINTER(addr))) {
            clear_bit_atomic(sfr_word(addr), pg->bql_only);
        }
    }
    return true;
}

typedef struct SfrHotQuery {
    uint32_t min;
    GString *out;
} SfrHotQuery;

static void sfr_page_hot(Tc4xSfrPage *pg, uint32_t base, void *opaque)
{
    SfrHotQuery *q = opaque;

    for (uint32_t w = 0; w < SFR_PAGE_WORDS; w++) {
        uint32_t n = qatomic_xchg(&pg->reads[w], 0);

        if (n >= q->min) {
            g_string_append_printf(q->out, "hot 0x%08x %u 0x%08x\n",
                                   base + w * 4, n, qatomic_read(&pg->val[w]));
        }
    }
}

void tc4x_sfr_store_hot(Tc4xSfrStore *st, uint32_t min, GString *out)
{
    SfrHotQuery q = { .min = min, .out = out };

    sfr_foreach_page(st, sfr_page_hot, &q);
}

static void sfr_check_store_backed(Tc4xSfrStore *st, uint32_t addr)
{
    Object *owner;

    /* vCPU threads look hooks up without a lock */
    assert(!phase_check(PHASE_MACHINE_READY));
    owner = tc4x_sfr_store_owner(st, addr);
    if (owner) {
        g_autofree char *path = object_get_canonical_path(owner);

        error_setg(&error_fatal, "tc4x-sfr-store: hook at 0x%08x, owned by %s",
                   addr, path);
    }
}

void tc4x_sfr_store_add_write_hook(Tc4xSfrStore *st, uint32_t addr,
                                   Tc4xSfrWriteFn fn, void *opaque)
{
    Tc4xSfrWriteHook *h = g_new(Tc4xSfrWriteHook, 1);

    sfr_check_store_backed(st, addr);
    h->fn = fn;
    h->opaque = opaque;
    g_hash_table_insert(st->write_hooks, GUINT_TO_POINTER(addr), h);
    sfr_set_bql_only(st, addr);
}

void tc4x_sfr_store_add_read_hook(Tc4xSfrStore *st, uint32_t addr,
                                  Tc4xSfrReadFn fn, void *opaque)
{
    Tc4xSfrReadHook *h = g_new(Tc4xSfrReadHook, 1);

    sfr_check_store_backed(st, addr);
    h->fn = fn;
    h->opaque = opaque;
    g_hash_table_insert(st->read_hooks, GUINT_TO_POINTER(addr), h);
    set_bit_atomic(sfr_word(addr), sfr_page(st, addr, true)->read_hooked);
}

void tc4x_sfr_store_set_models(Tc4xSfrStore *st, const Tc4xSfrModelOps *ops,
                               void *opaque)
{
    st->models = ops;
    st->models_opaque = opaque;
}

/* A word read under the BQL, before forces */
static uint32_t sfr_read_locked(Tc4xSfrStore *st, uint32_t addr)
{
    uint32_t v = tc4x_sfr_store_get(st, addr);
    Tc4xSfrReadHook *h = g_hash_table_lookup(st->read_hooks,
                                             GUINT_TO_POINTER(addr));
    Tc4xSfrPage *pg;
    unsigned long bit;

    if ((h && h->fn(h->opaque, addr, &v)) ||
        st->models->read(st->models_opaque, addr, &v)) {
        return v;
    }
    pg = sfr_page(st, addr, true);
    bit = BIT_MASK(sfr_word(addr));
    if (!(qatomic_fetch_or(&pg->known[BIT_WORD(sfr_word(addr))], bit) & bit)) {
        qemu_log_mask(LOG_UNIMP, "tc4x-sfr: first read of 0x%08x\n", addr);
    }
    return v;
}

uint64_t tc4x_sfr_store_access_read(Tc4xSfrStore *st, uint32_t addr,
                                    unsigned size, MemTxAttrs attrs)
{
    uint32_t word = addr & ~3u;
    Tc4xSfrPage *pg = sfr_page(st, word, false);
    unsigned w = sfr_word(word);
    uint32_t v = pg ? qatomic_read(&pg->val[w]) : 0;
    bool served = false;

    if (!pg || !test_bit(w, pg->read_hooked)) {
        switch (st->models->read_lockless(st->models_opaque, addr, size, &v)) {
        case TC4X_SFR_PATH_STORE:
            served = pg && test_bit(w, pg->known);
            break;
        case TC4X_SFR_PATH_SERVED:
            served = true;
            break;
        case TC4X_SFR_PATH_LOCKED:
            break;
        }
    }
    if (!served) {
        BQL_LOCK_GUARD();
        MemTxAttrs outer = st->hook_attrs;

        st->hook_attrs = attrs;
        v = sfr_read_locked(st, word);
        st->hook_attrs = outer;
        pg = sfr_page(st, word, false);
    }
    if (pg) {
        qatomic_inc(&pg->reads[w]);
    }
    return extract32(sfr_apply_force(pg, w, v), (addr & 3) * 8, size * 8);
}

/* A plain register's full-word write, without the BQL */
static bool sfr_write_plain(Tc4xSfrPage *pg, unsigned w, uint32_t val)
{
    uint32_t persist, old, prev;

    if (!(qatomic_load_acquire(&pg->plain_write[BIT_WORD(w)]) &
          BIT_MASK(w))) {
        return false;
    }
    persist = qatomic_read(&pg->plain_persist[w]);
    old = qatomic_read(&pg->val[w]);
    do {
        prev = old;
        old = qatomic_cmpxchg(&pg->val[w], prev,
                              (prev & ~persist) | (val & persist));
    } while (old != prev);
    return true;
}

static void sfr_write_locked(Tc4xSfrStore *st, uint32_t addr, uint32_t val)
{
    Tc4xSfrWriteHook *h = g_hash_table_lookup(st->write_hooks,
                                              GUINT_TO_POINTER(addr));

    if ((h && h->fn(h->opaque, addr, val)) ||
        st->models->write(st->models_opaque, addr, &val)) {
        return;
    }
    tc4x_sfr_store_write(st, addr, val);
}

void tc4x_sfr_store_access_write(Tc4xSfrStore *st, uint32_t addr, uint64_t val,
                                 unsigned size, MemTxAttrs attrs)
{
    uint32_t word_addr = addr & ~3u;
    uint32_t shift = (addr & 3) * 8;
    uint32_t word = val;
    Tc4xSimWatch watch;
    MemTxAttrs outer;

    if (size == 4 && !(addr & 3)) {
        Tc4xSfrPage *pg = sfr_page(st, addr, false);
        unsigned w = sfr_word(addr);

        if (!(pg && test_bit(w, pg->bql_only)) &&
            ((pg && sfr_write_plain(pg, w, val)) ||
             (st->models->write_lockless &&
              st->models->write_lockless(st->models_opaque, addr, val)))) {
            return;
        }
    }

    BQL_LOCK_GUARD();
    if (size < 4) {
        const Tc4xRegSemantics *m =
            g_hash_table_lookup(st->semantics, GUINT_TO_POINTER(word_addr));
        uint32_t others = ~MAKE_64BIT_MASK(shift, size * 8);

        word = deposit32(tc4x_sfr_store_get(st, word_addr), shift, size * 8,
                         val);
        if (m) {
            /* the bytes not written clear and set nothing */
            word &= ~(others & (m->w1c | m->w1s));
            word |= others & m->w0c;
        }
    }
    watch = GPOINTER_TO_UINT(g_hash_table_lookup(st->watches,
                                                 GUINT_TO_POINTER(word_addr)));
    outer = st->hook_attrs;
    st->hook_attrs = attrs;
    sfr_write_locked(st, word_addr, word);
    st->hook_attrs = outer;
    if (watch != TC4X_SIM_WATCH_OFF) {
        tc4x_sim_watch_event(word_addr, word, size, watch);
    }
}

static MemTxResult sfr_read(Tc4xSfrStore *st, uint32_t addr, uint64_t *data,
                            unsigned size, MemTxAttrs attrs)
{
    if (tc4x_master_refused(attrs, "tc4x-sfr", addr, false)) {
        return MEMTX_ERROR;
    }
    *data = tc4x_sfr_store_access_read(st, addr, size, attrs);
    return MEMTX_OK;
}

static MemTxResult sfr_write(Tc4xSfrStore *st, uint32_t addr, uint64_t val,
                             unsigned size, MemTxAttrs attrs)
{
    if (tc4x_master_refused(attrs, "tc4x-sfr", addr, true)) {
        return MEMTX_ERROR;
    }
    tc4x_sfr_store_access_write(st, addr, val, size, attrs);
    return MEMTX_OK;
}

static MemTxResult sfr_e_read(void *opaque, hwaddr off, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    return sfr_read(opaque, TC4X_SFR_SEGMENT_E + off, data, size, attrs);
}

static MemTxResult sfr_e_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size, MemTxAttrs attrs)
{
    return sfr_write(opaque, TC4X_SFR_SEGMENT_E + off, val, size, attrs);
}

static MemTxResult sfr_f_read(void *opaque, hwaddr off, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    return sfr_read(opaque, TC4X_SFR_SEGMENT_F + off, data, size, attrs);
}

static MemTxResult sfr_f_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size, MemTxAttrs attrs)
{
    return sfr_write(opaque, TC4X_SFR_SEGMENT_F + off, val, size, attrs);
}

static const MemoryRegionOps sfr_seg_ops[2] = {
    {
        .read_with_attrs = sfr_e_read,
        .write_with_attrs = sfr_e_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8 },
        .impl = { .min_access_size = 1, .max_access_size = 4 },
    }, {
        .read_with_attrs = sfr_f_read,
        .write_with_attrs = sfr_f_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8 },
        .impl = { .min_access_size = 1, .max_access_size = 4 },
    },
};

static void sfr_page_snapshot(Tc4xSfrPage *pg, uint32_t base, void *opaque)
{
    unsigned *pages = opaque;

    pg->cold = g_memdup2(pg->val, sizeof(pg->val));
    (*pages)++;
}

/* The cold image: every value once machine init is done */
static void tc4x_sfr_store_machine_done(Notifier *n, void *data)
{
    Tc4xSfrStore *st = container_of(n, Tc4xSfrStore, machine_done);
    unsigned pages = 0;

    sfr_foreach_page(st, sfr_page_snapshot, &pages);
    trace_tc4x_sfr_store_cold_image(pages);
}

static void sfr_page_restore(Tc4xSfrPage *pg, uint32_t base, void *opaque)
{
    if (pg->cold) {
        memcpy(pg->val, pg->cold, sizeof(pg->val));
    } else {
        memset(pg->val, 0, sizeof(pg->val));
    }
    /* the next write under the BQL finds plain registers again */
    bitmap_zero(pg->plain_write, SFR_PAGE_WORDS);
}

/* vCPUs are stopped: the cold image back in place */
static void tc4x_sfr_store_reset_hold(Object *obj, ResetType type)
{
    sfr_foreach_page(TC4X_SFR_STORE(obj), sfr_page_restore, NULL);
    trace_tc4x_sfr_store_reset();
}

static Tc4xSfrPath sfr_no_read_lockless(void *opaque, uint32_t addr,
                                        unsigned size, uint32_t *val)
{
    return TC4X_SFR_PATH_STORE;
}

static bool sfr_no_read(void *opaque, uint32_t addr, uint32_t *val)
{
    return false;
}

static bool sfr_no_write(void *opaque, uint32_t addr, uint32_t *val)
{
    return false;
}

static const Tc4xSfrModelOps tc4x_sfr_plain_models = {
    .read_lockless = sfr_no_read_lockless,
    .read = sfr_no_read,
    .write = sfr_no_write,
};

static void tc4x_sfr_store_init(Object *obj)
{
    Tc4xSfrStore *st = TC4X_SFR_STORE(obj);
    static const char *const names[2] = { "sfr.e", "sfr.f" };

    for (int i = 0; i < 2; i++) {
        memory_region_init_io(&st->seg[i], obj, &sfr_seg_ops[i], st, names[i],
                              TC4X_SFR_SEGMENT_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &st->seg[i]);
    }
    st->hook_attrs = MEMTXATTRS_UNSPECIFIED;
    st->semantics = g_hash_table_new(g_direct_hash, g_direct_equal);
    st->passthrough = g_array_new(false, false, sizeof(Tc4xSfrPassthrough));
    st->kresets = g_hash_table_new(g_direct_hash, g_direct_equal);
    st->write_hooks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, g_free);
    st->read_hooks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                           NULL, g_free);
    st->watches = g_hash_table_new(g_direct_hash, g_direct_equal);
    st->models = &tc4x_sfr_plain_models;
}

static void tc4x_sfr_store_realize(DeviceState *dev, Error **errp)
{
    Tc4xSfrStore *st = TC4X_SFR_STORE(dev);

    for (int i = 0; i < 2; i++) {
        memory_region_enable_lockless_io(&st->seg[i]);
        memory_region_set_imprecise_io(&st->seg[i], st->imprecise_io);
    }
    tc4x_sfr_store_add_semantics(st, tc4x_reg_semantics, tc4x_n_reg_semantics,
                                 tc4x_kernel_resets, tc4x_n_kernel_resets);
    st->machine_done.notify = tc4x_sfr_store_machine_done;
    qemu_add_machine_init_done_notifier(&st->machine_done);
}

static const Property tc4x_sfr_store_properties[] = {
    DEFINE_PROP_BOOL("imprecise-io", Tc4xSfrStore, imprecise_io, false),
};

static const VMStateDescription vmstate_tc4x_sfr_store = {
    .name = TYPE_TC4X_SFR_STORE,
    .unmigratable = 1,
};

/* hot [MIN]: the registers read at least MIN times since the last hot */
static bool tc4x_sfr_store_sim_run(Tc4xSimCommands *obj, int argc,
                                   char **argv, GString *out)
{
    uint64_t min = 100;

    if (argc > 2 ||
        (argc == 2 && !tc4x_sim_parse_uint(argv[1], UINT32_MAX, &min))) {
        g_string_append(out, "err usage: hot [MIN]\n");
        return true;
    }
    tc4x_sfr_store_hot(TC4X_SFR_STORE(obj), min, out);
    g_string_append(out, "ok\n");
    return true;
}

static const char *const tc4x_sfr_store_sim_commands[] = { "hot", NULL };

static void tc4x_sfr_store_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    Tc4xSimCommandsClass *sc = TC4X_SIM_COMMANDS_CLASS(oc);

    dc->desc = "TC4x SFR store";
    dc->realize = tc4x_sfr_store_realize;
    dc->vmsd = &vmstate_tc4x_sfr_store;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_sfr_store_reset_hold;
    sc->commands = tc4x_sfr_store_sim_commands;
    sc->run = tc4x_sfr_store_sim_run;
    sc->watch = tc4x_sfr_store_sim_watch;
    device_class_set_props(dc, tc4x_sfr_store_properties);
}

static const TypeInfo tc4x_sfr_store_types[] = {
    {
        .name = TYPE_TC4X_SFR_STORE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xSfrStore),
        .instance_init = tc4x_sfr_store_init,
        .class_init = tc4x_sfr_store_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_SIM_COMMANDS },
            { }
        },
    },
};

DEFINE_TYPES(tc4x_sfr_store_types)
