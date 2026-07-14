idf_component_register(
    SRCS
        "cst9217.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
)
