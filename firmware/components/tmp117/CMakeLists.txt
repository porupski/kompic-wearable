idf_component_register(
    SRCS
        "tmp117.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        data_broker
        boot_logic         # g_i2c_mutex
)
