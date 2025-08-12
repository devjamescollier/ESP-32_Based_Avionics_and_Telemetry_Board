idf_component_register(SRCS "fc_diag.c"
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc
                                esp_driver_uart esp_driver_sdspi esp_adc sdmmc)
