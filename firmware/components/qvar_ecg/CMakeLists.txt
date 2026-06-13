idf_component_register(
    SRCS
        "qvar_ecg.c"
        "ecg_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        freertos
        lvgl
        lvgl_ui
)
