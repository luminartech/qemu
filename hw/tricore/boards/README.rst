.. SPDX-License-Identifier: GPL-2.0-or-later

TC4x board extensions
=====================

A board of the tc4x machines (``docs/system/tricore/tc4x.rst``) is the
hardware around the SoC: devices on its buses and pins, straps, register
semantics, and what happens at reset.  Boards kept outside this tree are
built in from here: every subdirectory named in the ``tc4x_boards`` meson
option is included in the build, and everything in this directory except
this file is ignored by git.

Building
--------

.. code-block:: bash

  ln -s /path/to/my-board hw/tricore/boards/myboard
  ./configure --target-list=tricore-softmmu --tc4x-boards=myboard

A board directory holds a ``meson.build`` that adds its sources to
``tricore_ss``:

.. code-block:: meson

  tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('myboard.c'))

The include path has ``hw/tricore/boards``, so a board source includes
the board's headers as ``NAME/path`` (``"myboard/sensor.h"``); no board
header can then shadow a header of the tree.  Board code follows the
QEMU coding style.

Trace events
~~~~~~~~~~~~

A board directory may hold a ``trace-events`` file
(``docs/devel/tracing.rst``).  Its events belong to the group
``hw_tricore_boards_NAME``; the board's sources include the generated
header through a ``trace.h`` in the board directory (``"trace.h"`` from
a source next to it, ``"myboard/trace.h"`` from a subdirectory):

.. code-block:: c

  #include "trace/trace-hw_tricore_boards_myboard.h"

Board code reports state changes through trace events, guest mistakes
with ``qemu_log_mask(LOG_GUEST_ERROR, ...)`` and recognised but
unmodelled features with ``LOG_UNIMP``.

Board and machine types
-----------------------

A board is a QOM type derived from ``TYPE_TC4X_BOARD``
(``include/hw/tricore/tc4x_board.h``), named
``TC4X_BOARD_TYPE_NAME("NAME")`` so that ``-M tc4x,board=NAME`` selects
it.  A machine type derived from ``TYPE_TC4X_MACHINE``
(``include/hw/tricore/tc4x_machine.h``) that sets ``board = "NAME"`` in
its class makes ``-M NAME`` the same machine; ``hw/tricore/tc4x_kit.c``
is a complete example.

The board class states what the SoC applies itself:

``console``
  The ASCLIN instance on the first serial port.

``inputs``, ``n_inputs``
  GPIO pins the board holds at a fixed level (pull-ups, straps).

``reg_semantics``, ``kernel_resets``
  Register write semantics and kernel reset registers added to the SoC's
  own tables (``include/hw/tricore/tc4x_sfr_store.h``).

It has three hooks, which machine init and reset call in this order:

#. The machine creates the board object (``/machine/tc4x-board``) and the
   SoC (``/machine/soc``) with its properties.
#. ``wire(board, soc)``, before the SoC realizes: create the board
   devices that SoC devices link to, and set those links.  Of the SoC's
   children only ``flash`` and ``geth`` exist at this point; the GETH
   links (``phy0``, ``phy1``, ``port0-backend``, ``port1-backend``) must
   be set here.
#. The SoC realizes and creates its other children; the sim bridge is
   created.
#. ``init(board, soc)``: create the other board devices and connect them
   to the SoC's buses, lines and links.
#. The ``sfr-script`` runs and the ``-kernel`` images load.
#. At every machine reset: the devices reset (the SoC and its children,
   and every object registered with ``qemu_register_resettable()``), then
   ``reset(board, soc)`` runs, then core 0 starts.

Devices
-------

Each part of the board is a QOM device and a child of the board object
(``object_property_add_child(OBJECT(board), "NAME", OBJECT(dev))``), so
that it appears in the QOM tree under ``/machine/tc4x-board`` and its
properties can be read and set by path.  A device:

- realizes with ``Error **errp`` and fails realize on an invalid
  configuration instead of substituting a default;
- resets through its ``ResettableClass`` phases.  A device on no bus
  (``TYPE_DEVICE``) is not reset with the SoC: register it with
  ``qemu_register_resettable()``;
