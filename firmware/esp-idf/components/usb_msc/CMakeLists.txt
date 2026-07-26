idf_component_register(
    SRCS
        "usb_msc.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        freertos
        sdmmc
        fatfs
        vfs
        sdcard
        ws2812
)
