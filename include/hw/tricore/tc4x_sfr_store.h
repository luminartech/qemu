/*
 * TC4x SFR store: the background of the SoC's SFR space (0xE0000000 and
 * 0xF0000000 segments).  Every register no device models is plain storage
 * here, with the write semantics of the iLLD access types (bits that a
 * written 1 or 0 clears or sets) and the kernel resets of the modules.
 * Devices map their own registers over it at a higher priority.
 *
 * Unless noted otherwise, a function runs with the BQL held.  "Any thread"
 * functions may also run on a vCPU thread without the BQL.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_SFR_STORE_H
#define HW_TRICORE_TC4X_SFR_STORE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TC4X_SFR_STORE "tc4x-sfr-store"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xSfrStore, TC4X_SFR_STORE)

/* sysbus MMIO regions 0 and 1: the segments at their bases */
#define TC4X_SFR_SEGMENT_E      0xE0000000u
#define TC4X_SFR_SEGMENT_F      0xF0000000u
#define TC4X_SFR_SEGMENT_SIZE   0x10000000u

/* Priority of the store below the devices that overlay it */
#define TC4X_SFR_STORE_PRIORITY (-1000)

/*
 * Software write semantics of one register: bits that store what is
 * written, bits a written 1 clears, a written 0 clears, a written 1 sets.
 * Registers without an entry store every write as it is.
 */
typedef struct Tc4xRegSemantics {
    uint32_t addr;
    uint32_t persist;
    uint32_t w1c;
    uint32_t w0c;
    uint32_t w1s;
} Tc4xRegSemantics;

/*
 * A module's kernel reset registers: writing CTRLB.KRST completes the reset
 * at once (STAT.KRST set, the KRST request bits of CTRLA and CTRLB clear);
 * CTRLB.STATCLR clears STAT.KRST
 */
typedef struct Tc4xKernelReset {
    uint32_t ctrla;
    uint32_t ctrlb;
    uint32_t stat;
} Tc4xKernelReset;

/* The SoC's tables, generated from the iLLD SFR headers (tc4x_sfr_tables.c) */
extern const Tc4xRegSemantics tc4x_reg_semantics[];
extern const size_t tc4x_n_reg_semantics;
extern const Tc4xKernelReset tc4x_kernel_resets[];
extern const size_t tc4x_n_kernel_resets;

/* Add write semantics and kernel resets, replacing earlier entries */
void tc4x_sfr_store_add_semantics(Tc4xSfrStore *st, const Tc4xRegSemantics *sem,
                                  size_t n_sem, const Tc4xKernelReset *kr,
                                  size_t n_kr);

/* Stored value of @addr, without side effects (any thread) */
uint32_t tc4x_sfr_store_get(Tc4xSfrStore *st, uint32_t addr);

/* Store @val at @addr, without side effects (any thread) */
void tc4x_sfr_store_set(Tc4xSfrStore *st, uint32_t addr, uint32_t val);

/* Set @bits of @addr atomically (any thread) */
void tc4x_sfr_store_or(Tc4xSfrStore *st, uint32_t addr, uint32_t bits);

/* Store @val if @addr holds @old; returns the value found (any thread) */
uint32_t tc4x_sfr_store_cmpxchg(Tc4xSfrStore *st, uint32_t addr, uint32_t old,
                                uint32_t val);

/* A guest write of @val to @addr through its semantics and kernel reset */
void tc4x_sfr_store_write(Tc4xSfrStore *st, uint32_t addr, uint32_t val);

/* Pin the bits in @mask of @addr to @value for every read (mask 0: release) */
void tc4x_sfr_store_force(Tc4xSfrStore *st, uint32_t addr, uint32_t value,
                          uint32_t mask);

/*
 * The device whose registers overlay the store at @addr, or NULL when the
 * store serves @addr
 */
Object *tc4x_sfr_store_owner(Tc4xSfrStore *st, uint32_t addr);

