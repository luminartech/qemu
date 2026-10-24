/*
 * Boards of the tc4x machine, selected with -M tc4x,board=NAME (default
 * "generic", board=help lists them).  A board is a QOM type derived from
 * TYPE_TC4X_BOARD; board code kept outside the tree is built in from
 * hw/tricore/boards/ (see the README there).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_BOARD_H
#define HW_TRICORE_TC4X_BOARD_H

#include "qom/object.h"
#include "hw/tricore/tc4x.h"

#define TYPE_TC4X_BOARD "tc4x-board"
#define TC4X_BOARD_TYPE_NAME(name) (name "-" TYPE_TC4X_BOARD)

OBJECT_DECLARE_TYPE(Tc4xBoard, Tc4xBoardClass, TC4X_BOARD)

/* A GPIO input the board holds at a fixed level (strap, pull-up) */
typedef struct Tc4xBoardInput {
    uint8_t port;
    uint8_t pin;
    uint8_t level;
} Tc4xBoardInput;

/*
 * Software write semantics of one register: bits that store what is
 * written, bits a written 1 clears, a written 0 clears, a written 1 sets.
 * Registers without an entry store every write as it is.
 */
typedef struct Tc4xRegSemantics {
    uint32_t addr;
    uint32_t persist;
    uint32_t w1c;
    uint32_t w0c;
    uint32_t w1s;
} Tc4xRegSemantics;

/*
 * A module's kernel reset registers: writing CTRLB.KRST completes the reset
 * at once (STAT.KRST set, the KRST request bits of CTRLA and CTRLB clear);
 * CTRLB.STATCLR clears STAT.KRST
 */
typedef struct Tc4xKernelReset {
    uint32_t ctrla;
    uint32_t ctrlb;
    uint32_t stat;
} Tc4xKernelReset;

/* The SoC's tables, generated from the iLLD SFR headers (tc4x_sfr_tables.c) */
extern const Tc4xRegSemantics tc4x_reg_semantics[];
extern const size_t tc4x_n_reg_semantics;
extern const Tc4xKernelReset tc4x_kernel_resets[];
extern const size_t tc4x_n_kernel_resets;

struct Tc4xBoard {
    Object parent_obj;
};

struct Tc4xBoardClass {
    ObjectClass parent_class;

    const char *desc;
    unsigned console;           /* ASCLIN instance on serial0 */
    const Tc4xBoardInput *inputs;
    unsigned n_inputs;
    /* added to (and replacing entries of) the SoC's own tables */
    const Tc4xRegSemantics *reg_semantics;
    size_t n_reg_semantics;
    const Tc4xKernelReset *kernel_resets;
    size_t n_kernel_resets;
    /* Adds the board code, at the end of machine init before the sfr-script */
    void (*init)(Tc4xBoard *board, Tc4xSoc *soc);
    /*
     * At every machine reset, once the flashes hold their contents and
     * before core 0 starts: may choose where and whether it starts
     */
    void (*reset)(Tc4xBoard *board, Tc4xSoc *soc);
};

#endif
