set(GRAPE_DISPLAY_SRCS
    "src/grape_display.c"
)

if(CONFIG_GRAPE_DISPLAY_DRIVER_WAVESHARE_P4_BSP)
    list(APPEND GRAPE_DISPLAY_SRCS "drivers/grape_display_waveshare_p4_bsp.c")
endif()

if(CONFIG_GRAPE_DISPLAY_DRIVER_NULL)
    list(APPEND GRAPE_DISPLAY_SRCS "drivers/grape_display_null.c")
endif()

idf_component_register(
    SRCS ${GRAPE_DISPLAY_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES grape_common esp_lcd
    PRIV_REQUIRES esp_timer
)
