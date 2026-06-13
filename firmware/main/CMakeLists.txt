idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        boot_logic
        data_broker
        esp_lvgl_port
        nvs_flash
)