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
#include "qom/object.h"

#define TYPE_TC4X_GETH "tc4x-geth"

/*
 * A PHY on the MDIO bus of one GETH port: the object that link "phy0" or
 * "phy1" of the controller names, at MDIO address "phy0-addr" or
 * "phy1-addr".  MDIO reads of the port at other addresses, or of a port
 * without a PHY, return all ones.  Called with the BQL held.
 */
#define TYPE_TC4X_GETH_PHY "tc4x-geth-phy"

typedef struct Tc4xGethPhyClass Tc4xGethPhyClass;
DECLARE_CLASS_CHECKERS(Tc4xGethPhyClass, TC4X_GETH_PHY, TYPE_TC4X_GETH_PHY)
#define TC4X_GETH_PHY(obj) \
    INTERFACE_CHECK(Tc4xGethPhy, (obj), TYPE_TC4X_GETH_PHY)

typedef struct Tc4xGethPhy Tc4xGethPhy;

struct Tc4xGethPhyClass {
    InterfaceClass parent_class;

    /* Clause 22 frames (NULL: reads return all ones, writes are ignored) */
    uint16_t (*read22)(Tc4xGethPhy *phy, unsigned reg);
    void (*write22)(Tc4xGethPhy *phy, unsigned reg, uint16_t val);
    /* Clause 45 frames with their MMD (NULL: as above) */
    uint16_t (*read45)(Tc4xGethPhy *phy, unsigned mmd, unsigned reg);
    void (*write45)(Tc4xGethPhy *phy, unsigned mmd, unsigned reg,
                    uint16_t val);
    /*
     * The link of the port's MAC side: up while the port has a network
     * connection whose link is up (a netdev, see QMP set_link, or a port
     * backend).  Called when the controller realizes and on every change;
     * may be NULL.
     */
    void (*port_link)(Tc4xGethPhy *phy, bool up);
};

/*
 * Legacy PHY attach: operations with an opaque pointer.  Clause 22 frames
 * reach read22 and write22, Clause 45 frames read45 and write45 with their
 * MMD; reset (may be NULL) runs with the controller's reset.  Called with
 * the BQL held.
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
#define TC4X_HSPHY_BASE       0xF2000000u
#define TC4X_HSPHY_SIZE       0x2000000u

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
 * Before realize: take the -nic configurations of models "tc4x-geth"
 * (port 0) and "tc4x-geth-port1" (port 1)
 */
void tc4x_geth_configure_nics(DeviceState *dev);

/*
 * Attach @ops as the PHY at MDIO address @addr of @port after the
 * controller realized, in place of its PHY link (TYPE_TC4X_GETH_PHY)
 */
void tc4x_geth_attach_phy(DeviceState *dev, unsigned port, uint8_t addr,
                          const Tc4xGethPhyOps *ops, void *opaque);

#endif
