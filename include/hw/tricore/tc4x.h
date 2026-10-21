/*
 * Services of the tc4x machine for board code: the code that adds what
 * sits around the SoC on one board.
 *
 * Unless noted otherwise, a function runs in the main loop with the BQL
 * held.  "Any thread" functions may also run on a vCPU thread without the
 * BQL.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_H
#define HW_TRICORE_TC4X_H

typedef struct Tc4xSoc Tc4xSoc;

/* Stored value of the SFR at @addr (any thread) */
uint32_t tc4x_reg_get(Tc4xSoc *soc, uint32_t addr);

/* Store @val at @addr, no side effects (any thread) */
void tc4x_reg_set(Tc4xSoc *soc, uint32_t addr, uint32_t val);

/*
 * A slave on one chip select of a QSPI module.  BACONENTRY writes that
 * select it start a frame (@bacon: word length, bit order); every TX word
 * clocks one word in and its reply into the RX FIFO.  Called with the QSPI
 * lock held, from any thread: the ops must not take the BQL.
 */
typedef struct Tc4xQspiSlaveOps {
    void (*frame)(void *opaque, uint32_t bacon);
    uint32_t (*word)(void *opaque, uint32_t mosi);
} Tc4xQspiSlaveOps;

void tc4x_qspi_attach(Tc4xSoc *soc, unsigned module, unsigned cs,
                      const Tc4xQspiSlaveOps *ops, void *opaque);

/*
 * Board code writes the receive data of QSPI @module straight to where its
 * receive DMA would put it: its hardware-request DMA lists do not run.
 */
void tc4x_qspi_set_rx_fed(Tc4xSoc *soc, unsigned module, bool fed);

/* The lock QSPI slaves run under, for board code that shares their state */
void tc4x_qspi_lock(Tc4xSoc *soc);
void tc4x_qspi_unlock(Tc4xSoc *soc);

/* Output latch of P@port.@pin (any thread) */
bool tc4x_gpio_out(Tc4xSoc *soc, unsigned port, unsigned pin);

/* Drive (level 0/1) or release (level < 0) an input */
void tc4x_gpio_set_input(Tc4xSoc *soc, unsigned port, unsigned pin,
                         int level);

/* @fn sees every change of port @port's output latch, @old to @new */
typedef void (*Tc4xGpioListenerFn)(void *opaque, unsigned port, uint32_t old,
                                   uint32_t new);
void tc4x_gpio_add_listener(Tc4xSoc *soc, unsigned port,
                            Tc4xGpioListenerFn fn, void *opaque);

/*
 * Sim-bridge and sfr-script commands whose name starts with @prefix.  @fn
 * returns false for a command it does not know, else appends its reply
 * lines (ending in "ok ..." or "err ...") to @out.  With @qspi_lock the
 * command runs under the QSPI lock (it touches QSPI slave state).
 */
typedef bool (*Tc4xSimCommandFn)(void *opaque, int argc, char **argv,
                                 GString *out);
void tc4x_sim_add_command(Tc4xSoc *soc, const char *prefix,
                          Tc4xSimCommandFn fn, void *opaque, bool qspi_lock);

#endif
