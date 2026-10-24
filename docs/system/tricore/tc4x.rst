.. SPDX-License-Identifier: GPL-2.0-or-later

Infineon AURIX TC4Dx (``tc4x``)
===============================

The ``tc4x`` machine models an Infineon AURIX TC4Dx: six TriCore 1.8
cores (CPU type ``tc4x``) running in parallel under MTTCG, their local
scratchpad RAMs, the LMU, program and data flash, and the on-chip
peripherals listed below.  It loads TC4Dx images from Intel HEX or ELF
files.

Quick start
-----------

Build QEMU for TriCore (QEMU needs Python 3.11 or newer, or Python 3.9
with ``tomli``):

.. code-block:: console

  $ mkdir build && cd build
  $ ../configure --target-list=tricore-softmmu && make -j
  $ ./qemu-system-tricore -M tc4x,board=help

Get a compiler and a program.  The open-source TriCore GCC
(https://github.com/NoMore201/tricore-gcc-toolchain, ``-mcpu=tc4DAx``)
builds Infineon's public code examples
(https://github.com/Infineon/AURIX_code_examples) unchanged;
``scripts/tricore/tc4x-public-example.py`` fetches both, builds an example
as its AURIX Development Studio project does and runs it (see `Testing with
public examples`_).  To run an ELF or Intel HEX image of your own:

.. code-block:: console

  $ ./qemu-system-tricore -M tc4x,board=kit-tc4d7-lite -display none \
        -serial stdio -kernel blinky.elf -trace tc4x_kit_led

The console (ASCLIN0, the kit's USB debug UART) is on standard input and
output; ``-trace tc4x_kit_led`` prints every LED change.  Add
``-serial unix:/tmp/sim.sock,server=on,wait=off`` to drive the board
through the `Sim bridge`_, for example ``button press``.

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

``flash-dir=DIR``
  Keep the program and data flash in files ``DIR/pflash`` and
  ``DIR/dflash`` across runs: files that exist keep their contents and
  the image only seeds them, so a restart after a guest reset finds what
  the firmware programmed.

``imprecise-io=on``
  SFR accesses take effect at the next TB instead of the next instruction
  (no TCG I/O rewind).  Faster for firmware that polls registers in tight
  loops, but no longer instruction-precise; off by default, and icount
  keeps the precise behaviour.

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
SFR write hooks, analog inputs, interrupt pacers, sim-bridge commands, and
the flashes: their storage, where DMU commands find an address, how
the PFLASH read windows map the storage, and a hook that sees every DMU
command before it runs.

Boards kept outside this tree are built in from ``hw/tricore/boards/``:
see ``hw/tricore/boards/README.rst``.

``generic``
  The SoC alone: console on ASCLIN0, no inputs and no PHY.

``kit-tc4d7-lite``
  Infineon's AURIX TC4D7 lite kit (KIT_A3G_TC4D7_LITE), wired as the
  board header of the TC4D7 code examples (``Boards/KIT_TC4D7_LITE/pin.h``)
  describes:

  * user LEDs on P03.9 and P03.10, lit while the pin is low; trace event
    ``tc4x_kit_led`` reports every change (``led N on 0|1``)
  * user button on P03.11, released at start; sim-bridge command
    ``button press|release``
  * debug UART on ASCLIN0, the console
  * potentiometer on TMADC0 channel 0, at 1.65 V; sim-bridge command
    ``adc 0 0 VOLTS`` turns it
  * 2-Kbit EEPROM at I2C0 address 0x50 with an EUI-48 node address
    (52:54:00:12:34:56) at 0xFA
  * DP83825I Ethernet PHY at MDIO address 0 of GETH0 port 0 with the link
    up at 100 Mbit/s full duplex; sim-bridge command ``link up|down``.
    A ``-nic`` with ``model=tc4x-geth`` connects port 0, for example
    ``-nic socket,model=tc4x-geth,udp=127.0.0.1:5556,localaddr=127.0.0.1:5555``
    exchanges frames as UDP datagrams with a host process (the
    ``ETH_Demo`` example echoes every frame it receives).

Testing with public examples
----------------------------

``scripts/tricore/tc4x-public-example.py`` builds an Infineon AURIX code
example for the TC4x and runs it, from public sources only:

- the example comes from https://github.com/Infineon/AURIX_code_examples.
  It is an AURIX Development Studio project with its own iLLD, start-up
  code and GCC linker script; the script compiles it as its ``.cproject``
  configuration does, without the IDE;
- the compiler is the open-source TriCore GCC,
  https://github.com/NoMore201/tricore-gcc-toolchain (TriCore 1.8,
  ``-mcpu=tc4DAx``).  The script downloads a release unless
  ``--toolchain`` names the ``bin`` directory of one.  The prebuilt Linux
  release needs glibc 2.38 or newer; on older hosts build the toolchain
  from source (``docs/building.md`` of that repository).

The run passes when every ``--expect`` text appears on the console or in
the output of the trace events that ``--trace`` enables; ``--sim`` sends
sim-bridge commands while the example runs, one every ``--sim-delay``
seconds.  Without options it runs ``iLLD_TC4D7_LK_ADS_Printf`` on the
``generic`` board:

.. code-block:: console

  $ scripts/tricore/tc4x-public-example.py --qemu build/qemu-system-tricore
  ...
  iLLD_TC4D7_LK_ADS_Printf on generic: PASS

These TC4D7 examples pass, unchanged (all but Printf with
``--board kit-tc4d7-lite``):

``iLLD_TC4D7_LK_ADS_Printf``
  (no options)

``iLLD_TC4D7_LK_ADS_Blinky_LED``, ``iLLD_TC4D7_LK_ADS_STM_Interrupt``, ``iLLD_TC4D7_LK_ADS_Multicore``
  ``--trace tc4x_kit_led --expect "led 1 on 1"``

``iLLD_TC4D7_LK_ADS_GPIO_LED_Button``
  ``--trace tc4x_kit_led --sim "button press" --expect "led 1 on 1"``

``iLLD_TC4D7_LK_ADS_I2C_Read_Ext_Device``
  ``--trace tc4x_i2c_transfer --expect "addr 0x50 read 1 len 6 ack 1"``

``iLLD_TC4D7_lite_kit_ADS_ETH_Demo``
  ``--expect "GETH0_P0 link is up"``

Limitations
-----------

* Peripherals not listed under `Supported devices`_ are plain registers:
  writes are stored, nothing happens.  CAN, the eGTM beyond TOM and the
  TBU, the SMU, the MTU and the security core (CSRM) are not modelled.
* Timing: instructions take no modelled cycles; guest time follows QEMU
  virtual time (see `Guest time`_).
* Double-precision FPU corner cases differ from the TC1.8 architecture
  manual: subnormal operands and results are flushed to zero, invalid
  operations return a single quiet NaN, and DFTOF/FTODF do not keep NaN
  payloads.  DFTOIN, MAX.DF, MIN.DF and QSEED.DF raise an illegal opcode
  trap.

Sources
-------

Every TC4x detail the machine models is taken from public Infineon
material:

* the iLLD SFR headers (register addresses, bit fields and access types)
  and the iLLD drivers bundled with the TC4D7 projects of
  https://github.com/Infineon/AURIX_code_examples, under the Boost
  Software License 1.0;
* the TriCore TC1.8 architecture manual, volumes 1 and 2 (V1.0.0,
  2024-02-14), on infineon.com;
* the AURIX TC4xx documentation at https://documentation.infineon.com/aurixtc4xx;
* the TriCore 1.8 instruction encodings of the GNU binutils port at
  https://github.com/NoMore201/tricore-binutils-gdb.

Where the model completes a status at once (PLL lock, calibration, kernel
reset), the source names the iLLD code that waits for it.
