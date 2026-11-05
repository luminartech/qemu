/*
 * TC4x GPIO ports P00..P41.
 *
 * sysbus MMIO region 0: the registers of all ports.  The device serves the
 * output and input registers (OUT, IN, OMR, OMSR, OMCR and the GPIO bits
 * of PADCFGn_GPIO) and passes the others to the SFR store.
 *
 * Lines of port NN (two decimal digits), one per pin:
 * gpio-out "pNN-out"[16]: the output latch.
 * gpio-in "pNN-in"[16]: the level a device outside drives on the pin.
 * gpio-in "pNN-drive"[16]: 1 while that device drives the pin (IN reads
 * the "pNN-in" level), 0 when it lets go (IN reads the output latch).
 * A driver sets the level before it raises "pNN-drive".
 *
 * Property "straps": pins the board holds at a fixed level (pull-ups,
 * straps), each TC4X_PORTS_STRAP(port, pin, level).
 *
 * Locking: register accesses run on vCPU threads without the BQL; writes
 * to a port whose output lines are connected, or that has a listener, take
 * the BQL, and the lines and listeners run under it.  Input lines may be
 * set from any thread.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_PORTS_H
#define HW_TRICORE_TC4X_PORTS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TC4X_PORTS "tc4x-ports"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xPorts, TC4X_PORTS)

#define TC4X_PORT_COUNT         42u
#define TC4X_PORT_PINS          16u

#define TC4X_PORTS_STRAP(port, pin, level) \
    ((uint32_t)(port) << 16 | (uint32_t)(pin) << 8 | (uint32_t)(level))

/* Base and size of the register window of all ports */
uint32_t tc4x_ports_base(void);
uint32_t tc4x_ports_window(void);

/* Output latch of P@port.@pin (any thread) */
bool tc4x_ports_out(Tc4xPorts *p, unsigned port, unsigned pin);

/*
 * Drive P@port.@pin from outside at @level (0 or 1), or let go of it
 * (@level < 0): what the "in" and "drive" lines do (any thread)
 */
void tc4x_ports_drive(Tc4xPorts *p, unsigned port, unsigned pin, int level);

#endif
