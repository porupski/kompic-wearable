idf_component_register(
    SRCS
        "rgb_policy.c"
    INCLUDE_DIRS "."
    REQUIRES
        ws2812
        esp_timer
)
