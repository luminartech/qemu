/*
 * TC4x DMA: a board's handling of DMA bus errors
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_DMA_H
#define HW_TRICORE_TC4X_DMA_H

#include "qom/object.h"

/* ERRSR flags of a resource partition (iLLD IfxDma_regdef.h) */
#define TC4X_DMA_ERR_SER        BIT(16)     /* source error */
#define TC4X_DMA_ERR_DER        BIT(17)     /* destination error */
#define TC4X_DMA_ERR_DLLER      BIT(26)     /* linked list load error */

#define TYPE_TC4X_DMA_POLICY "tc4x-dma-policy"
typedef struct Tc4xDmaPolicyClass Tc4xDmaPolicyClass;
DECLARE_CLASS_CHECKERS(Tc4xDmaPolicyClass, TC4X_DMA_POLICY,
                       TYPE_TC4X_DMA_POLICY)
#define TC4X_DMA_POLICY(obj) \
    INTERFACE_CHECK(Tc4xDmaPolicy, (obj), TYPE_TC4X_DMA_POLICY)

typedef struct Tc4xDmaPolicy Tc4xDmaPolicy;

/*
 * The board's DMA rules, linked as the SoC's dma-policy.  The SoC reports
 * a bus error in the ERRSR of the channel's resource partition; what the
 * channel does beyond that is the policy's.  Without one the channel goes
 * on with the data the bus returned.
 */
struct Tc4xDmaPolicyClass {
    InterfaceClass parent_class;

    /*
     * Sees the bus error @err (TC4X_DMA_ERR_*) of channel @ch (DMA0's
     * channels, then DMA1's) at @addr when the access fails, under the
     * BQL.  Returns true to drop what the failed access delivers: after
     * a source error the move's write, after a linked-list load error the
     * loaded TCS, which ends the list.  After a destination error the
     * result has no effect.  The flags of source and destination errors
     * are set when the transaction ends, those of a linked-list load
     * error before the call.
     */
    bool (*bus_error)(Tc4xDmaPolicy *p, unsigned ch, uint32_t err,
                      uint32_t addr);
};

#endif
