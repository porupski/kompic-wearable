idf_component_register(
    SRCS
        "ws2812.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_rmt
        esp_pm             # Stage 11 Item D: PM lock around RMT tx
        esp_timer
        freertos
)
