idf_component_register(
    SRCS
        "src/grape.c"
        "src/grape_texture.c"
        "src/grape_surface.c"
        "src/grape_damage.c"
        "src/grape_compositor.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES grape_common grape_display
    PRIV_REQUIRES heap
)
