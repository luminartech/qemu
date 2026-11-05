/*
 * Internals shared by the tc4x SoC models (tc4x.c, tc4x_gpio.c, tc4x_adc.c,
 * tc4x_i2c.c, tc4x_qspi.c) and the tc4x machines.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_SOC_H
#define HW_TRICORE_TC4X_SOC_H

#include "qemu/units.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_board.h"
#include "hw/tricore/tc4x_i2c.h"
#include "hw/tricore/tc4x_qspi.h"

#define TC4X_NUM_CORES      7           /* CPU0..5 and CPUcs (core 6) */
#define TC4X_CORE_RAM_SIZE  (2 * MiB)       /* DSPR + PSPR (at +1 MiB) */
#define TC4X_LMU_SIZE       (64 * MiB)

#define TC4X_PORT_COUNT     42u

#define ADC_CLKEN_TMADC     0xF5000890u
#define TMADC_BASE          0xF5020000u
#define TMADC_STRIDE        0x800u
#define TMADC_COUNT         8u
#define TMADC_MODCFG        0x000u
#define TMADC_RUN           BIT(1)

typedef struct Tc4xGpio Tc4xGpio;
typedef struct Tc4xAdc Tc4xAdc;

Tc4xGpio *tc4x_soc_gpio(Tc4xSoc *soc);
Tc4xTimebase *tc4x_soc_timebase(Tc4xSoc *soc);

/* Store @val if @addr holds @old; returns the value found (any thread) */
uint32_t tc4x_reg_cmpxchg(Tc4xSoc *soc, uint32_t addr, uint32_t old,
                          uint32_t val);
Tc4xAdc *tc4x_soc_adc(Tc4xSoc *soc);
/* I2C controller @n (below TC4X_I2C_COUNT) */
Tc4xI2c *tc4x_soc_i2c(Tc4xSoc *soc, unsigned n);
/* QSPI module @n (below TC4X_QSPI_COUNT) */
Tc4xQspi *tc4x_soc_qspi(Tc4xSoc *soc, unsigned n);

/* Add register write semantics and kernel resets, replacing earlier ones */
void tc4x_soc_add_semantics(Tc4xSoc *soc, const Tc4xRegSemantics *sem,
                            size_t n_sem, const Tc4xKernelReset *kr,
                            size_t n_kr);

/*
 * Before images are loaded into kept flashes: the reset that follows
 * restores the flash contents, so that the images seed only the RAMs
 */
void tc4x_soc_keep_flash(Tc4xSoc *soc);

/*
 * After the reset: core 0 starts at the address a board set, else at
 * @entry, or stays halted when a board held it
 */
void tc4x_soc_boot(Tc4xSoc *soc, uint32_t entry);

/* Run one sim-bridge command line; replies go to the sim chardev */
void tc4x_sim_exec(Tc4xSoc *soc, char *line);

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

#endif
