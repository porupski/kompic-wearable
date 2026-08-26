idf_component_register(
    SRCS
        "field_capture.c"
        "fc_common.c"
        "fc_recording.c"
        "fc_modes.c"
        "fc_modes_lsm.c"
        "fc_modes_ppg_bcg.c"
        "fc_battery_test.c"
        "fc_shutdown.c"
        "fc_cli.c"
        "fc_sync.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_tsens   # temperature_sensor.h for ESP32-S3 SoC temp in FCM_TEMP
        esp_adc            # Stage 11 TEMPORARY: Vbat divider on GPIO9 in BATT_TEST
        freertos
        esp_timer
        nvs_flash
        fatfs
        vfs
        data_broker
        cross_driver
        # Peripherals + drivers used by the state machine
        drv2605
        mic_pdm
        sdcard
        ws2812
        flashlight
        encoder
        pcf85063
        nvs_cfg
        usb_msc
        bq25619
        bme688
        lsm6dsv16x
        lis3mdl
        veml6030
        max30101
        tmp117
        qvar_ecg
)
