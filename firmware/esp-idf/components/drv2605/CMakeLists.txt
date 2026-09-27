idf_component_register(
    SRCS
        "drv2605.c"
        "drv2605_cmd.c"
        "haptic.c"
        "haptic_tile.c"
    INCLUDE_DIRS 
    "."
    REQUIRES
        data_broker
        lvgl
        lvgl_ui
        esp_lvgl_port
        driver
        esp_driver_i2c   # Stage 35.2: i2c_master API
        freertos
        esp_timer
        boot_logic       # g_i2c1_bus_handle -- exposed via drv2605.h to consumers
)