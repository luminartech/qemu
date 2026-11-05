/*
 * TC4x QSPI module (QSPI0..7) as the master of an SSI bus.
 *
 * sysbus MMIO region 0: the module's registers (CLC .. MSSMUX).  The
 * device serves the transmit, receive and status registers and passes the
 * others to the SFR store.
 * Child bus "ssi" (TYPE_TC4X_QSPI_BUS): the peripherals on the module's
 * chip selects, TYPE_TC4X_QSPI_PERIPHERAL only (other SSI peripherals are
 * refused at realize: the bus runs without the BQL).
 * gpio-out "cs"[TC4X_QSPI_CS_COUNT]: the slave select outputs SLSO0..15,
 * low while a frame on that chip select runs; wire each to the
 * SSI_GPIO_CS input of the peripheral at that "cs" index
 * (tc4x_qspi_realize_peripheral() does both).
 * sysbus IRQs TC4X_QSPI_IRQ_*: the module's service requests.
 *
 * Locking: register accesses run on vCPU threads without the BQL, under
 * the module's bus lock (see TYPE_TC4X_QSPI_PERIPHERAL).  Holders of the
 * BQL take the bus lock after it.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_QSPI_H
#define HW_TRICORE_TC4X_QSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_TC4X_QSPI "tc4x-qspi"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xQspi, TC4X_QSPI)

/* The SSI bus of a module: refuses peripherals of other types */
#define TYPE_TC4X_QSPI_BUS "tc4x-qspi-bus"

#define TC4X_QSPI_COUNT         8u
#define TC4X_QSPI_CS_COUNT      16u

/* sysbus IRQs, in the order of the module's service request nodes */
enum {
    TC4X_QSPI_IRQ_TX,
    TC4X_QSPI_IRQ_RX,
    TC4X_QSPI_IRQ_ERR,
    TC4X_QSPI_IRQ_PT,
    TC4X_QSPI_IRQ_U,
    TC4X_QSPI_IRQ_COUNT,
};

/* Base of the registers of module @n */
uint32_t tc4x_qspi_base(unsigned n);

/* Size of the register window of one module */
uint32_t tc4x_qspi_window(void);

/* The module's bus */
SSIBus *tc4x_qspi_bus(Tc4xQspi *q);

/*
 * The bus lock of the module that is master of @bus, for code under the
 * BQL that shares state with its peripherals (taken after the BQL;
 * recursive)
 */
void tc4x_qspi_bus_lock(SSIBus *bus);
void tc4x_qspi_bus_unlock(SSIBus *bus);

/*
 * Realize @dev, a TYPE_TC4X_QSPI_PERIPHERAL with its "cs" property set,
 * on the bus of @q, and wire that chip select output to it.  Machine init
 * only.
 */
bool tc4x_qspi_realize_peripheral(Tc4xQspi *q, DeviceState *dev,
                                  Error **errp);

/*
 * The SoC's DMA, which serves the hardware requests of the modules:
 * armed() tells whether a channel with hardware requests enabled moves
 * data to or from module @n (any thread); run() runs those channels'
 * transmit lists and then their receive lists, under the BQL, and returns
 * whether a transmit list ran.
 */
typedef struct Tc4xQspiDmaOps {
    bool (*armed)(void *opaque, unsigned n);
    bool (*run)(void *opaque, unsigned n);
} Tc4xQspiDmaOps;

void tc4x_qspi_set_dma(Tc4xQspi *q, const Tc4xQspiDmaOps *ops, void *opaque);

/*
 * A peripheral on one chip select of a QSPI module (an SSI peripheral with
 * an active-low chip select).  A BACONENTRY write that selects it, or the
 * first word after a frame under that entry ended (BACON.LAST), starts a
 * frame: frame() sees the BACON entry (word length, bit order, chip
 * select); each word written to DATAENTRY is then one transfer() whose
 * result goes to the receive FIFO.
 *
 * Bus lock contract: frame(), transfer() and set_cs() run with the bus
 * lock of the module held, on a vCPU thread without the BQL or on a thread
 * that holds it.  They must not take the BQL.  State a peripheral shares
 * with code under the BQL (timers, sim commands, properties) is accessed
 * there under tc4x_qspi_bus_lock().
 */
#define TYPE_TC4X_QSPI_PERIPHERAL "tc4x-qspi-peripheral"
OBJECT_DECLARE_TYPE(Tc4xQspiPeripheral, Tc4xQspiPeripheralClass,
                    TC4X_QSPI_PERIPHERAL)

struct Tc4xQspiPeripheral {
    SSIPeripheral parent_obj;
};

struct Tc4xQspiPeripheralClass {
    SSIPeripheralClass parent_class;

    void (*frame)(Tc4xQspiPeripheral *p, uint32_t bacon);
};

/*
 * A peripheral of the Tc4xQspiSlaveOps (tc4x.h).  Created by
 * tc4x_qspi_attach() only.
 */
#define TYPE_TC4X_QSPI_LEGACY "tc4x-qspi-legacy"

#endif
