.. SPDX-License-Identifier: GPL-2.0-or-later

Infineon AURIX TC4Dx (``tc4x``, ``kit-tc4d7-lite``)
===================================================

The tc4x machines model an Infineon AURIX TC4Dx: seven TriCore 1.8
cores (CPU type ``tc4x``; CPU0..5 and the CSRM core CPUcs) running in
parallel under MTTCG, their local scratchpad RAMs, the LMU, program and
data flash, and the on-chip peripherals listed below.  They load TC4Dx
images from Intel HEX or ELF files.

``tc4x``
  The SoC with the board that ``board=NAME`` names (default
  ``generic``: the SoC alone).

``kit-tc4d7-lite``
  The SoC on Infineon's TC4D7 lite kit, the same machine as
  ``-M tc4x,board=kit-tc4d7-lite``.

Every tc4x machine type derives from the abstract
``tc4x-common-machine`` and shares its properties (see `Running`_).

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
* DMA0 and DMA1: linked lists, software and QSPI hardware requests; a
  bus error sets SER, DER or DLLER and the channel (LEC) in the ERRSR of
  the channel's resource partition, whose EER enables the partition's
  error service request and whose CLRE clears the flags.  What the
  channel does beyond that is the board's DMA policy; without one it
  goes on with the data the bus returned
* QSPI0..7: full-duplex FIFOs, each module the master of an SSI bus with
  16 chip selects; the end of a frame (``BACON.LAST``) releases its chip
  select, and on frames that a CPU drives raises the phase transition
  request that ``GLOBALCON1`` enables for it; DMA jobs end through phase
  transition 2 or the move counter
* I2C0..2: each controller the master of an I2C bus; a byte that no slave
  acknowledges sets NACK
* GETH0 (``hw/net/tc4x_geth.c``), based on the Synopsys DWC Ethernet QoS
  core, with its two MAC ports (see `GETH`_)
* ASCLIN0..27: transmit at once and raise their service requests.  One
  instance is the console on the first serial port: ASCLIN0 unless the
  board names another.
* PFLASH and DFLASH with the DMU command interfaces of the host and the
  CSRM (see `Flash`_)
* GPIO ports P00..P41, TMADC0..7 conversions of pin voltages, eGTM TOM
  counters and TBU channel 0
* Start-up status that completes at once: the TMADC power-on calibration
  phase (``MODSTAT.CALPH``), the clock ramp oscillator and the VMT memory
  tests

Every other SFR is plain storage with the TC4x write semantics: bits that
clear when written with 1 or 0 and bits that set when written with 1 act
so, write-only bits read as 0, other bits read what was written, and
writing a module's ``RST_CTRLB.KRST`` completes its kernel reset at once.
The tables (``hw/tricore/tc4x_sfr_tables.c``) are generated from the
access types of the iLLD SFR headers with
``scripts/tricore/tc4x-sfr-tables.py``; a board can add to them.  The
register addresses and bit fields the models use
(``include/hw/tricore/tc4x_regs.h``) are generated from the same headers
with ``scripts/tricore/tc4x-regs.py``.

Running
-------

.. code-block:: bash

  qemu-system-tricore -M tc4x -display none \
      -serial stdio -serial unix:/tmp/sim.sock,server=on,wait=off \
      -kernel image.hex

``-kernel`` takes a comma-separated list of images; core 0 starts at the
first entry point among them that is not 0.  Machine properties
(``-M NAME,PROP=VALUE``):

``board=NAME``
  Board around the SoC (default ``generic``); ``board=help`` lists the
  boards built in.  ``tc4x`` machine only: the other machines name their
  board.

``sfr-script=FILE``
  File of sim-bridge commands run at machine init, after the board code;
  a command that fails stops QEMU with the file, the line and the reply.

``sim-chardev=ID``
  Character device of the sim bridge (default: the second serial port).
  An ID that names no character device stops QEMU.

