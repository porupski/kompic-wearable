idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        boot_logic
        data_broker
        cross_driver
        app_logic
        nvs_cfg
        field_capture     # for firmware_version.h (KOMPIC_FW_VERSION)
        lvgl_ui           # Stage 22 §4.1b: lvgl_ui_display_setup entry point
        driver
        nvs_flash
)