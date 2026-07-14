idf_component_register(
    SRCS
        "max30101.c"
        "health_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        lvgl
        lvgl_ui
        data_broker
        cross_driver
        drv2605            # health_tile.c needs haptic_play() / haptic_get_ui_effect()
        boot_logic         # g_i2c_mutex
)
