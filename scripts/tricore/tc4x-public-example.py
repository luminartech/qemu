#!/usr/bin/env python3
#
# Build an Infineon AURIX code example for the TC4x and run it on the tc4x
# machine, from public sources only:
#
#   - the example: https://github.com/Infineon/AURIX_code_examples (an
#     AURIX Development Studio project with its own iLLD, startup code and
#     GCC linker script), built from its .cproject settings without ADS;
#   - the compiler: the open-source TriCore GCC,
#     https://github.com/NoMore201/tricore-gcc-toolchain (TriCore 1.8);
#   - QEMU: qemu-system-tricore -M tc4x (generic board unless --board names
#     another, console on ASCLIN0).
#
# The run passes when every expected text appears on the console or in the
# trace output (--trace enables trace events, e.g. tc4x_kit_led for the LEDs
# of the kit-tc4d7-lite board).  --sim sends sim-bridge commands to the
# running machine, one every --sim-delay seconds.
#
#   scripts/tricore/tc4x-public-example.py --qemu build/qemu-system-tricore
#   scripts/tricore/tc4x-public-example.py --board kit-tc4d7-lite \
#       --example iLLD_TC4D7_LK_ADS_Blinky_LED --trace tc4x_kit_led \
#       --expect "led 1 on 1" --expect "led 1 on 0"
#
# Copyright (c) 2026 MicroVision, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import pathlib
import re
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import xml.etree.ElementTree as ET

EXAMPLES_REPO = "https://github.com/Infineon/AURIX_code_examples.git"
TOOLCHAIN_URL = ("https://github.com/NoMore201/tricore-gcc-toolchain/releases/"
                 "download/{tag}/tricore-gcc-{tag}-linux.tar.gz")
DEFAULT_EXAMPLE = "iLLD_TC4D7_LK_ADS_Printf"
DEFAULT_EXPECT = "Serial Printf"
# ADS names the TC4x GCC CPU option after the device family
MCPU = {"tc4dax": "tc4DAx"}


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def get_toolchain(work, tag):
    """bin directory of the TriCore GCC, downloaded once into work/"""
    root = work / f"tricore-gcc-{tag}"
    gcc = root / "bin" / "tricore-elf-gcc"
    if not gcc.exists():
        archive = work / f"tricore-gcc-{tag}-linux.tar.gz"
        if not archive.exists():
            url = TOOLCHAIN_URL.format(tag=tag)
            print(f"downloading {url}", flush=True)
            urllib.request.urlretrieve(url, archive)
        root.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as tar:
            try:
                tar.extractall(root, filter="tar")
            except TypeError:   # Python without extraction filters
                tar.extractall(root)
    return gcc.parent


def get_example(work, name, ref):
    """the example's directory, from a sparse checkout of the public repo"""
    repo = work / "AURIX_code_examples"
    if not repo.exists():
        run(["git", "clone", "--quiet", "--filter=blob:none", "--sparse",
             EXAMPLES_REPO, repo])
    run(["git", "-C", repo, "sparse-checkout", "add", f"code_examples/{name}"])
    run(["git", "-C", repo, "checkout", "--quiet", ref])
    example = repo / "code_examples" / name
    if not (example / ".cproject").exists():
        sys.exit(f"{name}: no such example in {EXAMPLES_REPO}")
    return example


def build(example, bindir, out):
    """Compile and link the example as its first .cproject configuration does"""
    cfg = ET.parse(example / ".cproject").getroot().find(".//configuration")
    incs, defs, mcpu, ldscript = [], [], "tc4dax", None
    for opt in cfg.iter("option"):
        kind = ((opt.get("superClass") or "") + (opt.get("id") or "")).lower()
        values = [v.get("value") or "" for v in opt.findall("listOptionValue")]
        if "include" in kind and "path" in kind:
            for v in values:
                m = re.search(r"\$\{ProjName\}/?([^}]*)\}", v)
                if m:
                    incs.append(example / m.group(1))
        elif "define" in kind or "symbols" in kind:
            defs += [v for v in values if v]
        elif kind.endswith("mcpu"):
            mcpu = (opt.get("value") or "").rsplit(".", 1)[-1]
        elif "linkerscript" in kind:
            ldscript = (opt.get("value") or "").removeprefix("../")
    excluded = [example / x for e in cfg.iter("entry")
                for x in (e.get("excluding") or "").split("|") if x]
    srcs = [p for p in sorted(example.rglob("*.c"))
            if not any(p == x or x in p.parents for x in excluded)]
    cpu = MCPU.get(mcpu.lower(), mcpu)
    gcc = bindir / "tricore-elf-gcc"
    cflags = [f"-mcpu={cpu}", "-O2", "-fno-common"]
    cflags += [f"-I{i}" for i in incs] + [f"-D{d}" for d in defs]
    out.mkdir(parents=True, exist_ok=True)
    print(f"compiling {len(srcs)} files for -mcpu={cpu}", flush=True)
    objs = []
    for src in srcs:
        obj = out / (str(src.relative_to(example)).replace("/", "_") + ".o")
        subprocess.run([gcc, *cflags, "-c", src, "-o", obj], check=True)
        objs.append(obj)
    elf = out / f"{example.name}.elf"
    run([gcc, f"-mcpu={cpu}", f"-T{example / ldscript}", "-Wl,--gc-sections",
         "-Wl,--no-warn-rwx-segments", *objs, "-o", elf])
    return elf


