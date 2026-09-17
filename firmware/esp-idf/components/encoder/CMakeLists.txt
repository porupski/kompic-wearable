idf_component_register(
    SRCS
        "encoder.c"
        "encoder_cmd.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        freertos
    WHOLE_ARCHIVE  # encoder has no tile yet (deferred until Mk1b display is
                   # up); nothing intra-archive references encoder_cmd_*,
                   # so force-link the object. See
                   # feedback_whole_archive_cmd_extract.md.
)
