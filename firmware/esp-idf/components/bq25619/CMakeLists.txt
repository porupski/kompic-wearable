idf_component_register(
    SRCS
        "bq25619.c"
        "battery_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
        data_broker
        lvgl
        esp_lvgl_port
)
