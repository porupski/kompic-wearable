idf_component_register(
    SRCS
        "co5300.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
)
