.. SPDX-License-Identifier: GPL-2.0-or-later

Infineon AURIX TC4Dx (``tc4x``)
===============================

The ``tc4x`` machine models an Infineon AURIX TC4Dx: seven TriCore 1.8
cores (CPU type ``tc4x``) running in parallel under MTTCG, their local
scratchpad RAMs, the LMU, program and data flash, and the on-chip
peripherals listed below.  It loads TC4Dx images from Intel HEX or ELF
files.  The ``kit-tc4d7-lite`` machine is the same SoC on Infineon's TC4D7
lite kit, the ``tc4x`` machine with ``board=kit-tc4d7-lite``.

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

  $ ./qemu-system-tricore -M kit-tc4d7-lite -display none \
        -serial stdio -kernel blinky.elf -trace tc4x_kit_led

The console (ASCLIN0, the kit's USB debug UART) is on standard input and
output; ``-trace tc4x_kit_led`` prints every LED change.  Add
``-serial unix:/tmp/sim.sock,server=on,wait=off`` to drive the board
through the `Sim bridge`_, for example ``button press``.

Supported devices
-----------------

* CPUs: boot of core 0 at the image entry point, cores 1-5 released
  through ``BOOTCON.BHALT`` / ``HRA_PC``; CPUcs (core 6, its scratchpads in
  segment 1) stays halted unless a board starts it; per-core STM with
  compares per virtual machine; the non-maskable interrupt (trap class 7)
  when board code raises it; a device's error response to a fetch, load or
  store as a bus error trap (class 4: PSE, DSE, DAE); the TC1.8 range-based
  memory protection (``CORECON.PROTEN``, class 1: MPR, MPW, MPX)
* Interrupt router: SRC registers, per-core arbitration, interrupt delivery
  and acknowledge
* DMA0 and DMA1: linked lists, software and QSPI hardware requests
* QSPI0..7: full-duplex FIFOs, end of job through phase transition 2 or the
  move counter; slaves are attached by board code
* GETH0 (``hw/net/tc4x_geth.c``), based on the Synopsys DWC Ethernet QoS
  core, with its two MAC ports.  Board code links a PHY to a port through
  the controller's ``phy0``/``phy1`` links (``TYPE_TC4X_GETH_PHY``, MDIO
  address in ``phy0-addr``/``phy1-addr``); MDIO reads of a port without
  one return all ones.  The PHY sees the link state of the port's network
  connection (QMP ``set_link``).  The bridge forwards frames from a receive
  queue of one port to a transmit queue of the other when the TxQ/RxC
  mapping makes that queue a forwarding path (``BRIDGE_TXQ_MAP_PORT_n``,
  ``BRIDGE_PORT_n_CONTROL``); its packet length limit, queue flushes and
  same-port loopback are not modelled.
* ASCLIN0..27: transmit at once and raise their service requests.  One
  instance is the console on the first serial port: ASCLIN0 unless the
  board names another.
* PFLASH and DFLASH (see `Flash`_), DMU flash command interface, eGTM
  TOM counters and TBU, TMADC conversions
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
  boards built in.  ``tc4x`` machine only: the other machines name their
  board.

``sfr-script=FILE``
  File of sim-bridge commands run at machine init (after the board); a
  command that fails stops QEMU.

``sim-chardev=ID``
  Character device of the sim bridge (default: the second serial port).

``shm-dir=DIR``
  Back every RAM with a shared file ``DIR/NAME`` that external processes
  can map.  The core and LMU RAMs are ``memory-backend-file`` objects
  named after the RAMs.

``flash-dir=DIR``
  Keep the program and data flash in the raw files ``DIR/pflash`` and
  ``DIR/dflash`` across runs, as their drives (see `Flash`_).  Missing
  files are created erased.

``imprecise-io=on``
  SFR accesses take effect at the next TB instead of the next instruction
  (no TCG I/O rewind).  Faster for firmware that polls registers in tight
  loops, but no longer instruction-precise; off by default, and icount
  keeps the precise behaviour.

``deterministic=on``
  Runs repeat exactly (see `Deterministic mode`_).  Needs
  ``-icount shift=N,sleep=off``, ``-accel tcg,thread=single`` and
  ``-seed``; QEMU refuses to start without them.