``shm-dir=DIR``
  Back the core and LMU RAMs with shared files ``DIR/core0.spr`` ..
  ``DIR/core6.spr`` and ``DIR/lmu`` that external processes can map: the
  machine creates a ``memory-backend-file`` object for each, named after
  the RAM.

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
  ``-icount shift=N,sleep=off`` (which also keeps TCG to one thread) and
  ``-seed``; QEMU refuses to start without them.

``time-scale=N``
  With ``deterministic=on``: guest time runs at 1/N of QEMU virtual time,
  fixed (default 1).  Any other value without ``deterministic=on``, and 0,
  stop QEMU.

CPU property, with ``-global tc4x-tricore-cpu.csa-cache=on``:

``csa-cache=on``
  Remember the RAM pages of the context save areas across TBs instead of
  probing them on every call and return.  Faster, but stores through them
  skip dirty tracking and watchpoints set later, and the RAM must not be
  remapped; off by default.

Device tree
-----------

The machine builds this QOM tree (``info qom-tree`` shows it with the
memory regions and interrupt lines):

.. code-block:: none

  /machine                    the machine (tc4x-common-machine)
    /soc                      tc4x-soc
      /sfr-store              tc4x-sfr-store
      /ir                     tc4x-ir
      /timebase               tc4x-timebase
      /flash                  tc4x-flash
      /i2c[0] .. /i2c[2]      tc4x-i2c, each with its bus "i2c"
      /qspi[0] .. /qspi[7]    tc4x-qspi, each with its bus "ssi"
      /ports                  tc4x-ports
      /tmadc                  tc4x-tmadc
      /geth                   tc4x-geth
    /sim-bridge               tc4x-sim-bridge
    /tc4x-board               the board object and its devices

No device of the tree is user-creatable: the machine creates the SoC,
the SoC its children, and the board its own devices.  The sim bridge's
``get`` and ``set`` commands, and QMP ``qom-get`` / ``qom-set``, reach
every property below by path.

``tc4x-soc``
  The CPUs (``cpu[0]`` .. ``cpu[6]``), the core RAMs and the LMU, the
  DMA, the ASCLINs, the eGTM TOMs and the children below.  Properties
  set by the machine: ``cpu-type``, ``console`` (the ASCLIN instance on
  the first serial port), ``console-chardev``, ``imprecise-io``,
  ``deterministic``, ``time-scale`` and ``port-straps``; ``pflash`` and
  ``dflash`` are aliases of the flash's drives.  Links ``lmu-memdev``
  (64 MiB) and ``core0-memdev`` .. ``core6-memdev`` (2 MiB each: DSPR and
  PSPR) take a RAM from a memory backend instead of plain RAM, for
  example ``-object
  memory-backend-file,id=lmu,size=64M,mem-path=/dev/shm/lmu,share=on
  -global tc4x-soc.lmu-memdev=lmu``; a backend of the wrong size, one
  already in use, or ``shm-dir`` and a link for the same RAM stop QEMU.
  Link ``dma-policy``: the board's handling of DMA bus errors.

``tc4x-sfr-store``
  The background of the SFR segments at 0xE0000000 and 0xF0000000,
  mapped at priority -1000 below every device that serves registers of
  its own: the plain registers with their write semantics and kernel
  resets.  It keeps the value of every register once machine init is
  done and restores them at every machine reset, so that a system reset
  starts from the registers of a cold start.  Devices that map a region
  over it may pass the registers they do not serve back to it.

``tc4x-ir``
  The service request nodes (SRC registers at 0xF4432000) and the
  arbitration that offers each CPU its highest pending priority.  Named
  gpio-in lines: ``src[n]`` sets SRR of the node at 0xF4432000 + 4 * n (a
  node already pending sets IOV), ``nmi[c]`` raises the non-maskable
  interrupt of CPU c.  Links ``cpu0`` .. ``cpu6``.

