/*
 * Internals shared by the tc4x SoC models (tc4x.c, tc4x_gpio.c).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_SOC_H
#define HW_TRICORE_TC4X_SOC_H

#include "hw/tricore/tc4x.h"

#define TC4X_PORT_COUNT     42u

typedef struct Tc4xGpio Tc4xGpio;

Tc4xGpio *tc4x_soc_gpio(Tc4xSoc *soc);

/*
 * The SoC models.  _read and _write return true when the model owns
 * @addr (a write is then consumed: the register file does not store it).
 */
Tc4xGpio *tc4x_gpio_new(Tc4xSoc *soc);
bool tc4x_gpio_read(Tc4xGpio *g, uint32_t addr, uint32_t *val);
bool tc4x_gpio_write(Tc4xGpio *g, uint32_t addr, uint32_t val);

#endif
