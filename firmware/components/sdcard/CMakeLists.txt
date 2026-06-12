idf_component_register(
    SRCS
        "sdcard.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        sdmmc
        fatfs
        vfs
        esp_timer
        freertos
)