``tc4x-timebase``
  Guest time (see `Guest time`_): the count of the CPUs' system timers
  (STM) and of eGTM TBU channel 0.  It serves the first SFR page of each
  CPU and of eGTM cluster 0, passing what it does not serve to the store.
  Named gpio-out lines ``stm[16 * n + 2 * vm + k]``: service request k of
  virtual machine vm of the STM of CPU n.  Links ``sfr-store``, ``ir``,
  ``cpu0`` .. ``cpu6``; properties ``deterministic`` and ``time-scale``.

``tc4x-flash``
  PFLASH and DFLASH with the DMU command interfaces (see `Flash`_).
  Drive properties ``pflash`` and ``dflash``; link ``policy``.

``tc4x-i2c``
  One I2C module (``index``): it serves the transfer registers and is the
  master of its child bus ``i2c``, on which board code puts
  ``TYPE_I2C_SLAVE`` devices.  Trace event ``tc4x_i2c_transfer``.

``tc4x-qspi``
  One QSPI module (``index``): it serves the transmit, receive and status
  registers and is the master of its child bus ``ssi``, which takes only
  ``tc4x-qspi-peripheral`` devices: it runs without the BQL.  A BACONENTRY
  write selects a chip select, whose gpio-out line ``cs[n]`` goes low
  while the frame runs; every word written to DATAENTRY is one transfer
  of the selected peripheral, whose reply goes to the receive FIFO.  Its
  service requests (TX, RX, ERR, PT, U) are sysbus interrupts wired to the
  router.  Trace events ``tc4x_qspi_frame``, ``tc4x_qspi_frame_end`` and
  ``tc4x_qspi_dma_job``.

``tc4x-ports``
  The GPIO ports P00..P41: it serves OUT, IN, OMR, OMSR, OMCR and the GPIO
  bits of ``PADCFGn_GPIO``.  Named lines of port NN (two decimal digits),
  16 each: gpio-out ``pNN-out`` (the output latch), gpio-in ``pNN-in``
  (the level a device outside drives) and ``pNN-drive`` (1 while that
  level holds the pin, 0 when IN reads the latch again).  Property
  ``straps``: pins the board holds at a fixed level.  Trace event
  ``tc4x_ports_out``.

``tc4x-tmadc``
  TMADC0..7: it serves the results ``AW0_RES0..15`` and ``MODSTAT``.  A
  kernel that runs converts, for each of its six service request lines,
  a round of channels every ``period-ns[K][SR]`` ns of guest time (default
  10 ms; 0: none, for boards that trigger conversions themselves).  A
  channel converts the output of the analog source that board code
  connected to it, else the voltage in its property ``volts[K][CH]``
  (0 to 3.3 V; beyond that the input saturates).  Named gpio-out lines
  ``sr[6 * K + i]``: service request SRi of kernel K.

``tc4x-geth``
  GETH0, see `GETH`_.

``tc4x-sim-bridge``
  The sim bridge (see `Sim bridge`_): property ``chardev``, link ``soc``.

Bus masters
~~~~~~~~~~~

Every access in the SoC carries its bus master in
``MemTxAttrs.requester_id`` (``include/hw/tricore/tc4x_master.h``): a
master index in bits 15:1 and, for a core, its ``PSW.S`` in bit 0.  The
indexes are the model's own numbering: CPU0..5 and CPUcs, the DMA
channels of both DMA modules, GETH, and the debug master of the sim
bridge; a board maps them onto the master IDs its protection units
compare.  The CPU properties ``requester-id`` and ``requester-id-safe``
give a core's IDs for ``PSW.S`` clear and set; the SoC sets them.

The SFR store, the router, the time base pages, the I2C, QSPI, port and
TMADC devices and the GETH registers refuse an access whose requester ID
is none of these with a guest error and a bus error response.
Unspecified attributes (monitor, qtest, loader, generic DMA helpers)
count as the debug master.  DMA bus errors are reported as described
above; a bus error of a GETH DMA channel sets ``DMA_CH_STATUS.FBE``, and
the channel goes on with the data the bus returned.

