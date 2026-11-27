idf_component_register(
    SRCS "main.c" "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES panel dualboot nvs_flash esp_timer esp_system esp_driver_gpio esp_driver_rmt
)
