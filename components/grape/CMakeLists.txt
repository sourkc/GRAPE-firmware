idf_component_register(
    SRCS
        "src/grape.c"
        "src/grape_feature.c"
        "src/grape_benchmark_hooks.c"
        "src/grape_function_profile.c"
        "src/grape_texture.c"
        "src/grape_gpu.c"
        "src/grape_gpu_buffer.c"
        "src/grape_gpu_depth.c"
        "src/grape_gpu_texture.c"
        "src/grape_gpu_tile.c"
        "src/grape_gpu_pipeline.c"
        "src/grape_gpu_vertex.c"
        "src/grape_gpu_raster.c"
        "src/grape_shader.c"
        "src/grape_screenshot.c"
        "src/grape_path.c"
        "src/grape_font.c"
        "src/grape_glyph_cache.c"
        "src/grape_text.c"
        "src/grape_svg.c"
        "src/grape_surface.c"
        "src/grape_damage.c"
        "src/grape_debug.c"
        "src/grape_compositor.c"
        "src/grape_shear.c"
        "src/grape_ppa.c"
        "../../shaders/generated_shaders.c"
    INCLUDE_DIRS
        "include"
        "../../shaders"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES grape_common grape_display
    PRIV_REQUIRES heap esp_driver_ppa esp_driver_jpeg freertos
)


if(CONFIG_GRAPE_FUNCTION_PROFILING)
    target_compile_options(${COMPONENT_LIB} PRIVATE -finstrument-functions)
    set_source_files_properties(
        "src/grape_function_profile.c"
        "src/grape_benchmark_hooks.c"
        PROPERTIES COMPILE_OPTIONS "-fno-instrument-functions"
    )
endif()
