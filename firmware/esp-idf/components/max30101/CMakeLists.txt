idf_component_register(
    # Stage 35: max30101.c + health_tile.c dropped (they use legacy driver/i2c.h
    # which is incompatible with the new i2c_master API this binary now uses).
    # Stub file provides only chip identity strings.
    SRCS
        "max30101_stub.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        data_broker
)
