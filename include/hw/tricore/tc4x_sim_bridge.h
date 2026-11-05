/*
 * The sim bridge of the tc4x machines: a line-oriented command interface
 * on a character device for external processes and tests (the commands
 * are listed in docs/system/tricore/tc4x.rst).  Every command line is
 * answered by zero or more data lines and one line that starts with "ok"
 * or "err"; event lines, which start with "ev ", may come at any time.
 *
 * The bridge serves the bus and QOM commands itself (r, w, master, get,
 * set, vmstop, vmcont, ack).  Any other command word is served by an
 * object of the QOM tree that implements TYPE_TC4X_SIM_COMMANDS and lists
 * the word.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_SIM_BRIDGE_H
#define HW_TRICORE_TC4X_SIM_BRIDGE_H

#include "qom/object.h"

/*
 * The bridge: property "chardev" (may be unset: only script commands),
 * link "soc".  One per machine, a child of the machine named "sim-bridge".
 */
#define TYPE_TC4X_SIM_BRIDGE "tc4x-sim-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xSimBridge, TC4X_SIM_BRIDGE)

/*
 * An object that serves sim-bridge commands.  The bridge finds it in the
 * QOM tree.
 */
#define TYPE_TC4X_SIM_COMMANDS "tc4x-sim-commands"

typedef struct Tc4xSimCommandsClass Tc4xSimCommandsClass;
DECLARE_CLASS_CHECKERS(Tc4xSimCommandsClass, TC4X_SIM_COMMANDS,
                       TYPE_TC4X_SIM_COMMANDS)
#define TC4X_SIM_COMMANDS(obj) \
    INTERFACE_CHECK(Tc4xSimCommands, (obj), TYPE_TC4X_SIM_COMMANDS)

typedef struct Tc4xSimCommands Tc4xSimCommands;

struct Tc4xSimCommandsClass {
    InterfaceClass parent_class;

    /* The command words served, NULL-terminated (may be NULL: none) */
    const char *const *commands;
    /*
     * Run the command @argv, whose argv[0] is one of the words, in the
     * main loop with the BQL held: append the reply lines, the last one
     * starting with "ok" or "err", to @out.  Return false, with nothing
     * appended, to leave the line to the next object that serves the word.
     */
    bool (*run)(Tc4xSimCommands *obj, int argc, char **argv, GString *out);
};

/*
 * Numbers of command arguments, in C syntax (decimal, 0x hexadecimal,
 * 0 octal): false for anything else, a trailing character or a value out
 * of range included
 */
bool tc4x_sim_parse_uint(const char *arg, uint64_t max, uint64_t *val);
bool tc4x_sim_parse_int(const char *arg, int64_t min, int64_t max,
                        int64_t *val);
/* A finite floating-point number */
bool tc4x_sim_parse_double(const char *arg, double *val);

/*
 * Send the event line "ev " @fmt to the client of the sim bridge, if any.
 * Called with the BQL held, from any thread.
 */
void G_GNUC_PRINTF(1, 2) tc4x_sim_event(const char *fmt, ...);

/*
 * Run one command line (empty lines and lines starting with '#' do
 * nothing) and append its reply to @out; false when the reply is "err".
 * Main loop, BQL held.
 */
bool tc4x_sim_bridge_exec(Tc4xSimBridge *b, const char *line, GString *out);

/* Run every line of the file @path; an "err" reply is an error */
bool tc4x_sim_bridge_run_script(Tc4xSimBridge *b, const char *path,
                                Error **errp);

#endif