def run_qemu(qemu, elf, board, log, expects, traces, sims, sim_delay,
             timeout):
    """True once every text of expects appears within timeout seconds"""
    console = log.with_suffix(".console")
    trace = log.with_suffix(".trace")
    # a short directory: socket paths are limited to 107 bytes
    sim_dir = tempfile.TemporaryDirectory(prefix="tc4x-")
    sim_path = pathlib.Path(sim_dir.name) / "sim"
    for f in (console, trace):
        f.unlink(missing_ok=True)
    cmd = [qemu, "-M", f"tc4x,board={board}", "-kernel", elf,
           "-display", "none", "-monitor", "none",
           "-serial", f"file:{console}",
           "-serial", f"unix:{sim_path},server=on,wait=off"]
    for pattern in traces:
        cmd += ["-trace", pattern]
    with open(trace, "w") as err:
        proc = subprocess.Popen([str(c) for c in cmd], stderr=err)
    start = time.monotonic()
    deadline = start + timeout
    sim = None
    pending = list(sims)
    try:
        while time.monotonic() < deadline and proc.poll() is None:
            text = "".join(f.read_text(errors="replace")
                           for f in (console, trace) if f.exists())
            log.write_text(text)
            if all(e in text for e in expects):
                return True
            if pending and time.monotonic() - start >= sim_delay * (
                    len(sims) - len(pending) + 1):
                if sim is None:
                    sim = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sim.connect(str(sim_path))
                print(f"sim: {pending[0]}", flush=True)
                sim.sendall((pending.pop(0) + "\n").encode())
            time.sleep(0.2)
        return False
    finally:
        if sim is not None:
            sim.close()
        proc.terminate()
        proc.wait()
        sim_dir.cleanup()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="build/qemu-system-tricore",
                    help="qemu-system-tricore to run")
    ap.add_argument("--example", default=DEFAULT_EXAMPLE,
                    help="code_examples/ directory of the public repo")
    ap.add_argument("--example-ref", default="master",
                    help="commit or branch of the public repo")
    ap.add_argument("--board", default="generic",
                    help="tc4x board to run the example on")
    ap.add_argument("--expect", action="append",
                    help="text the console or trace output must show "
                         f"(repeatable, default {DEFAULT_EXPECT!r})")
    ap.add_argument("--trace", action="append", default=[],
                    help="trace event pattern to enable (repeatable)")
    ap.add_argument("--sim", action="append", default=[],
                    help="sim-bridge command to send (repeatable)")
    ap.add_argument("--sim-delay", type=float, default=2.0,
                    help="seconds before each --sim command")
    ap.add_argument("--toolchain", type=pathlib.Path,
                    help="bin/ of a TriCore GCC (default: download one)")
    ap.add_argument("--toolchain-tag", default="13.4.1",
                    help="tricore-gcc-toolchain release to download")
    ap.add_argument("--work", type=pathlib.Path,
                    default=pathlib.Path("tc4x-public-example"),
                    help="directory for downloads and the build")
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="seconds to wait for the expected text")
    args = ap.parse_args()

    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    bindir = args.toolchain or get_toolchain(work, args.toolchain_tag)
    example = get_example(work, args.example, args.example_ref)
    elf = build(example, bindir.resolve(), work / "build" / args.example)
    expects = args.expect or [DEFAULT_EXPECT]
    log = work / f"{args.example}.log"
    ok = run_qemu(pathlib.Path(args.qemu).resolve(), elf, args.board, log,
                  expects, args.trace, args.sim, args.sim_delay, args.timeout)
    text = log.read_text(errors="replace") if log.exists() else ""
    print(text)
    missing = [e for e in expects if e not in text]
    print(f"{args.example} on {args.board}: {'PASS' if ok else 'FAIL'}"
          + (f" (not seen: {missing})" if missing else ""))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
