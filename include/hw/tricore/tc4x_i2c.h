/*
 * TC4x I2C controller (I2C0..2) as the master of an I2C bus.
 *
 * sysbus MMIO region 0: the module's kernel registers (CLC1 .. RXD).  The
 * device serves the transfer registers and passes the others to the SFR
 * store.  Child bus "i2c": the slaves (TYPE_I2C_SLAVE) the controller
 * addresses.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_I2C_H
#define HW_TRICORE_TC4X_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_TC4X_I2C "tc4x-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xI2c, TC4X_I2C)

#define TC4X_I2C_COUNT          3u
#define TC4X_I2C_WINDOW         0x10000u

/* Base of the kernel registers of module @n */
uint32_t tc4x_i2c_base(unsigned n);

/* The controller's bus */
I2CBus *tc4x_i2c_bus(Tc4xI2c *c);

/*
 * A slave of the whole-transfer Tc4xI2cSlaveOps (tc4x.h) on an I2C bus.
 * Created by tc4x_i2c_attach() only.
 */
#define TYPE_TC4X_I2C_LEGACY "tc4x-i2c-legacy"

#endif
