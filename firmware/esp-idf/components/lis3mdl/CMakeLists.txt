idf_component_register(
    # Stage 35: lis3mdl.c + compass_tile.c dropped -- legacy driver/i2c.h
    # incompatible with new i2c_master API. Stub provides identity.
    SRCS
        "lis3mdl_stub.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        data_broker
)
