idf_component_register(
    SRCS
        "boot_power.c"
        "boot_tasks.c"
        "boot_hw_init.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
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
        # Non-I2C peripherals
        encoder
        ws2812
        flashlight
        sdcard
        mic_pdm
        # Application logic (task_field_capture_fn extern)
        field_capture
        alarm
)