``time-scale=N``
  With ``deterministic=on``: guest time runs at 1/N of QEMU virtual time,
  fixed (default 1).

The SoC is the device ``tc4x-soc``, the machine's child ``soc``.  Its
properties ``lmu-memdev`` and ``core0-memdev`` .. ``core6-memdev`` take
the LMU (64 MiB) and the scratchpad RAMs of a core (2 MiB) from a memory
backend, for example
``-object memory-backend-file,id=lmu,size=64M,mem-path=/dev/shm/lmu,share=on
-global tc4x-soc.lmu-memdev=lmu``; shm-dir cannot be combined with them.

CPU property, with ``-global tc4x-tricore-cpu.csa-cache=on``:

``csa-cache=on``
  Remember the RAM pages of the context save areas across TBs instead of
  probing them on every call and return.  Faster, but stores through them
  skip dirty tracking and watchpoints set later, and the RAM must not be
  remapped; off by default.

Sim bridge
----------

The sim bridge (device ``tc4x-sim-bridge``, the machine's child
``sim-bridge``) is a line-oriented command interface for external
processes and tests, on the character device that the machine property
``sim-chardev`` names (``-chardev socket,id=sim,path=sim.sock,server=on,wait=off
-M kit-tc4d7-lite,sim-chardev=sim``), else on the second serial port.
Commands take numbers in C syntax (decimal, ``0x`` hexadecimal, ``0``
octal); every command is answered by zero or more data lines and a line
that starts with ``ok`` or ``err``.  A malformed number, an argument too
many or a value out of range is an ``err`` reply, never a default.

The bridge serves:

=========================================  ====================================
``r ADDR [SIZE]``                          read through the bus, ``ok VALUE``;
                                           a read the bus refuses is ``err
                                           VALUE bus error at ADDR``
``w ADDR VAL [SIZE]``                      write through the bus (side effects
                                           run); as a debugger, it patches
                                           flash
``master [debug|cpu N [safe]|dma CH]``     the bus master of ``r`` and ``w``
                                           (default: the debug master)
``get PATH PROP``                          a QOM property, ``ok VALUE``
``set PATH PROP VALUE``                    set a QOM property from its text
                                           form
``vmstop``, ``vmcont``                     pause and resume the guest
``ack ID``                                 the core waiting since the event
                                           ``... sync ID`` goes on
=========================================  ====================================

The devices serve:

=============================  ==============================================
``poke ADDR VAL``              set an SFR's stored value, no side effects
``force ADDR VAL [MASK]``      SFR reads return VAL on MASK until ``unforce``
``irq SRC``                    hardware-set SRR of a service request node
``watch ADDR [sync]``          report guest writes as ``ev w ADDR VAL SIZE CPU``
``unwatch ADDR``               stop reporting them
``cores``                      per-core PC, state, ICR and some registers
``hot [MIN]``                  SFRs read at least MIN times since last asked
``timescale N``                request guest time at 1/N of virtual time
                               (refused with ``deterministic=on``)
``adc K CH VOLTS``             set a TMADC pin voltage
``gpio PORT PIN 0|1|release``  drive or release an input
``button press|release``       the kit's user button
``link up|down``               the kit's Ethernet cable
=============================  ==============================================

``poke``, ``force`` and ``unforce`` act on registers of the SFR store only;
for a register a device serves, the reply is ``err ADDR owned by PATH``.

Event lines start with ``ev`` and may come at any time, before a reply
included: ``ev w ...`` for watched writes, sent once the write took
effect (CPU -1: not a core).

``watch ADDR sync`` makes the watch synchronous: a core that writes the
register sends ``ev w ADDR VAL SIZE CPU sync ID`` and waits until the
client sends ``ack ID``, so the guest sees the client's answer (``poke``,
``irq``) before its next instruction.  A stop of the VM (``vmstop``, a
reset, quit) or the client's disconnection lets the core go on without
the ack; a write that no core makes (``w``, a timer) sends the event
without ``sync``.  The SFR store and the interrupt router report writes;
for a register another device owns, ``watch`` is an error.

