idf_component_register(
    SRCS
        "src/grape_gfxlink.c"
        "src/grape_gfxlink_render.c"
    INCLUDE_DIRS "include"
    REQUIRES
        grape
        esp_tinyusb
)
