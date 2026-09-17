idf_component_register(
    SRCS
        "boot_power.c"
        "boot_tasks.c"
        "boot_hw_init.c"
        "boot_display.c"
        "boot_pm.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_pm             # Stage 11 Item D Block A -- esp_pm_configure
        esp_timer
        esp_lcd            # Stage 30.2 -- esp_lcd_new_panel_io_spi in boot_display.c
        data_broker
        app_logic
        cross_driver
        nvs_flash
        # I2C drivers
        bme688
        lsm6dsv16x
        lis3mdl
        veml6030
        max30101
        tmp117
        pcf85063
        drv2605
        bq25619
        max17048
        # Display bring-up (Stage 21 §4.1a)
        co5300
        cst9217
        # Non-I2C peripherals
        encoder
        ws2812
        rgb_policy
        flashlight
        sdcard
        mic_pdm
        # GNSS (Stage 25 Batch A)
        max_m10s
        # Application logic (task_field_capture_fn extern)
        field_capture
        alarm
)
