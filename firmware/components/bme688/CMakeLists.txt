idf_component_register(
    SRCS
        "bme688_drv.c"
        "env_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        lvgl
        lvgl_ui
        data_broker
        cross_driver
        boot_logic
)
