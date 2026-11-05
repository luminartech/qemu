/*
 * Boards of the tc4x machines, selected with -M tc4x,board=NAME (default
 * "generic", board=help lists them) or by a machine type derived from
 * TYPE_TC4X_MACHINE that names one.  A board is a QOM type derived from
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
#include "hw/tricore/tc4x_sfr_store.h"

#define TYPE_TC4X_BOARD "tc4x-board"
#define TC4X_BOARD_TYPE_NAME(name) (name "-" TYPE_TC4X_BOARD)

OBJECT_DECLARE_TYPE(Tc4xBoard, Tc4xBoardClass, TC4X_BOARD)

/* A GPIO input the board holds at a fixed level (strap, pull-up) */
typedef struct Tc4xBoardInput {
    uint8_t port;
    uint8_t pin;
    uint8_t level;
} Tc4xBoardInput;

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
    /*
     * Before the SoC realizes: creates the board devices that SoC devices
     * link to and sets those links (tc4x_soc_geth())
     */
    void (*wire)(Tc4xBoard *board, Tc4xSoc *soc);
    /* Adds the board code, at the end of machine init before the sfr-script */
    void (*init)(Tc4xBoard *board, Tc4xSoc *soc);
    /*
     * At every machine reset, once the flashes hold their contents and
     * before core 0 starts: may choose where and whether it starts
     */
    void (*reset)(Tc4xBoard *board, Tc4xSoc *soc);
};

#endif
