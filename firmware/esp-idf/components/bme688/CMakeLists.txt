idf_component_register(
    SRCS
        "bme688_drv.c"
        "env_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c   # Stage 35.3: i2c_master API
        lvgl
        lvgl_ui
        data_broker
        cross_driver
        boot_logic
)