/*
 * @mr, a region of another device over the store, serves some registers
 * itself and passes the other accesses to tc4x_sfr_store_access_read/write:
 * its addresses count as the store's
 */
void tc4x_sfr_store_add_passthrough(Tc4xSfrStore *st, MemoryRegion *mr);


/*
 * Append "hot ADDR READS VALUE" lines for the registers read at least @min
 * times since the last call, and reset the counts
 */
void tc4x_sfr_store_hot(Tc4xSfrStore *st, uint32_t min, GString *out);

/*
 * Full-word writes of @addr may skip the BQL once a write under the BQL
 * found it plain storage (no watch, no hook, no bits that clear or set)
 */
void tc4x_sfr_store_allow_lockless_write(Tc4xSfrStore *st, uint32_t addr);

/* Accesses of @addr take the BQL (any thread) */
bool tc4x_sfr_store_bql_only(Tc4xSfrStore *st, uint32_t addr);

/*
 * The store's MMIO access path for regions that forward to it (any
 * thread; takes the BQL where the access needs it), with the attributes
 * of the access for the hooks and models it calls
 */
uint64_t tc4x_sfr_store_access_read(Tc4xSfrStore *st, uint32_t addr,
                                    unsigned size, MemTxAttrs attrs);
void tc4x_sfr_store_access_write(Tc4xSfrStore *st, uint32_t addr, uint64_t val,
                                 unsigned size, MemTxAttrs attrs);

/*
 * From a hook or a model callback: the attributes of the guest access that
 * runs it; MEMTXATTRS_UNSPECIFIED outside one (any thread)
 */
MemTxAttrs tc4x_sfr_store_hook_attrs(Tc4xSfrStore *st);

/*
 * Hooks of board code that has no device of its own yet: guest writes of
 * @addr call @fn first, and the write is consumed when it returns true;
 * guest reads call @fn, and *@val is the value read when it returns true.
 * Both run under the BQL.  Added during machine init only, at addresses
 * the store serves.
 */
typedef bool (*Tc4xSfrWriteFn)(void *opaque, uint32_t addr, uint32_t val);
typedef bool (*Tc4xSfrReadFn)(void *opaque, uint32_t addr, uint32_t *val);
void tc4x_sfr_store_add_write_hook(Tc4xSfrStore *st, uint32_t addr,
                                   Tc4xSfrWriteFn fn, void *opaque);
void tc4x_sfr_store_add_read_hook(Tc4xSfrStore *st, uint32_t addr,
                                  Tc4xSfrReadFn fn, void *opaque);

/*
 * The SoC's peripheral models that the store's regions still serve.
 * read_lockless and write_lockless run on vCPU threads without the BQL;
 * the others run under the BQL, after the hooks.
 */
typedef enum Tc4xSfrPath {
    TC4X_SFR_PATH_STORE,    /* a plain register: the store serves it */
    TC4X_SFR_PATH_SERVED,   /* the model served it */
    TC4X_SFR_PATH_LOCKED,   /* take the BQL */
} Tc4xSfrPath;

typedef struct Tc4xSfrModelOps {
    /* @addr as accessed; *@val holds the stored word */
    Tc4xSfrPath (*read_lockless)(void *opaque, uint32_t addr, unsigned size,
                                 uint32_t *val);
    /* a full aligned word: true when the model took the write (optional) */
    bool (*write_lockless)(void *opaque, uint32_t addr, uint32_t val);
    /* a word: *@val holds the stored word; true when the model served it */
    bool (*read)(void *opaque, uint32_t addr, uint32_t *val);
    /*
     * A word: true when the model consumed the write; else the store
     * stores *@val, which the model may change
     */
    bool (*write)(void *opaque, uint32_t addr, uint32_t *val);
} Tc4xSfrModelOps;

void tc4x_sfr_store_set_models(Tc4xSfrStore *st, const Tc4xSfrModelOps *ops,
                               void *opaque);

#endif
