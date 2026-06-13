idf_component_register(
    SRCS
        "alarm.c"
        "alarm_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        data_broker
        drv2605
        pcf85063
        ws2812
        app_logic
        lvgl
        esp_lvgl_port
        lvgl_ui
        freertos
        esp_timer
        nvs_flash
)