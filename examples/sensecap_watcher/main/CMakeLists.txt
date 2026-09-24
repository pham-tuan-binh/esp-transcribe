idf_component_register(SRCS main.c serial_test.c watcher.c
                       PRIV_REQUIRES esp-transcribe esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_uart
                                     esp_lcd esp_timer)
