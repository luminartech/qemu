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

#include "hw/net/tc4x_geth.h"

typedef struct Tc4xSoc Tc4xSoc;

/* Stored value of the SFR at @addr (any thread) */
uint32_t tc4x_reg_get(Tc4xSoc *soc, uint32_t addr);

/* Store @val at @addr, no side effects (any thread) */
void tc4x_reg_set(Tc4xSoc *soc, uint32_t addr, uint32_t val);

/*
 * Guest writes of @addr call @fn first; when it returns true the write is
 * consumed, else it continues as if there were no hook.  Writes of @addr
 * always take the BQL.  Hooks are added during machine init only.
 */
typedef bool (*Tc4xSfrWriteFn)(void *opaque, uint32_t addr, uint32_t val);
void tc4x_sfr_add_write_hook(Tc4xSoc *soc, uint32_t addr, Tc4xSfrWriteFn fn,
                             void *opaque);

/* Hardware-set SRR of the service request node at @src (any thread) */
void tc4x_irq_raise(Tc4xSoc *soc, uint32_t src);

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

/* Attach a PHY at MDIO address @addr of GETH0 @port (see tc4x_geth.h) */
void tc4x_geth_attach(Tc4xSoc *soc, unsigned port, uint8_t addr,
                      const Tc4xGethPhyOps *ops, void *opaque);

/* Output latch of P@port.@pin (any thread) */
bool tc4x_gpio_out(Tc4xSoc *soc, unsigned port, unsigned pin);

/* Drive (level 0/1) or release (level < 0) an input (any thread) */
void tc4x_gpio_set_input(Tc4xSoc *soc, unsigned port, unsigned pin,
                         int level);

/*
 * @fn sees every change of port @port's output latch, @old to @new; writes
 * to the port then always take the BQL
 */
typedef void (*Tc4xGpioListenerFn)(void *opaque, unsigned port, uint32_t old,
                                   uint32_t new);
void tc4x_gpio_add_listener(Tc4xSoc *soc, unsigned port,
                            Tc4xGpioListenerFn fn, void *opaque);

/* Pin voltage of channel @ch of kernel @kernel */
double tc4x_adc_volts(Tc4xSoc *soc, unsigned kernel, unsigned ch);

/* Set the pin voltage of channel @ch of kernel @kernel */
void tc4x_adc_set_volts(Tc4xSoc *soc, unsigned kernel, unsigned ch,
                        double volts);

/*
 * @fn supplies the pin voltage of channel @ch of kernel @kernel at each
 * conversion, in place of the voltage set (any thread)
 */
typedef double (*Tc4xAdcInputFn)(void *opaque, unsigned kernel, unsigned ch);
void tc4x_adc_set_input(Tc4xSoc *soc, unsigned kernel, unsigned ch,
                        Tc4xAdcInputFn fn, void *opaque);

/* Kernel running and service request line @sr enabled */
bool tc4x_adc_sr_enabled(Tc4xSoc *soc, unsigned kernel, unsigned sr);

/* Conversion period of service request line @sr (0: no periodic rounds) */
void tc4x_adc_set_period(Tc4xSoc *soc, unsigned kernel, unsigned sr,
                         int64_t period_ns);

/*
 * A slave at one address of a bus.  A transfer completes at once: start
 * (may be NULL) sees the address phase and NACKs it by returning false,
 * then write or read moves all data bytes and stop (may be NULL) ends it.
 */
typedef struct Tc4xI2cSlaveOps {
    bool (*start)(void *opaque, bool read);
    void (*write)(void *opaque, const uint8_t *data, unsigned len);
    void (*read)(void *opaque, uint8_t *data, unsigned len);
    void (*stop)(void *opaque);
} Tc4xI2cSlaveOps;

/* Attach a slave at @addr of I2C@bus, replacing the one there */
void tc4x_i2c_attach(Tc4xSoc *soc, unsigned bus, uint8_t addr,
                     const Tc4xI2cSlaveOps *ops, void *opaque);

/* The program and data flash, as storage */
typedef enum Tc4xFlash {
    TC4X_PFLASH,
    TC4X_DFLASH,
} Tc4xFlash;