- sets ``user_creatable = false`` and a ``vmsd`` (``.unmigratable = 1``
  unless it migrates its state);
- implements ``TYPE_TC4X_SIM_COMMANDS``
  (``include/hw/tricore/tc4x_sim_bridge.h``) for sim-bridge commands of
  its own, parsing numbers with ``tc4x_sim_parse_uint/int/double()``.
  The board object may implement it too.

SoC devices
~~~~~~~~~~~

Board code reaches the SoC's devices by their child names:

.. code-block:: c

  Tc4xQspi *q3 = TC4X_QSPI(object_resolve_path_component(OBJECT(soc),
                                                         "qspi[3]"));

=================  ===============================================
Child of ``soc``   Type (header in ``include/hw/tricore/``)
=================  ===============================================
``i2c[0..2]``      ``TYPE_TC4X_I2C`` (``tc4x_i2c.h``)
``qspi[0..7]``     ``TYPE_TC4X_QSPI`` (``tc4x_qspi.h``)
``ports``          ``TYPE_TC4X_PORTS`` (``tc4x_ports.h``)
``tmadc``          ``TYPE_TC4X_TMADC`` (``tc4x_tmadc.h``)
``ir``             ``TYPE_TC4X_IR`` (``tc4x_ir.h``)
``timebase``       ``TYPE_TC4X_TIMEBASE`` (``tc4x_timebase.h``)
``flash``          ``TYPE_TC4X_FLASH`` (``tc4x_flash.h``)
``sfr-store``      ``TYPE_TC4X_SFR_STORE`` (``tc4x_sfr_store.h``)
``geth``           ``TYPE_TC4X_GETH`` (``include/hw/net/tc4x_geth.h``),
                   also ``tc4x_soc_geth()``
=================  ===============================================

I2C
  A slave is a ``TYPE_I2C_SLAVE`` device on the controller's bus
  ``tc4x_i2c_bus()``: ``i2c_slave_new()``, ``object_property_add_child()``
  and ``i2c_slave_realize_and_unref()``.  The controller moves a transfer
  byte by byte through the slave's ``event``, ``send`` and ``recv``
  methods; an address or data byte that no slave acknowledges sets NACK.

QSPI
  A peripheral is a device derived from ``TYPE_TC4X_QSPI_PERIPHERAL``
  with its ``cs`` property set, realized with
  ``tc4x_qspi_realize_peripheral()``, which also connects the module's
  chip select line to it.  Its ``frame()`` class method sees the BACON
  entry that starts a frame; ``SSIPeripheralClass.transfer`` serves each
  word.  Both run on a vCPU thread without the BQL, under the module's
  bus lock: they must not take the BQL.  The module's bus refuses other
  SSI peripherals, which do not keep this contract.  Code under the BQL that shares
  state with a peripheral (timers, sim commands, properties) takes
  ``tc4x_qspi_bus_lock()`` after the BQL.

Ports
  ``qdev_connect_gpio_out_named(ports, "p03-out", 9, irq)`` follows the
  output latch of P03.9.  A device drives the pin through
  ``qdev_get_gpio_in_named(ports, "p03-in", 9)`` (the level, set first)
  and ``"p03-drive"`` (1 while it drives the pin);
  ``tc4x_ports_drive()`` does both.  Writes to a port with a connected
  output line take the BQL, and the line runs under it.

TMADC
  A device with analog outputs implements ``TYPE_TC4X_ANALOG_SOURCE``:
  ``volts(src, line)`` runs at every conversion, on a vCPU thread without
  the BQL.  ``tc4x_tmadc_connect(adc, k, ch, src, line, errp)``, at
  machine init, makes channel ``ch`` of kernel ``k`` convert it.  Without
  a source a channel converts its property ``volts[K][CH]``; the property
  ``period-ns[K][SR]`` sets the period of the conversion rounds of a
  service request line (0: none, for a board that triggers conversions
  itself).