GETH
~~~~

GETH0 is based on the Synopsys DWC Ethernet QoS core and has two MAC
ports.  A ``-nic`` with ``model=tc4x-geth`` connects port 0 and one with
``model=tc4x-geth-port1`` port 1 (properties ``netdev``, ``mac``,
``netdev1``, ``mac1``).

Board code links a PHY to a port through the controller's ``phy0`` /
``phy1`` links (an object with the ``tc4x-geth-phy`` interface, MDIO
address in ``phy0-addr`` / ``phy1-addr``); MDIO reads of a port without
one, or at another address, return all ones.  The PHY sees the link state
of the port's network connection (QMP ``set_link``).  A device linked as
``port0-backend`` or ``port1-backend`` (interface
``tc4x-geth-port-backend``) stands in for the port's network connection:
the port then has no NIC, and a netdev for it as well stops QEMU.

The bridge forwards frames from a receive queue of one port to a transmit
queue of the other when the TxQ/RxC mapping makes that queue a forwarding
path (``BRIDGE_TXQ_MAP_PORT_n``, ``BRIDGE_PORT_n_CONTROL``); its packet
length limit, queue flushes and same-port loopback are not modelled.

Sim bridge
----------

The sim bridge is a line-oriented command interface for external
processes and tests.  It runs on the character device that the machine
property ``sim-chardev`` names, else on the second serial port:

.. code-block:: bash

  qemu-system-tricore -M kit-tc4d7-lite,sim-chardev=sim -display none \
      -chardev socket,id=sim,path=sim.sock,server=on,wait=off \
      -serial stdio -kernel image.elf

Protocol
~~~~~~~~

* A command is one line of words separated by blanks; the first word
  names the command.  Empty lines and lines starting with ``#`` do
  nothing.
* Every command is answered by zero or more data lines and then one line
  that starts with ``ok`` or ``err``.  Commands run one at a time, in the
  order they arrive.
* Numbers are in C syntax (decimal, ``0x`` hexadecimal, ``0`` octal).  A
  malformed number, a trailing character, a value out of range, a
  missing or extra argument is an ``err usage: ...`` reply, never a
  default or a truncated value.  An unknown command is ``err unknown
  command 'WORD'``.
* Event lines start with ``ev`` and may come at any time, before a reply
  included.  ``ev w ADDR VAL SIZE CPU`` reports a watched write once it
  took effect (``CPU`` -1: not a core).

The bridge serves:

=========================================  ====================================
``r ADDR [1|2|4]``                         read through the bus, ``ok VALUE``;
                                           a read the bus refuses is ``err
                                           VALUE bus error at ADDR``
``w ADDR VAL [1|2|4]``                     write through the bus (side effects
                                           run), ``ok``; a write the bus
                                           refuses is ``err bus error at
                                           ADDR``.  As a debugger, it patches
                                           flash.
``master [debug|cpu N [safe]|dma CH]``     the bus master of ``r`` and ``w``
                                           (default: the debug master);
                                           replies with the master
``get PATH PROP``                          a QOM property, ``ok VALUE``
``set PATH PROP VALUE``                    set a QOM property from its text
                                           form
``vmstop``, ``vmcont``                     pause and resume the guest
``ack ID``                                 the core waiting since the event
                                           ``... sync ID`` goes on
=========================================  ====================================

The devices serve:

=============================  ==============================================
``poke ADDR VAL``              set a stored SFR value, no side effects
                               (``/soc``)
``force ADDR VAL [MASK]``      SFR reads return VAL on MASK until ``unforce``
                               (``/soc``)
``unforce ADDR``               release a force (``/soc``)
``watch ADDR [sync]``          report guest writes (``/soc``)
``unwatch ADDR``               stop reporting them (``/soc``)
``cores``                      per core: PC, ``run``/``wait``/``bhalt``, ICR,
                               pending priority, D4..D6, A11 (``/soc``)
