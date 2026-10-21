tricore_ss = ss.source_set()
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testboard.c'))
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testdevice.c'))
tricore_ss.add(when: 'CONFIG_TRIBOARD', if_true: files('triboard.c'))
tricore_ss.add(when: 'CONFIG_TC27X_SOC', if_true: files('tc27x_soc.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_gpio.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_adc.c'))
tricore_ss.add(when: 'CONFIG_TC4X', if_true: files('tc4x_i2c.c'))

hw_arch += {'tricore': tricore_ss}
