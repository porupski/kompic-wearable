idf_component_register(
    SRCS
        "lvgl_ui.c"
        "lvgl_ui_display.c"
        "lvgl_ui_screenshot.c"
        "ui_pane_styles.c"
        "ui_animations.c"
        "ui_subjects.c"
        "ui_broker.c"
        "ui_status_bar.c"
        "ui_lock_screen.c"
        "ui_shutdown_overlay.c"
        "ui_main_screen.c"
        "ui_settings_screen.c"
        "ui_navigation.c"
        "ui_notif_overlay.c"
        "tile_registry.c"
        "system_tile.c"
        "fa5_select_14.c"
        "fonts/space_grotesk_medium_16.c"
        "fonts/space_grotesk_medium_20.c"
        "fonts/space_grotesk_medium_28.c"
        "fonts/space_grotesk_medium_96.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl
        esp_lvgl_port
        data_broker
        app_logic
        veml6030
        pcf85063
        max_m10s
        lis3mdl
        drv2605
        lsm6dsv16x
        bme688
        max30101
        alarm
        sdcard              # screenshot writes to SD

        esp_timer
        driver
        esp_adc
        nvs_flash
    PRIV_REQUIRES
        spi_flash
        boot_logic
        esp_lcd     # boot_display.h now exposes esp_lcd panel/io handles (Stage 30.2)
    WHOLE_ARCHIVE  # lvgl_ui_screenshot.c has no intra-archive caller; force
                   # link so fc_cli's LVGL_SCREENSHOT verb resolves. Other
                   # objects were all pulled in via tile references already,
                   # so the size impact is essentially zero.
                   # See feedback_whole_archive_cmd_extract.md.
)
