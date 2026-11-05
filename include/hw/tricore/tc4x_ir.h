/*
 * TC4x interrupt router: the service request nodes (SRC registers) of the
 * SoC and the arbitration that presents each CPU its highest pending
 * priority.
 *
 * Named gpio-in lines: "src"[n] sets SRR of the node at
 * TC4X_IR_SRC_BASE + 4 * n when raised (a hardware service request; a node
 * already pending sets IOV); "nmi"[c] raises the non-maskable interrupt on
 * CPU c.  Both can be driven from any thread.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_IR_H
#define HW_TRICORE_TC4X_IR_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_TC4X_IR "tc4x-ir"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xIr, TC4X_IR)

/* sysbus MMIO region 0: the SRC registers */
#define TC4X_IR_SRC_BASE    0xF4432000u
#define TC4X_IR_SRC_SIZE    0x2000u
#define TC4X_IR_NUM_SRC     (TC4X_IR_SRC_SIZE / 4)
#define TC4X_IR_NUM_CPUS    7

/* The node at @src is one of the router's */
static inline bool tc4x_ir_is_src(uint32_t src)
{
    return src >= TC4X_IR_SRC_BASE && src < TC4X_IR_SRC_BASE + TC4X_IR_SRC_SIZE;
}

/* The "src" line of the node at @src (any thread) */
qemu_irq tc4x_ir_src(Tc4xIr *ir, uint32_t src);

/* The SRC register at @src (any thread) */
uint32_t tc4x_ir_src_get(Tc4xIr *ir, uint32_t src);

/* Store @val in the SRC register at @src, without side effects */
void tc4x_ir_src_set(Tc4xIr *ir, uint32_t src, uint32_t val);

/*
 * @fn sees every acknowledge of the node at @src, in the main loop.  Added
 * during machine init only.
 */
typedef void (*Tc4xIrAckFn)(void *opaque, uint32_t src);
void tc4x_ir_add_ack_listener(Tc4xIr *ir, uint32_t src, Tc4xIrAckFn fn,
                              void *opaque);

/*
 * Guest writes of the SRC register at @src call @fn before they take effect
 * (@on), or no longer (!@on).  One @fn for all watched registers.
 */
typedef void (*Tc4xIrWatchFn)(void *opaque, uint32_t src, uint32_t val,
                              unsigned size);
void tc4x_ir_set_watch_fn(Tc4xIr *ir, Tc4xIrWatchFn fn, void *opaque);
void tc4x_ir_watch(Tc4xIr *ir, uint32_t src, bool on);

#endif
