idf_component_register(
    SRCS
        "max17048.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c    # Stage 35.2: i2c_master API
        freertos
        esp_timer
        boot_logic        # g_i2c1_bus_handle
)