Interrupts
  ``tc4x_ir_src(ir, addr)``, the gpio-in line ``src[n]``, raises the
  service request node at ``addr`` from any thread, for example with
  ``qemu_irq_pulse()``; gpio-in ``nmi[c]`` raises the non-maskable
  interrupt of CPU c.  ``tc4x_ir_add_ack_listener()``, at machine init,
  sees the acknowledges of a node in the main loop.  A device's interrupt
  outputs are ``qemu_irq`` lines connected to ``src`` inputs.

Guest time
  A ``Tc4xGuestTimer`` (``tc4x_guest_timer_init/mod/del()``) fires once
  guest time reaches its deadline and re-arms itself when the time scale
  changes or guest time resumes from a pacer's barrier.  Interrupt
  pacers (``tc4x_timebase_new_pacer()``,
  ``include/hw/tricore/tc4x_pacer.h``) raise a periodic interrupt on an
  exact grid of guest time.  A timer of another kind re-arms itself from
  a retime notifier (``tc4x_timebase_add_retime_notifier()``).

Flash
  An object with the ``TYPE_TC4X_FLASH_POLICY`` interface, set as the
  flash's link ``policy`` in the ``init`` hook, holds the board's flash
  rules: ``locate`` (where a DMU command finds its flash), ``permit``
  (error flags that refuse a command), ``command`` (runs or refuses a
  command itself), ``error`` (sees the error flags raised on a command
  interface: what a failed command does beyond them) and ``reset`` (at
  every reset, once the contents are final).
  ``tc4x_flash_storage_read/write/fill()`` change the contents directly
  and ``tc4x_flash_set_maps()`` arranges the PFLASH read windows.

DMA
  An object with the ``TYPE_TC4X_DMA_POLICY`` interface
  (``include/hw/tricore/tc4x_dma.h``), set as the SoC's link
  ``dma-policy`` in the ``init`` hook, decides what a DMA channel does
  after a bus error: its ``bus_error`` method sees each failed access
  and drops what it delivers (the move's write after a source error,
  the loaded TCS after a linked-list load error) by returning true.
  Without a policy the channel goes on with the data the bus returned.

Ethernet
  A PHY implements ``TYPE_TC4X_GETH_PHY`` and is set as the GETH's
  ``phy0`` or ``phy1`` link, with its MDIO address in ``phy0-addr`` or
  ``phy1-addr``; its ``port_link()`` method follows the link of the
  port's network connection.  A device that implements
  ``TYPE_TC4X_GETH_PORT_BACKEND``, linked as ``port0-backend`` or
  ``port1-backend``, stands in for the port's network connection and
  delivers frames with ``tc4x_geth_port_receive()``.  Both kinds of link
  are set in the ``wire`` hook.

Registers
  A device that models SFRs maps its own MMIO region over the SFR store
  (which sits at priority ``TC4X_SFR_STORE_PRIORITY``) and serves it
  with ``read_with_attrs``/``write_with_attrs``: it refuses accesses of
  unknown bus masters with ``tc4x_master_refused()`` and tells the cores
  apart with ``tc4x_master_core()`` (``include/hw/tricore/tc4x_master.h``).
  A region that serves only some of its registers passes the others,
  with the access's attributes (the store's hooks see them through
  ``tc4x_access_core()``), to ``tc4x_sfr_store_access_read/write()`` and
  registers itself with ``tc4x_sfr_store_add_passthrough()``, whose
  callback names the served offsets.  The sim bridge refuses ``poke`` and ``force`` of the served
  registers, and ``watch`` unless the device implements the ``watch``
  method of ``TYPE_TC4X_SIM_COMMANDS`` and reports writes with
  ``tc4x_sim_watch_event()``.  ``tc4x_sfr_store_get/set/or()`` read and
  change registers that no device serves.

Threads and locks
-----------------

- Machine init, the board hooks and sim-bridge commands run in the main
  loop with the BQL held; device timers, the output lines of the ports
  and pacer callbacks run with the BQL held.
- QSPI peripherals run under their module's bus lock, possibly on a vCPU
  thread without the BQL (see above).
- Analog sources run, and the ``src`` and ``nmi`` lines may be raised, on
  a vCPU thread without the BQL.
