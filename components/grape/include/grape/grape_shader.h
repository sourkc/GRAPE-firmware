#pragma once

#include "grape/grape_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct {
    grape_shader_kernel_fn_t kernel;
    size_t uniform_size;
} grape_shader_program_t;

#ifdef __cplusplus
}
#endif
