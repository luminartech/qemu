/*
 * Internals shared by the tc4x SoC models (tc4x.c, tc4x_gpio.c, tc4x_adc.c,
 * tc4x_i2c.c).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_SOC_H
#define HW_TRICORE_TC4X_SOC_H

#include "hw/tricore/tc4x.h"

#define TC4X_PORT_COUNT     42u

#define ADC_CLKEN_TMADC     0xF5000890u
#define TMADC_BASE          0xF5020000u
#define TMADC_STRIDE        0x800u
#define TMADC_COUNT         8u
#define TMADC_MODCFG        0x000u
#define TMADC_RUN           BIT(1)

typedef struct Tc4xGpio Tc4xGpio;
typedef struct Tc4xAdc Tc4xAdc;
typedef struct Tc4xI2c Tc4xI2c;

Tc4xGpio *tc4x_soc_gpio(Tc4xSoc *soc);

/* Store @val if @addr holds @old; returns the value found (any thread) */
uint32_t tc4x_reg_cmpxchg(Tc4xSoc *soc, uint32_t addr, uint32_t old,
                          uint32_t val);
Tc4xAdc *tc4x_soc_adc(Tc4xSoc *soc);
Tc4xI2c *tc4x_soc_i2c(Tc4xSoc *soc);

/*
 * The SoC models.  _read and _write return true when the model owns
 * @addr (a write is then consumed: the register file does not store it).
 */
Tc4xGpio *tc4x_gpio_new(Tc4xSoc *soc);
bool tc4x_gpio_read(Tc4xGpio *g, uint32_t addr, uint32_t *val);
bool tc4x_gpio_write(Tc4xGpio *g, uint32_t addr, uint32_t val);
/* tc4x_gpio_write without the BQL: false when it needs the BQL */
bool tc4x_gpio_write_lockless(Tc4xGpio *g, uint32_t addr, uint32_t val);

Tc4xAdc *tc4x_adc_new(Tc4xSoc *soc);
bool tc4x_adc_read(Tc4xAdc *a, uint32_t addr, uint32_t *val);
bool tc4x_adc_write(Tc4xAdc *a, uint32_t addr, uint32_t val);

Tc4xI2c *tc4x_i2c_new(Tc4xSoc *soc);
bool tc4x_i2c_owns(uint32_t addr);
bool tc4x_i2c_read(Tc4xI2c *c, uint32_t addr, uint32_t *val);
bool tc4x_i2c_write(Tc4xI2c *c, uint32_t addr, uint32_t val);

#endif
