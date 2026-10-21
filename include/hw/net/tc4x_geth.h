/*
 * TC4x GETH Ethernet controller (Synopsys DWC Ethernet QoS based)
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_TC4X_GETH_H
#define HW_NET_TC4X_GETH_H

#include "hw/core/sysbus.h"

#define TYPE_TC4X_GETH "tc4x-geth"

/*
 * A PHY on the MDIO bus of one port.  Clause 22 frames reach read22 and
 * write22, Clause 45 frames read45 and write45 with their MMD; reset (may
 * be NULL) runs with the controller's reset.  Called with the BQL held.
 */
typedef struct Tc4xGethPhyOps {
    uint16_t (*read22)(void *opaque, unsigned reg);
    void (*write22)(void *opaque, unsigned reg, uint16_t val);
    uint16_t (*read45)(void *opaque, unsigned mmd, unsigned reg);
    void (*write45)(void *opaque, unsigned mmd, unsigned reg, uint16_t val);
    void (*reset)(void *opaque);
} Tc4xGethPhyOps;

/* -nic model name that selects GETH port 1 (port 0 matches "tc4x-geth") */
#define TC4X_GETH_PORT1_MODEL "tc4x-geth-port1"

#define TC4X_GETH_BASE        0xF9000000u
#define TC4X_GETH_SIZE        0x20000u

#define TC4X_GETH_NUM_PORTS   2
#define TC4X_GETH_NUM_DMA     8

/*
 * Output IRQ lines (sysbus irq index):
 *   0..7   TX DMA channel 0..7
 *   8..15  RX DMA channel 0..7
 *   16     MAC
 */
#define TC4X_GETH_IRQ_TX(ch)  (ch)
#define TC4X_GETH_IRQ_RX(ch)  (TC4X_GETH_NUM_DMA + (ch))
#define TC4X_GETH_IRQ_MAC     (2 * TC4X_GETH_NUM_DMA)
#define TC4X_GETH_NUM_IRQ     (TC4X_GETH_IRQ_MAC + 1)

/*
 * Create and realize GETH0 with the -nic configurations of models
 * "tc4x-geth" (port 0) and "tc4x-geth-port1" (port 1), and map it over
 * @sysmem with priority 1.
 */
DeviceState *tc4x_geth_create(MemoryRegion *sysmem);

/*
 * Attach the PHY that board code wires to MDIO address @addr of @port.
 * Without one, MDIO reads of the port, or of any other address, return
 * all ones.
 */
void tc4x_geth_attach_phy(DeviceState *dev, unsigned port, uint8_t addr,
                          const Tc4xGethPhyOps *ops, void *opaque);

#endif
