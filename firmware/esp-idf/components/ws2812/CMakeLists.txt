idf_component_register(
    SRCS
        "ws2812.c"
        "ws2812_cmd.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_rmt
        esp_pm             # Stage 11 Item D: PM lock around RMT tx
        esp_timer
        freertos
        rgb_policy         # ws2812_cmd pauses/resumes the policy
    WHOLE_ARCHIVE          # no intra-archive caller for ws2812_cmd_* -- fc_cli
                           # is the only consumer. Same gotcha as bq25619/pcf.
)