- The state struct of a board device names the lock each field is under.

Transitional API
----------------

Besides the SoC type and ``tc4x_soc_geth()``,
``include/hw/tricore/tc4x.h`` holds transitional services: functions
with an opaque pointer that act on the devices, for board code that is
not converted to devices.  New board code uses the device API.  Where a
service needs an object of the device API, a shim object stands in for
it and appears in the QOM tree:

==========================================  ===================================
Transitional service                        Device API
==========================================  ===================================
``tc4x_i2c_attach()``, ``Tc4xI2cSlaveOps``  ``TYPE_I2C_SLAVE`` on the bus;
                                            shim ``tc4x-i2c-legacy``
``tc4x_qspi_attach()``,                     ``TYPE_TC4X_QSPI_PERIPHERAL``;
``Tc4xQspiSlaveOps``                        shim ``tc4x-qspi-legacy``
``tc4x_qspi_set_rx_fed()``                  the DMA moves the receive words
``tc4x_qspi_lock/unlock()``                 ``tc4x_qspi_bus_lock()``
``tc4x_geth_attach()``, ``Tc4xGethPhyOps``  ``TYPE_TC4X_GETH_PHY`` link;
                                            shim ``tc4x-geth-phy-legacy``
``tc4x_gpio_out()``,                        named gpio lines of ``ports``
``tc4x_gpio_set_input()``,
``tc4x_gpio_add_listener()``
``tc4x_adc_set_input()``,                   ``TYPE_TC4X_ANALOG_SOURCE``;
``tc4x_adc_volts()``,                       ``volts``, ``period-ns``
``tc4x_adc_set_volts()``,                   properties; shim
``tc4x_adc_set_period()``,                  ``tc4x-analog-legacy``
``tc4x_adc_sr_enabled()``
``tc4x_flash_set_locate()``,                ``TYPE_TC4X_FLASH_POLICY`` link;
``tc4x_dmu_set_hook()``,                    shim ``tc4x-flash-legacy-policy``
``tc4x_dmu_error()``
``tc4x_flash_read/write/fill()``,           ``tc4x_flash_storage_*()``,
``tc4x_pflash_set_maps()``,                 ``tc4x_flash_set_maps()``,
``tc4x_flash_kept()``                       ``tc4x_flash_is_kept()``
``tc4x_sfr_add_write_hook()``,              a device region over the store
``tc4x_sfr_add_read_hook()``
``tc4x_reg_get/set/or()``                   ``tc4x_sfr_store_get/set/or()``
``tc4x_access_core()``                      ``tc4x_master_core()`` of the
                                            access's attributes
``tc4x_irq_raise()``, ``tc4x_nmi()``        gpio-in ``src``, ``nmi`` of ``ir``
``tc4x_guest_ns()``,                        ``tc4x_timebase_ns()``,
``tc4x_host_ns_for_guest()``,               ``Tc4xGuestTimer``
``tc4x_time_add_retime_notifier()``
``tc4x_sim_add_command()``                  ``TYPE_TC4X_SIM_COMMANDS``
==========================================  ===================================

The boot services ``tc4x_set_boot_pc()``, ``tc4x_hold_boot()``,
``tc4x_start_core()`` and ``tc4x_release_boot()`` serve the ``reset``
hook and have no device counterpart.

Rules of the transitional services:

- SFR hooks are added at machine init, at registers the SFR store
  serves; a hook on a register a device serves aborts QEMU.  A write
  hook sees the write before the store applies it and consumes it by
  returning true; a read hook replaces the value read by returning true.
  Hooks run under the BQL, so the hooked accesses take it.
- Commands of ``tc4x_sim_add_command()`` match by prefix and are offered
  after the objects that serve the word; with ``qspi_lock`` they run
  under the bus locks of all QSPI modules.
- ``Tc4xQspiSlaveOps`` follow the bus lock contract of
  ``TYPE_TC4X_QSPI_PERIPHERAL``; ``tc4x_qspi_lock()`` takes the bus locks
  of all modules after the BQL.
