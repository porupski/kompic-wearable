idf_component_register(
    SRCS "veml6030.c" "light_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        data_broker
        lvgl
        lvgl_ui
    PRIV_REQUIRES
        boot_logic
)