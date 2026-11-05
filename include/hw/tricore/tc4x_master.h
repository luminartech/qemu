/*
 * Bus masters of the tc4x SoC
 *
 * Every access in the SoC carries its bus master in
 * MemTxAttrs.requester_id: a master index and, for a core, its PSW.S
 * (safe task identifier, TriCore architecture) in bit 0.  The indexes
 * are this model's own numbering: a board maps them onto the bus master
 * IDs its protection units compare.  Unspecified attributes (the monitor,
 * gdbstub without requester IDs, qtest, the loader, generic DMA helpers)
 * count as the debug master.  Devices refuse an access with a requester ID
 * they do not know.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_MASTER_H
#define HW_TRICORE_TC4X_MASTER_H

#include "qemu/log.h"
#include "exec/hwaddr.h"
#include "exec/memattrs.h"

#define TC4X_MASTER_CPU_COUNT   7
#define TC4X_MASTER_DMA_COUNT   256

#define TC4X_MASTER_CPU(n)      (0x001 + (n))   /* CPU0..CPU5, CPUcs */
#define TC4X_MASTER_GETH        0x040
#define TC4X_MASTER_DMA(ch)     (0x100 + (ch))  /* DMA0 and DMA1 channels */
#define TC4X_MASTER_DEBUG       0x7fff          /* the sim bridge */

#define TC4X_REQUESTER_ID(master, safe) (((master) << 1) | !!(safe))
#define TC4X_REQUESTER_MASTER(id)       ((id) >> 1)
#define TC4X_REQUESTER_SAFE(id)         ((id) & 1)

/* The attributes of @master's accesses, with PSW.S = @safe for a core */
static inline MemTxAttrs tc4x_master_attrs(unsigned master, bool safe)
{
    return (MemTxAttrs) { .requester_id = TC4X_REQUESTER_ID(master, safe) };
}

static inline bool tc4x_master_known(MemTxAttrs attrs)
{
    unsigned m = TC4X_REQUESTER_MASTER(attrs.requester_id);

    return attrs.unspecified ||
           (m >= TC4X_MASTER_CPU(0) &&
            m < TC4X_MASTER_CPU(TC4X_MASTER_CPU_COUNT)) ||
           (m >= TC4X_MASTER_DMA(0) &&
            m < TC4X_MASTER_DMA(TC4X_MASTER_DMA_COUNT)) ||
           m == TC4X_MASTER_GETH || m == TC4X_MASTER_DEBUG;
}

/* The core whose access @attrs describes, or -1 for any other master */
static inline int tc4x_master_core(MemTxAttrs attrs)
{
    unsigned m = TC4X_REQUESTER_MASTER(attrs.requester_id);

    if (attrs.unspecified || m < TC4X_MASTER_CPU(0) ||
        m >= TC4X_MASTER_CPU(TC4X_MASTER_CPU_COUNT)) {
        return -1;
    }
    return m - TC4X_MASTER_CPU(0);
}

/*
 * Whether device @dev refuses the access at @addr: true, with a guest
 * error logged, if its requester ID is no known master's (the device then
 * answers MEMTX_ERROR)
 */
static inline bool tc4x_master_refused(MemTxAttrs attrs, const char *dev,
                                       hwaddr addr, bool write)
{
    if (likely(tc4x_master_known(attrs))) {
        return false;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: %s at 0x%08" HWADDR_PRIx " by an "
                  "unknown bus master (requester 0x%x) refused\n", dev,
                  write ? "write" : "read", addr, attrs.requester_id);
    return true;
}

#endif
