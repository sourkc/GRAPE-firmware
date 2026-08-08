idf_component_register(
    SRCS "src/grape_telemetry.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
    PRIV_REQUIRES log
)
