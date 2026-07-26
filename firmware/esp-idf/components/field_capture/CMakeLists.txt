idf_component_register(
    SRCS
        "field_capture.c"
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