/* The flashes came from flash-dir files of an earlier run (board init on) */
bool tc4x_flash_kept(Tc4xSoc *soc);

/* Read or write flash storage directly, without the DMU */
void tc4x_flash_read(Tc4xSoc *soc, Tc4xFlash flash, uint64_t off, void *buf,
                     uint64_t len);
void tc4x_flash_write(Tc4xSoc *soc, Tc4xFlash flash, uint64_t off,
                      const void *buf, uint64_t len);

/*
 * Where DMU commands find their addresses: @fn places [@addr, @addr+@len)
 * in one flash (@flash, @off) or returns false, and the command fails.
 * Without one (or with NULL), the PFLASH storage is linear from both read
 * windows and the DFLASH from 0xAE000000.
 */
typedef bool (*Tc4xFlashLocateFn)(void *opaque, uint32_t addr, uint64_t len,
                                  Tc4xFlash *flash, uint64_t *off);
void tc4x_flash_set_locate(Tc4xSoc *soc, Tc4xFlashLocateFn fn, void *opaque);

/*
 * The PFLASH read windows (cached at 0x80000000, non-cached at 0xA0000000)
 * show the storage linearly; each map shows @size bytes of storage from
 * @storage at window offset @win instead.  A call replaces the maps of
 * the previous one; n = 0 makes the windows linear again.
 */
#define TC4X_PFLASH_MAPS_MAX 16
typedef struct Tc4xPflashMap {
    uint64_t win;
    uint64_t size;
    uint64_t storage;
} Tc4xPflashMap;
void tc4x_pflash_set_maps(Tc4xSoc *soc, const Tc4xPflashMap *maps,
                          unsigned n);

/* A DMU command as the board's hook sees it, before it runs */
typedef enum Tc4xDmuOp {
    TC4X_DMU_PROGRAM,       /* write page or burst: @len bytes of @data */
    TC4X_DMU_ERASE,         /* erase logical sectors: @len bytes */
    TC4X_DMU_VERIFY,        /* verify erased: @len bytes */
    TC4X_DMU_RESET_TO_READ,
    TC4X_DMU_OTHER,         /* @code: the 5554 value, or class << 8 | opcode */
} Tc4xDmuOp;

typedef struct Tc4xDmuCommand {
    Tc4xDmuOp op;
    uint32_t addr;          /* command address */
    uint32_t code;
    bool located;           /* @flash and @off hold @addr (program..verify) */
    Tc4xFlash flash;
    uint64_t off;
    uint64_t len;
    const uint8_t *data;
} Tc4xDmuCommand;

/*
 * @fn sees every DMU command before it runs and returns true when it
 * executed or refused the command itself, raising error flags with
 * tc4x_dmu_error(); false lets the SoC run it.  Either way the SoC
 * updates the request status.
 */
typedef bool (*Tc4xDmuHookFn)(void *opaque, const Tc4xDmuCommand *cmd);
void tc4x_dmu_set_hook(Tc4xSoc *soc, Tc4xDmuHookFn fn, void *opaque);

/* Set bits of DMU_HCI_ERR */
void tc4x_dmu_error(Tc4xSoc *soc, uint32_t err);

/*
 * From a board's reset hook: core 0 starts at @pc instead of the image's
 * entry point, or stays halted (as before a debugger releases it through
 * BOOTCON), for this reset only
 */
void tc4x_set_boot_pc(Tc4xSoc *soc, uint32_t pc);
void tc4x_hold_boot(Tc4xSoc *soc);

/* Guest time: what the STM and the eGTM count, in ns (any thread) */
int64_t tc4x_guest_ns(Tc4xSoc *soc);

/* QEMU_CLOCK_VIRTUAL time at which guest time reaches @guest_ns */
int64_t tc4x_host_ns_for_guest(Tc4xSoc *soc, int64_t guest_ns);

/*
 * Guest time changed its rate or stood still: @fn re-arms timers that wait
 * for a guest time (with tc4x_host_ns_for_guest())
 */
typedef void (*Tc4xRetimeFn)(void *opaque);
void tc4x_time_add_retime_notifier(Tc4xSoc *soc, Tc4xRetimeFn fn,
                                   void *opaque);

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
