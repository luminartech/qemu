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

struct Tc4xBoard {
    Object parent_obj;
};

struct Tc4xBoardClass {
    ObjectClass parent_class;

    const char *desc;
    unsigned console;           /* ASCLIN instance on serial0 */
    const Tc4xBoardInput *inputs;
    unsigned n_inputs;
    /* Adds the board code, at the end of machine init before the sfr-script */
    void (*init)(Tc4xBoard *board, Tc4xSoc *soc);
};

#endif
