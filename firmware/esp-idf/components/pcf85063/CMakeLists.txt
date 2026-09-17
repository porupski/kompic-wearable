idf_component_register(
    SRCS
        "pcf85063.c"
        "pcf85063_cmd.c"
        "rtc_tile.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
        data_broker
        lvgl
        esp_lvgl_port
    PRIV_REQUIRES
        boot_logic
    WHOLE_ARCHIVE  # rtc_tile is read-only; nothing intra-archive references
                   # pcf85063_cmd_*. Same gotcha bq25619 hit -- force-link
                   # so fc_cli.c can resolve the new cmd surface + filename
                   # helper. See feedback_whole_archive_cmd_extract.md.
)