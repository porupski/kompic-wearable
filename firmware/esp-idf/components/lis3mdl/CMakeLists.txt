idf_component_register(
    # Stage 38: lis3mdl.c back online, ported to driver/i2c_master.h.
    # compass_tile.c stays off the SRCS list for now -- not registered in
    # tile_registry, port it when the compass UI is revisited.
    SRCS
        "lis3mdl.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c      # Stage 38: driver/i2c_master.h
        esp_timer
        freertos
        data_broker
        boot_logic          # g_i2c0_bus_handle, g_i2c_mutex, BOOT_I2C_FREQ_HZ
        lvgl_ui             # g_mag_q for UI drain path (Stage 32.1)
)
