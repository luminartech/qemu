.. SPDX-License-Identifier: GPL-2.0-or-later

TC4x board extensions
=====================

Boards of the ``tc4x`` machine (``include/hw/tricore/tc4x_board.h``) that
are kept outside this tree are built in from here: every subdirectory
named in the ``tc4x_boards`` meson option is included in the build, and
everything in this directory except this file is ignored by git.

.. code-block:: bash

  ln -s /path/to/my-board hw/tricore/boards/myboard
  ./configure --target-list=tricore-softmmu --tc4x-boards=myboard

A board directory holds a ``meson.build`` that adds its sources to
``tricore_ss``:

.. code-block:: meson

  tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('myboard.c'))

and C sources that register one or more QOM types derived from
``TYPE_TC4X_BOARD``, named ``TC4X_BOARD_TYPE_NAME("NAME")`` so that
``-M tc4x,board=NAME`` selects them.  The board code follows the QEMU
coding style and uses the SoC services of ``include/hw/tricore/tc4x.h``
and the pacer of ``include/hw/tricore/tc4x_pacer.h``.

Threads and locks:

- The class ``init`` hook, sim-bridge commands, GPIO listeners, SFR write
  hooks, retime notifiers and pacer callbacks run in the main loop with
  the BQL held.
- QSPI slave operations run under the QSPI lock, possibly on a vCPU thread
  without the BQL: they must not take the BQL, allocate per access or call
  QEMU functions that need the BQL.  Board code that shares state with a
  slave takes ``tc4x_qspi_lock()``.