A device, a board or any other object adds commands by implementing the
QOM interface ``tc4x-sim-commands`` (``include/hw/tricore/tc4x_sim_bridge.h``):
the class lists its command words and runs them; the bridge finds the
object in the QOM tree.  An object that owns registers implements the
interface's ``watch`` method and reports watched writes with
``tc4x_sim_watch_event()``; ``tc4x_sim_event()`` sends any event line.  The
``sfr-script`` machine property runs a file of commands at machine init;
a command that fails stops QEMU with the line and its reply.

Flash
-----

The PFLASH (80 MiB, read windows at 0x80000000 and 0xA0000000) and the
DFLASH (32 MiB at 0xAE000000) are the device ``tc4x-flash``, the SoC's
child ``flash``.  Guest loads and instruction fetches read them at RAM
speed; a guest store into them is a guest error with a bus error
response, since only the DMU programs flash.  DMU commands, the sim
bridge ``w`` and board code change the contents, and translated code of
the changed bytes is discarded.

The SoC properties ``pflash`` and ``dflash`` take drives that hold the
contents across runs, written through on every change: ``-drive
if=pflash,index=0,format=raw,file=pflash.bin`` (80 MiB) and ``index=1``
(32 MiB), or ``flash-dir``.  When a drive holds anything but erased
cells (zeros), the flashes are kept and ``-kernel`` images seed only the
RAMs; otherwise the images seed the flashes at the first reset.  A
machine reset leaves the flash contents as they are.

The DMU command interfaces of the host and of the CSRM program, erase
and verify the flashes.  A command whose address is not aligned to its
page, wordline or logical sector, or whose range has no flash, fails
with a sequence error (SQER).

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

Deterministic mode
------------------

With ``deterministic=on`` the guest-visible results of a run depend only on
the command line, the images and the inputs:

- Guest time is QEMU virtual time divided by the fixed ``time-scale``, and
  with ``-icount shift=N,sleep=off`` virtual time is the instruction count
  times 2^N ns (idle CPUs skip ahead to the next timer).  One TCG thread
  runs the CPUs in turn, so 7 busy cores share the count.
- Requests for another scale (``timescale``) are refused or ignored, and
  pacers only raise their interrupts on the grid: they neither stop nor
  slow guest time, and a period the core has not taken merges with the
  next, as on silicon.
- Device timers are QEMU_CLOCK_VIRTUAL timers, which -icount runs at a
  fixed instruction count; work that the default mode hands to the main
  loop (DMA jobs started without the BQL, interrupt acknowledge listeners)
  runs from such a timer due at once.
- Guest random numbers come from ``qemu_guest_getrandom`` seeded by
  ``-seed``.

Not covered: input from outside (console and sim-bridge commands, network
frames) arrives at host time; an external model that must answer a guest
write at a fixed point watches it with ``watch ADDR sync`` and acknowledges
it.  It runs several times slower than the default multi-threaded mode.

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

A tc4x machine is a machine type derived from ``tc4x-common-machine``
(see ``include/hw/tricore/tc4x_machine.h``), whose class names its board.
A board is a QOM type derived from ``tc4x-board`` (see
``include/hw/tricore/tc4x_board.h``).  Its class states what the SoC
applies itself: the console ASCLIN instance, GPIO inputs held by straps or
pull-ups, and register write semantics or kernel reset registers to add to
the SoC's own.  An ``init``
hook adds the board code through the services of
``include/hw/tricore/tc4x.h``: QSPI slaves, Ethernet PHYs, GPIO listeners,
SFR read and write hooks, which can tell the core making the access and
see writes before the SoC acts on them (a software trigger of a general
purpose service request included), non-maskable interrupts, analog inputs,
interrupt pacers, sim-bridge commands, and
the flashes: their storage, where DMU commands find an address, how
the PFLASH read windows map the storage, and a hook that sees every DMU
command before it runs.  A ``reset`` hook runs at every machine reset
before core 0 starts and may set its start address or keep it halted.

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
    up at 100 Mbit/s full duplex while the cable is plugged (sim-bridge
    command ``link up|down``) and port 0's network link is up.
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
  TBU, the SMU, the MTU and the peripherals around CPUcs are not modelled.
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