``hot [MIN]``                  ``hot ADDR READS VALUE`` for each SFR read at
                               least MIN (default 100) times since the last
                               ``hot`` (``/soc/sfr-store``)
``irq SRC``                    hardware-set SRR of a service request node
                               (``/soc/ir``)
``timescale N``                request guest time at 1/N of virtual time,
                               ``ok SCALE`` with the scale in effect;
                               refused with ``deterministic=on``
                               (``/soc/timebase``)
``gpio PORT PIN 0|1|release``  drive or release a pin from outside
                               (``/soc/ports``; PORT in decimal)
``adc K CH VOLTS``             set ``volts[K][CH]`` (``/soc/tmadc``)
``button press|release``       the kit's user button
``link up|down``               the kit's Ethernet cable
=============================  ==============================================

``poke``, ``force`` and ``unforce`` act on registers of the SFR store
only; for a register that a device serves, the reply is ``err ADDR owned
by PATH``.  ``watch`` asks the register's owner: the SFR store and the
interrupt router report writes, for a register of another device the
reply is an error.

``watch ADDR sync`` makes the watch synchronous: a core that writes the
register sends ``ev w ADDR VAL SIZE CPU sync ID``, leaves its translation
block, which a precise access ends (always with ``-icount``), and waits
outside the access until the client sends ``ack ID``, so the core sees
the client's answer (``poke``, ``irq``) before its next instruction.  The
client's other commands run while the core waits.  A stop of the VM (``vmstop``, a reset, quit) or
the client's disconnection lets the core go on without the ack; a write
that no core makes (``w``, a timer) sends the event without ``sync``.

The ``sfr-script`` machine property runs a file of commands at machine
init through the same interface.

A device, a board or any other object adds commands by implementing the
QOM interface ``tc4x-sim-commands``
(``include/hw/tricore/tc4x_sim_bridge.h``): its class lists the command
words and runs them, and may decline a line so that the next object that
serves the word gets it.  The bridge finds the objects in the QOM tree.

Flash
-----

The PFLASH (80 MiB, read windows at 0x80000000 and 0xA0000000) and the
DFLASH (32 MiB at 0xAE000000) are the device ``tc4x-flash``, the SoC's
child ``flash``.  Guest loads and instruction fetches read them at RAM
speed; a guest store into them is a guest error with a bus error
response, since only the DMU programs flash.  DMU commands, the sim
bridge ``w`` and board code change the contents, and translated code of
the changed bytes is discarded.

Drives
~~~~~~

The flash properties ``pflash`` and ``dflash`` (aliased on the SoC) take
raw drives of the flash's size that hold the contents across runs,
written through on every change:

.. code-block:: bash

  qemu-system-tricore -M tc4x ... \
      -drive if=pflash,index=0,format=raw,file=pflash.bin \
      -drive if=pflash,index=1,format=raw,file=dflash.bin

``index=0`` is the PFLASH (80 MiB), ``index=1`` the DFLASH (32 MiB); the
machine property ``flash-dir`` makes the files of a directory these
drives, and cannot be combined with them.  A drive must be writable.  A
flash without a drive starts erased and lives in memory.

When a drive holds anything but erased cells (zeros), the flashes are
kept and ``-kernel`` images seed only the RAMs; otherwise the images seed
the flashes at the first reset.  A machine reset leaves the flash
contents as the guest programmed them.

DMU
~~~

The DMU command interfaces of the host and of the CSRM program, erase
and verify the flashes with the command sequences of the iLLD
``IfxFlash.h``.  A command whose address is not aligned to its page,
wordline or logical sector (sizes of the iLLD ``IfxFlash_cfg.h``), or
whose range has no flash, fails with a sequence error (SQER).  A failed
command has no effect beyond the flags it sets in ``ERR``.  A board can
decide where a command finds its flash, refuse or run commands itself
and what a failed command does through the link ``policy``.

