idf_component_register(
    SRCS
        "ws2812.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_rmt
        esp_timer
        freertos
)
