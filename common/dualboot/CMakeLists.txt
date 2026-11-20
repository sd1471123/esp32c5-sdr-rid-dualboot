idf_component_register(
    SRCS "dual_boot.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_partition esp_app_format nvs_flash esp_system esp_timer
)
