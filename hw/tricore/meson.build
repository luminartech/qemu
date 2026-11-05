tricore_ss = ss.source_set()
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testboard.c'))
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testdevice.c'))
tricore_ss.add(when: 'CONFIG_TRIBOARD', if_true: files('triboard.c'))
tricore_ss.add(when: 'CONFIG_TC27X_SOC', if_true: files('tc27x_soc.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_machine.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_sfr_tables.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_sfr_store.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_ir.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_timebase.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_ports.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_tmadc.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_i2c.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_qspi.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_flash.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_pacer.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_sim_bridge.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_kit.c'))

# Board extensions: each hw/tricore/boards/NAME adds its files to tricore_ss.
# The include path has hw/tricore/boards, not the board directories: board
# sources include their headers as "NAME/...", and no board header can
# shadow one that another source of tricore_ss includes.
if get_option('tc4x_boards').length() > 0
  tricore_ss.add(when: 'CONFIG_TC4X', if_true: declare_dependency(
    include_directories: include_directories('boards')))
endif
foreach board : get_option('tc4x_boards')
  subdir('boards' / board)
endforeach

hw_arch += {'tricore': tricore_ss}
