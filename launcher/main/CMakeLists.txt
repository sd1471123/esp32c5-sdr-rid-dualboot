idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES panel dualboot nvs_flash esp_timer esp_system
)
