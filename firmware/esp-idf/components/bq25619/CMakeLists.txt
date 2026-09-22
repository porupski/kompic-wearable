idf_component_register(
    SRCS
        "bq25619.c"
        "bq25619_cmd.c"
        "battery_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
        data_broker
        lvgl
        esp_lvgl_port
        boot_logic        # g_display_sleep -- battery poll skips while display asleep
        max17048          # BQ REG_VBAT ADC unusable on iv8.0; use fuel gauge VCELL/SOC
    WHOLE_ARCHIVE  # bq25619_cmd.c has no intra-archive caller; force link so
                   # fc_cli.c's BQ verb resolves against the new command surface.
)
