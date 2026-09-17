idf_component_register(
    SRCS
        "max17048.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
)
