#pragma once

#include "grape/grape_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x;
    float y;
} grape_shader_vec2_t;

typedef struct {
    float x;
    float y;
    float z;
} grape_shader_vec3_t;

typedef struct {
    float x;
    float y;
    float z;
    float w;
} grape_shader_vec4_t;

typedef struct {
    grape_shader_vec2_t c0;
    grape_shader_vec2_t c1;
} grape_shader_mat2_t;

typedef struct {
    grape_shader_vec3_t c0;
    grape_shader_vec3_t c1;
    grape_shader_vec3_t c2;
} grape_shader_mat3_t;

typedef struct grape_shader_kernel_args grape_shader_kernel_args_t;

typedef struct {
    grape_shader_vec4_t source_color;
    grape_shader_vec2_t uv;
    grape_shader_vec2_t local_position;
    grape_shader_vec2_t surface_size;
    grape_shader_vec2_t frag_coord;
} grape_shader_eval_args_t;

typedef grape_shader_vec4_t (*grape_shader_eval_fn_t)(
    const grape_shader_eval_args_t *args,
    const void *uniforms
);

typedef void (*grape_shader_kernel_fn_t)(
    const grape_shader_kernel_args_t *args,
    const void *uniforms
);

enum {
    GRAPE_SHADER_PROGRAM_USES_SOURCE_COLOR = 1U << 0,
};

typedef struct {
    grape_shader_kernel_fn_t kernel;
    grape_shader_eval_fn_t eval;
    size_t uniform_size;
    uint32_t flags;
} grape_shader_program_t;

esp_err_t grape_shader_render_procedural_to_texture(grape_texture_t *target,
                                                     const grape_shader_program_t *shader,
                                                     const void *uniforms);

#ifdef __cplusplus
}
#endif
