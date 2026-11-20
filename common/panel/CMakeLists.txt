idf_component_register(
    SRCS "st7789.c" "buttons.c" "buzzer.c" "font5x7.c" "font7x12.c" "panel_draw.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_spi esp_hw_support esp_timer esp_rom soc
)
