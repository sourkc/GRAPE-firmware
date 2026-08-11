#pragma once

#include "grape/grape_types.h"

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

typedef struct grape_shader_kernel_args grape_shader_kernel_args_t;

typedef void (*grape_shader_kernel_fn_t)(
    const grape_shader_kernel_args_t *args,
    const void *uniforms
);

enum {
    GRAPE_SHADER_PROGRAM_USES_SOURCE_COLOR = 1U << 0,
};

typedef struct {
    grape_shader_kernel_fn_t kernel;
    size_t uniform_size;
    uint32_t flags;
} grape_shader_program_t;

#ifdef __cplusplus
}
#endif
