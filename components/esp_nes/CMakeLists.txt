file(GLOB_RECURSE T_SOURCES *.c)

idf_component_register(
    SRCS ${T_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES esp32_s31_korvo_1
    PRIV_REQUIRES driver esp_driver_uart nofrendo hid subor_keyboard nvs_flash spi_flash esp_lcd
)
