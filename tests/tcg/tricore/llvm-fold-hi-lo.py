#!/usr/bin/env python3
#
# Rewrite GNU tricore-as relocation operators for the LLVM assembler:
# lo:/up: on numeric constants become plain 16-bit immediates (LLVM only
# accepts them on symbols).  GNU up:sym is the plain upper half (paired
# with a zero-extended lo:), LLVM hi: the one adjusted for a sign-extended
# lo:, so up:sym becomes hi:(sym-0x8000).
#
# Copyright (c) 2026 MicroVision, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import re
import sys


def fold(m):
    v = int(m.group(2), 0) & 0xFFFFFFFF
    return str(v & 0xFFFF if m.group(1) == "lo" else v >> 16)


src = open(sys.argv[1]).read()
src = re.sub(r"\b(lo|up):((?:0x[0-9a-fA-F]+)|\d+)\b", fold, src)
src = re.sub(r"\bup:([A-Za-z_.$][\w.$]*)", r"hi:(\1-0x8000)", src)
open(sys.argv[2], "w").write(src)
