idf_component_register(
    SRCS
        "mic_pdm.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2s
        esp_timer
        freertos
)
