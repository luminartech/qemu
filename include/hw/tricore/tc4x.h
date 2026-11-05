/*
 * Services of the tc4x SoC (TYPE_TC4X_SOC) for board code: the code that
 * adds what sits around the SoC on one board.
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

#include "qom/object.h"
#include "hw/net/tc4x_geth.h"
#include "hw/tricore/tc4x_flash.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "hw/tricore/tc4x_timebase.h"

#define TYPE_TC4X_SOC "tc4x-soc"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xSoc, TC4X_SOC)

/* Stored value of the SFR at @addr (any thread) */
uint32_t tc4x_reg_get(Tc4xSoc *soc, uint32_t addr);

/* Store @val at @addr, no side effects (any thread) */
void tc4x_reg_set(Tc4xSoc *soc, uint32_t addr, uint32_t val);

/*
 * Set @bits of the SFR at @addr atomically, no side effects (any thread):
 * a guest write that clears other bits at the same time is not undone.
 * Not for service request nodes (tc4x_irq_raise() sets SRR).
 */
void tc4x_reg_or(Tc4xSoc *soc, uint32_t addr, uint32_t bits);

/* Hooks of the SFR store at @addr (see tc4x_sfr_store.h) */
void tc4x_sfr_add_write_hook(Tc4xSoc *soc, uint32_t addr, Tc4xSfrWriteFn fn,
                             void *opaque);
void tc4x_sfr_add_read_hook(Tc4xSoc *soc, uint32_t addr, Tc4xSfrReadFn fn,
                            void *opaque);

/*
 * From a read or write hook or the DMU hook: the core whose load or store
 * is being served, from the bus master in the attributes the SFR store
 * passes on (tc4x_master.h), or -1 when no core makes it (a DMA channel,
 * also one a core's store starts, the sim bridge as the debug master).
 * The sim bridge acting as a core ("master cpu N") counts as that core.
 * Outside a hook: the core of the current vCPU, or -1 (a timer).  A board
 * region's own callbacks take the master from their attrs argument
 * (tc4x_master_core()) instead, which also tells DMA and the sim bridge
 * apart from the core whose store started them.
 */
int tc4x_access_core(Tc4xSoc *soc);

/* Hardware-set SRR of the service request node at @src (any thread) */
void tc4x_irq_raise(Tc4xSoc *soc, uint32_t src);

/*
 * A slave on one chip select of a QSPI module.  BACONENTRY writes that
 * select it start a frame (@bacon: word length, bit order); every TX word
 * clocks one word in and its reply into the RX FIFO.  Called with the
 * module's bus lock held, from any thread: the ops must not take the BQL
 * (the contract of TYPE_TC4X_QSPI_PERIPHERAL, tc4x_qspi.h).
 */
typedef struct Tc4xQspiSlaveOps {
    void (*frame)(void *opaque, uint32_t bacon);
    uint32_t (*word)(void *opaque, uint32_t mosi);
} Tc4xQspiSlaveOps;

/*
 * Attach a slave at chip select @cs of QSPI @module (a peripheral on the
 * module's bus), replacing an attached slave there.  A chip select taken by
 * another device ends QEMU with an error.  Machine init only.
 */
void tc4x_qspi_attach(Tc4xSoc *soc, unsigned module, unsigned cs,
                      const Tc4xQspiSlaveOps *ops, void *opaque);

/*
 * Board code writes the receive data of QSPI @module straight to where its
 * receive DMA would put it: its hardware-request DMA lists do not run.
 */
void tc4x_qspi_set_rx_fed(Tc4xSoc *soc, unsigned module, bool fed);

/*
 * The bus locks of all QSPI modules, which their slaves run under, for
 * board code that shares their state (taken after the BQL)
 */
void tc4x_qspi_lock(Tc4xSoc *soc);
void tc4x_qspi_unlock(Tc4xSoc *soc);

/*
 * GETH0 (TYPE_TC4X_GETH): machine init sets its PHY and port backend links
 * before the SoC realizes
 */
DeviceState *tc4x_soc_geth(Tc4xSoc *soc);

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

/*
 * Attach a slave at 7-bit address @addr of I2C@bus (an I2C slave device on
 * the controller's bus), replacing an attached slave there.  An address
 * taken by another device ends QEMU with an error.  Machine init only.
 */
void tc4x_i2c_attach(Tc4xSoc *soc, unsigned bus, uint8_t addr,
                     const Tc4xI2cSlaveOps *ops, void *opaque);

/*
 * The flashes came from drives that hold contents of an earlier run
 * (board init on; see tc4x_flash.h)
 */
bool tc4x_flash_kept(Tc4xSoc *soc);

/*
 * Read, write or fill flash storage directly, without the DMU (see
 * tc4x_flash_storage_read() and the others)
 */
void tc4x_flash_read(Tc4xSoc *soc, Tc4xFlash flash, uint64_t off, void *buf,
                     uint64_t len);
void tc4x_flash_write(Tc4xSoc *soc, Tc4xFlash flash, uint64_t off,
                      const void *buf, uint64_t len);
void tc4x_flash_fill(Tc4xSoc *soc, Tc4xFlash flash, uint64_t off,
                     uint8_t value, uint64_t len);

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
void tc4x_pflash_set_maps(Tc4xSoc *soc, const Tc4xPflashMap *maps,
                          unsigned n);

/*
 * @fn sees every DMU command (Tc4xDmuCommand, tc4x_flash.h) before it
 * runs and returns true when it executed or refused the command itself,
 * raising error flags with tc4x_dmu_error(); false lets the flash run it.
 * Either way the flash updates the request status.  It and the locate
 * function act as the flash's policy (TYPE_TC4X_FLASH_POLICY).
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

/*
 * Start @core, still halted since the reset, at @pc: CPUcs (core 6), or
 * core 0 held by tc4x_hold_boot
 */
void tc4x_start_core(Tc4xSoc *soc, int core, uint32_t pc);

/* Start core 0, held by tc4x_hold_boot, at this reset's start address */
void tc4x_release_boot(Tc4xSoc *soc);

/* Raise the non-maskable interrupt (trap class 7, TIN 0) on @core */
void tc4x_nmi(Tc4xSoc *soc, int core);

/* Guest time: what the STM and the eGTM count, in ns (any thread) */
int64_t tc4x_guest_ns(Tc4xSoc *soc);

/* QEMU_CLOCK_VIRTUAL time at which guest time reaches @guest_ns */
int64_t tc4x_host_ns_for_guest(Tc4xSoc *soc, int64_t guest_ns);

/*
 * Guest time changed its rate or stood still: @fn re-arms timers that wait
 * for a guest time (with tc4x_host_ns_for_guest())
 */
void tc4x_time_add_retime_notifier(Tc4xSoc *soc, Tc4xRetimeFn fn,
                                   void *opaque);

/*
 * Sim-bridge and sfr-script commands whose name starts with @prefix,
 * offered after the objects that serve the word (TYPE_TC4X_SIM_COMMANDS,
 * tc4x_sim_bridge.h).  @fn returns false for a command it does not know,
 * else appends its reply lines (ending in "ok ..." or "err ...") to @out.
 * With @qspi_lock the command runs under the QSPI bus locks (it touches
 * QSPI slave state).  Board init only.
 */
typedef bool (*Tc4xSimCommandFn)(void *opaque, int argc, char **argv,
                                 GString *out);
void tc4x_sim_add_command(Tc4xSoc *soc, const char *prefix,
                          Tc4xSimCommandFn fn, void *opaque, bool qspi_lock);

#endif
