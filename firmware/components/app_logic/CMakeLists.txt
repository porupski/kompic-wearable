idf_component_register(
    SRCS
        "app_nvs.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        data_broker
        lvgl_ui
)
