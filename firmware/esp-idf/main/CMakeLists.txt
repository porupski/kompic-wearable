idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        boot_logic
        data_broker
        cross_driver
        app_logic
        nvs_flash
    # esp_lvgl_port intentionally omitted until display returns.
    # It stays in the graph transitively via lvgl_ui / driver *_tile.c files
    # (dead code, compiles fine), but main.c does not call it directly.
)