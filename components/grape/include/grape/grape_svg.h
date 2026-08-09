#pragma once

#include "grape/grape_path.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t grape_svg_parse_path_data(grape_path_t *path, const char *data);

#ifdef __cplusplus
}
#endif
