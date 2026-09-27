idf_component_register(
    SRCS
        "cst9217.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c   # Stage 35.3: i2c_master API
        freertos
        esp_timer
        boot_logic       # g_i2c_mutex + g_i2c0_bus_handle
)
