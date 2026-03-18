avr_ss = ss.source_set()
avr_ss.add(files('boot.cpp'))
avr_ss.add(when: 'CONFIG_AVR_ATMEGA_MCU', if_true: files('atmega.cpp'))
avr_ss.add(when: 'CONFIG_ARDUINO', if_true: files('arduino.cpp'))

hw_arch += {'avr': avr_ss}
