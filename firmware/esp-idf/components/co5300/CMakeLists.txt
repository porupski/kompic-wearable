idf_component_register(
    SRCS
        "co5300_panel.c"
        "co5300_panel_io.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
        esp_lcd
)
