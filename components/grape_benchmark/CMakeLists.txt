idf_component_register(
    SRCS
        "src/grape_benchmark.c"
        "src/grape_benchmark_report.c"
        "src/grape_benchmark_registry.c"
        "src/cases/grape_benchmark_damage_mark.c"
        "src/cases/grape_benchmark_damage_plan.c"
        "src/cases/grape_benchmark_compositor.c"
        "src/cases/grape_benchmark_pixel_backends.c"
        "src/cases/grape_benchmark_three_shear.c"
        "src/cases/grape_benchmark_fragmentation.c"
        "src/cases/grape_benchmark_presentation.c"
        "src/cases/grape_benchmark_lifecycle.c"
        "src/cases/grape_benchmark_scenes.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES grape grape_common grape_storage esp_timer
    PRIV_REQUIRES log heap esp_system esp_hw_support
)
