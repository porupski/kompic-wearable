idf_component_register(
    SRCS
        "nvs_cfg.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_timer
        pcf85063
)
