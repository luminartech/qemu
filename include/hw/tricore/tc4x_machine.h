/*
 * The tc4x machines: the tc4x SoC and the board around it.  A machine
 * type derived from TYPE_TC4X_MACHINE names the board it is built around
 * in its class; the "tc4x" machine takes any board with board=NAME.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_MACHINE_H
#define HW_TRICORE_TC4X_MACHINE_H

#include "hw/core/boards.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_board.h"
#include "hw/tricore/tc4x_sim_bridge.h"

#define TYPE_TC4X_MACHINE "tc4x-common-machine"
OBJECT_DECLARE_TYPE(Tc4xMachineState, Tc4xMachineClass, TC4X_MACHINE)

struct Tc4xMachineClass {
    MachineClass parent_class;

    /* the board: NAME of its type TC4X_BOARD_TYPE_NAME(NAME) */
    const char *board;
};

struct Tc4xMachineState {
    MachineState parent_obj;

    Tc4xSoc *soc;
    Tc4xBoard *board;
    Tc4xSimBridge *sim_bridge;
    uint32_t entry;         /* entry point of the first image */
    char *sfr_script;
    char *sim_chardev;
    char *board_name;       /* board= of the tc4x machine */
    char *shm_dir;
    char *flash_dir;
    bool imprecise_io;
};

#endif
