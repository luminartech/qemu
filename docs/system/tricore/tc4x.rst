.. SPDX-License-Identifier: GPL-2.0-or-later

Infineon AURIX TC4Dx (``tc4x``)
===============================

The ``tc4x`` machine models an Infineon AURIX TC4Dx: six TriCore 1.8
cores (CPU type ``tc4x``) running in parallel under MTTCG, their local
scratchpad RAMs, the LMU, program and data flash, and the on-chip
peripherals listed below.  It loads TC4Dx images from Intel HEX or ELF
files.

Supported devices
-----------------

* CPUs: boot of core 0 at the image entry point, the other cores released
  through ``BOOTCON.BHALT`` / ``HRA_PC``; per-core STM with compares per
  virtual machine
* Interrupt router: SRC registers, per-core arbitration, interrupt delivery
  and acknowledge
* DMA0 and DMA1: linked lists, software and QSPI hardware requests
* QSPI0..7: full-duplex FIFOs, end of job through phase transition 2 or the
  move counter; slaves are attached by board code
* GETH0 (``hw/net/tc4x_geth.c``), based on the Synopsys DWC Ethernet QoS
  core, with its two MAC ports.  No PHY is attached by default, so MDIO
  reads return all ones; board code attaches PHYs through
  ``tc4x_geth_attach_phy()``.
* ASCLIN0..27: transmit at once and raise their service requests.  One
  instance is the console on the first serial port: ASCLIN0 unless the
  board names another.
* DMU flash command interface, eGTM TOM counters and TBU, TMADC conversions
  of pin voltages, I2C0..2 with slaves attached by board code, GPIO ports
* Start-up status that completes at once: the TMADC power-on calibration
  phase (``MODSTAT.CALPH``), the clock ramp oscillator and the VMT memory
  tests

Every other SFR is plain storage with the TC4x write semantics: bits that
clear when written with 1 or 0 and bits that set when written with 1 act
so, write-only bits read as 0, other bits read what was written, and
writing a module's
``RST_CTRLB.KRST`` completes its kernel reset at once.  The tables
(``hw/tricore/tc4x_sfr_tables.c``) are generated from the access types of
the iLLD SFR headers with ``scripts/tricore/tc4x-sfr-tables.py``; a board
can add to them.

Running
-------

.. code-block:: bash

  qemu-system-tricore -M tc4x -display none \
      -serial stdio -serial unix:/tmp/sim.sock,server=on,wait=off \
      -kernel image.hex

Machine properties:

``board=NAME``
  Board around the SoC (default ``generic``); ``board=help`` lists the
  boards built in.

``sfr-script=FILE``
  File of sim-bridge commands run at machine init (after the board).

``shm-dir=DIR``
  Back every RAM with a shared file ``DIR/NAME`` that external processes
  can map.

CPU property, with ``-global tc4x-tricore-cpu.csa-cache=on``:

``csa-cache=on``
  Remember the RAM pages of the context save areas across TBs instead of
  probing them on every call and return.  Faster, but stores through them
  skip dirty tracking and watchpoints set later, and the RAM must not be
  remapped; off by default.

Sim bridge
----------

The second serial port is the sim bridge, a line-oriented command interface
for external processes and tests.  Commands take numbers in C syntax and reply with a line
starting with ``ok`` or ``err``:

=============================  ==============================================
``r ADDR [SIZE]``              read through the bus, ``ok VALUE``
``w ADDR VAL [SIZE]``          write through the bus (side effects run)
``poke ADDR VAL``              set an SFR's stored value, no side effects
``force ADDR VAL [MASK]``      SFR reads return VAL on MASK until ``unforce``
``irq SRC``                    hardware-set SRR of a service request node
``watch ADDR``                 report guest writes as ``ev w ADDR VAL SIZE CPU``
``unwatch ADDR``               stop reporting them
``cores``                      per-core PC, state, ICR and some registers
``hot [MIN]``                  SFRs read at least MIN times since last asked
``timescale N``                request guest time at 1/N of virtual time
``vmstop``, ``vmcont``         pause and resume the guest
``adc K CH VOLTS``             set a TMADC pin voltage
``gpio PORT PIN 0|1|release``  drive or release an input
=============================  ==============================================

Asynchronous events: ``ev w ...`` for watched writes.  Board code adds its
own commands with ``tc4x_sim_add_command()``.

Guest time
----------

The STM and the eGTM count guest time, which runs at 1/N of QEMU virtual
time.  N is the largest request of its clients: the sim bridge
(``timescale``) and interrupt pacers.  Slowing guest time down gives
external processes, and emulated cores that are slower than silicon, more
wall time per guest millisecond.

A pacer (``tc4x_pacer.c``) serves board code that interrupts one core at a
fixed period of guest time.  It raises the period's interrupts on an exact grid of guest time, stops guest
time at the next slot until the main loop raised it, and holds guest time
(at a large scale) while a raised interrupt is still pending, so that no
two periods merge into one interrupt.  Its logic is covered by
``tests/unit/test-tc4x-pacer.c``.

Hot paths without the BQL
-------------------------

The SFR regions are ``lockless_io``: register reads without side effects,
the STM and TBU counters, GPIO pin writes, TOM shadow register writes and
QSPI register writes are served on the vCPU thread without taking the BQL.
Other accesses with side effects take the BQL.  QSPI writes run the
attached slaves and raise service requests under the QSPI lock, so slaves
must not take the BQL.

Boards
------

A board is a QOM type derived from ``tc4x-board`` (see
``include/hw/tricore/tc4x_board.h``).  Its class states what the SoC
applies itself: the console ASCLIN instance, GPIO inputs held by straps or
pull-ups, and register write semantics or kernel reset registers to add to
the SoC's own.  An ``init``
hook adds the board code through the services of
``include/hw/tricore/tc4x.h``: QSPI slaves, Ethernet PHYs, GPIO listeners,
SFR write hooks, analog inputs, interrupt pacers and sim-bridge commands.

The ``generic`` board has its console on ASCLIN0, no inputs and no PHY.
Boards kept outside this tree are built in from ``hw/tricore/boards/``:
see ``hw/tricore/boards/README.rst``.