Guest time
----------

The STM and the eGTM count guest time, which runs at 1/N of QEMU virtual
time.  N is the largest request of its clients: the sim bridge
(``timescale``) and interrupt pacers.  Slowing guest time down gives
external processes, and emulated cores that are slower than silicon, more
wall time per guest millisecond.  Trace event ``tc4x_timebase_set_scale``
reports every change.

A pacer (``tc4x_pacer.c``) serves board code that interrupts one core at a
fixed period of guest time.  It raises the period's interrupts on an
exact grid of guest time, stops guest time at the next slot until the
main loop raised it, and holds guest time (at a large scale) while a
raised interrupt is still pending, so that no two periods merge into one
interrupt.  Its logic is covered by ``tests/unit/test-tc4x-pacer.c``.

Deterministic mode
------------------

With ``deterministic=on`` the guest-visible results of a run depend only on
the command line, the images and the inputs:

.. code-block:: bash

  qemu-system-tricore -M tc4x,deterministic=on,time-scale=4 \
      -icount shift=0,sleep=off -seed 1 ...

- Guest time is QEMU virtual time divided by the fixed ``time-scale``, and
  with ``-icount shift=N,sleep=off`` virtual time is the instruction count
  times 2^N ns (idle CPUs skip ahead to the next timer).  One TCG thread
  runs the CPUs in turn, so 7 busy cores share the count; a core held by a
  synchronous watch stops before its next instruction, the other cores at
  the end of their turn.
- Requests for another scale are ignored and ``timescale`` is refused;
  pacers only raise their interrupts on the grid: they neither stop nor
  slow guest time, and a period the core has not taken merges with the
  next, as on silicon.
- Device timers are QEMU_CLOCK_VIRTUAL timers, which -icount runs at a
  fixed instruction count; work that the default mode hands to the main
  loop (DMA jobs started without the BQL, interrupt acknowledge listeners)
  runs from such a timer due at once.
- Guest random numbers, which board devices draw from
  ``qemu_guest_getrandom``, come from ``-seed``.
- ``shm-dir`` RAM is an input like the others: a process that shares it
  keeps runs repeatable only by changing it while a core waits on a
  synchronous watch.

Trace event ``tc4x_timebase_stm_match`` stamps every STM compare match
with its guest time, so that two runs can be compared event by event.

Not covered: input from outside (console and sim-bridge commands, network
frames) arrives at host time; an external model that must answer a guest
write at a fixed point watches it with ``watch ADDR sync`` and acknowledges
it.  The mode is much slower than the default multi-threaded one, since
one thread runs every core.

Hot paths without the BQL
-------------------------

The SFR regions are ``lockless_io``: register reads without side effects,
the STM and TBU counters, GPIO pin writes, TOM shadow register writes and
QSPI register accesses are served on the vCPU thread without taking the
BQL.  Other accesses with side effects take the BQL.  QSPI accesses run
the peripherals on the module's bus and raise service requests under the
module's bus lock, so peripherals must not take the BQL.

Boards
------

A board is the hardware around the SoC: devices on its buses and pins,
straps, register semantics, and what happens at reset.  Board code is a
QOM type derived from ``tc4x-board`` (``include/hw/tricore/tc4x_board.h``)
whose devices are QOM devices linked to the SoC's; a machine type
derived from ``tc4x-common-machine`` names it.  Boards kept outside this
tree are built in from ``hw/tricore/boards/``: see
``hw/tricore/boards/README.rst`` for the board API.

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
  * DP83825I Ethernet PHY (``/machine/tc4x-board/phy``) at MDIO address 0
    of GETH0 port 0 with the link up at 100 Mbit/s full duplex while the
    cable is plugged (sim-bridge command ``link up|down``) and port 0's
    network link is up.
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
* Migration and snapshots: every tc4x device is unmigratable, so
  ``savevm`` and migration refuse to run.
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
