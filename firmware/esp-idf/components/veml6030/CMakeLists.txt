idf_component_register(
    # Stage 35: veml6030.c + veml6030_cmd.c + light_tile.c dropped -- legacy
    # driver/i2c.h incompatible with new i2c_master API. Stub provides identity.
    SRCS
        "veml6030_stub.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        data_broker
)
