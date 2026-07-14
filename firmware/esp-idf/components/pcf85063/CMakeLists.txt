idf_component_register(
    SRCS
        "pcf85063.c"
        "rtc_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
        data_broker
        lvgl
        esp_lvgl_port
    PRIV_REQUIRES
        boot_logic
)