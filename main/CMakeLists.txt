idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "grape_demo.svg"
    REQUIRES
        grape
        grape_benchmark
        esp_timer
)
