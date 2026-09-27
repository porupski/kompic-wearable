idf_component_register(
    # Stage 35: tmp117.c dropped -- legacy driver/i2c.h incompatible with new
    # i2c_master API. Stub provides identity.
    SRCS
        "tmp117_stub.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        data_broker
)
