/*
 * Internals shared by the tc4x SoC models (tc4x.c, tc4x_ports.c,
 * tc4x_tmadc.c, tc4x_i2c.c, tc4x_qspi.c) and the tc4x machines.
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
#include "hw/tricore/tc4x_ports.h"
#include "hw/tricore/tc4x_qspi.h"
#include "hw/tricore/tc4x_tmadc.h"

#define TC4X_NUM_CORES      7           /* CPU0..5 and CPUcs (core 6) */
#define TC4X_CORE_RAM_SIZE  (2 * MiB)       /* DSPR + PSPR (at +1 MiB) */
#define TC4X_LMU_SIZE       (64 * MiB)


Tc4xPorts *tc4x_soc_ports(Tc4xSoc *soc);
Tc4xTimebase *tc4x_soc_timebase(Tc4xSoc *soc);

/* Store @val if @addr holds @old; returns the value found (any thread) */
uint32_t tc4x_reg_cmpxchg(Tc4xSoc *soc, uint32_t addr, uint32_t old,
                          uint32_t val);
Tc4xTmadc *tc4x_soc_tmadc(Tc4xSoc *soc);
/* I2C controller @n (below TC4X_I2C_COUNT) */
Tc4xI2c *tc4x_soc_i2c(Tc4xSoc *soc, unsigned n);
/* QSPI module @n (below TC4X_QSPI_COUNT) */
Tc4xQspi *tc4x_soc_qspi(Tc4xSoc *soc, unsigned n);

/* Add register write semantics and kernel resets, replacing earlier ones */
void tc4x_soc_add_semantics(Tc4xSoc *soc, const Tc4xRegSemantics *sem,
                            size_t n_sem, const Tc4xKernelReset *kr,
                            size_t n_kr);

Tc4xFlashState *tc4x_soc_flash(Tc4xSoc *soc);

/*
 * The flash's side of the board API in tc4x.h: tc4x_flash_set_locate(),
 * tc4x_dmu_set_hook() and tc4x_dmu_error() (on the interface running the
 * current command, else the host's)
 */
void tc4x_flash_set_locate_fn(Tc4xFlashState *s, Tc4xFlashLocateFn fn,
                              void *opaque);
void tc4x_flash_set_hook_fn(Tc4xFlashState *s, Tc4xDmuHookFn fn,
                            void *opaque);
void tc4x_flash_dmu_error_running(Tc4xFlashState *s, uint32_t err);

/*
 * After the reset: core 0 starts at the address a board set, else at
 * @entry, or stays halted when a board held it
 */
void tc4x_soc_boot(Tc4xSoc *soc, uint32_t entry);

/*
 * @fn sees every change of port @port's output latch (machine init only;
 * fails with an error for a port that does not exist)
 */
void tc4x_ports_add_listener(Tc4xPorts *p, unsigned port,
                             Tc4xGpioListenerFn fn, void *opaque);

#endif
