#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "expat.h"

#include "grape/grape_svg.h"
#include "grape/grape_telemetry.h"
#include "grape/grape_surface.h"

typedef struct {
    const char *cursor;
} grape_svg_path_parser_t;

static bool svg_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static bool svg_is_command(char c)
{
    switch (c) {
    case 'M': case 'm':
    case 'L': case 'l':
    case 'H': case 'h':
    case 'V': case 'v':
    case 'C': case 'c':
    case 'S': case 's':
    case 'Q': case 'q':
    case 'T': case 't':
    case 'A': case 'a':
    case 'Z': case 'z':
        return true;
    default:
        return false;
    }
}

static bool svg_is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void svg_skip_separators(grape_svg_path_parser_t *parser)
{
    while (svg_is_space(*parser->cursor) || *parser->cursor == ',') {
        parser->cursor++;
    }
}

static esp_err_t svg_parse_number(grape_svg_path_parser_t *parser, float *out_value)
{
    svg_skip_separators(parser);

    char *end = NULL;
    float value = strtof(parser->cursor, &end);
    if (end == parser->cursor || !isfinite(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    parser->cursor = end;
    *out_value = value;
    return ESP_OK;
}

static esp_err_t svg_parse_flag(grape_svg_path_parser_t *parser, bool *out_value)
{
    svg_skip_separators(parser);

    if (*parser->cursor == '0') {
        *out_value = false;
    } else if (*parser->cursor == '1') {
        *out_value = true;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    parser->cursor++;
    return ESP_OK;
}

static esp_err_t svg_execute_command(grape_path_t *path,
                                     grape_svg_path_parser_t *parser,
                                     char command,
                                     char *out_repeat_command)
{
    const bool relative = command >= 'a' && command <= 'z';
    esp_err_t ret;

    switch (command) {
    case 'M':
    case 'm': {
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }

        ret = relative ? grape_path_move_to_relative(path, x, y)
                       : grape_path_move_to(path, x, y);
        if (ret == ESP_OK) {
            *out_repeat_command = relative ? 'l' : 'L';
        }
        return ret;
    }

    case 'L':
    case 'l': {
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_line_to_relative(path, x, y)
                        : grape_path_line_to(path, x, y);
    }

    case 'H':
    case 'h': {
        float x;
        if ((ret = svg_parse_number(parser, &x)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_horizontal_to_relative(path, x)
                        : grape_path_horizontal_to(path, x);
    }

    case 'V':
    case 'v': {
        float y;
        if ((ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_vertical_to_relative(path, y)
                        : grape_path_vertical_to(path, y);
    }

    case 'Q':
    case 'q': {
        float control_x;
        float control_y;
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &control_x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &control_y)) != ESP_OK ||
            (ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_quad_to_relative(path, control_x, control_y, x, y)
                        : grape_path_quad_to(path, control_x, control_y, x, y);
    }

    case 'T':
    case 't': {
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_smooth_quad_to_relative(path, x, y)
                        : grape_path_smooth_quad_to(path, x, y);
    }

    case 'C':
    case 'c': {
        float control1_x;
        float control1_y;
        float control2_x;
        float control2_y;
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &control1_x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &control1_y)) != ESP_OK ||
            (ret = svg_parse_number(parser, &control2_x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &control2_y)) != ESP_OK ||
            (ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_cubic_to_relative(
                              path,
                              control1_x, control1_y,
                              control2_x, control2_y,
                              x, y
                          )
                        : grape_path_cubic_to(
                              path,
                              control1_x, control1_y,
                              control2_x, control2_y,
                              x, y
                          );
    }

    case 'S':
    case 's': {
        float control2_x;
        float control2_y;
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &control2_x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &control2_y)) != ESP_OK ||
            (ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_smooth_cubic_to_relative(
                              path, control2_x, control2_y, x, y
                          )
                        : grape_path_smooth_cubic_to(
                              path, control2_x, control2_y, x, y
                          );
    }

    case 'A':
    case 'a': {
        float radius_x;
        float radius_y;
        float rotation;
        bool large_arc;
        bool sweep;
        float x;
        float y;
        if ((ret = svg_parse_number(parser, &radius_x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &radius_y)) != ESP_OK ||
            (ret = svg_parse_number(parser, &rotation)) != ESP_OK ||
            (ret = svg_parse_flag(parser, &large_arc)) != ESP_OK ||
            (ret = svg_parse_flag(parser, &sweep)) != ESP_OK ||
            (ret = svg_parse_number(parser, &x)) != ESP_OK ||
            (ret = svg_parse_number(parser, &y)) != ESP_OK) {
            return ret;
        }
        return relative ? grape_path_arc_to_relative(
                              path,
                              radius_x, radius_y, rotation,
                              large_arc, sweep,
                              x, y
                          )
                        : grape_path_arc_to(
                              path,
                              radius_x, radius_y, rotation,
                              large_arc, sweep,
                              x, y
                          );
    }

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t grape_svg_parse_path_data(grape_path_t *path, const char *data)
{
    GRAPE_TIME_SCOPE(SVG_PATH_PARSE);
    if (!path || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_svg_path_parser_t parser = {
        .cursor = data,
    };
    char repeat_command = '\0';
    bool parsed_any = false;

    for (;;) {
        svg_skip_separators(&parser);
        if (*parser.cursor == '\0') {
            return parsed_any ? ESP_OK : ESP_ERR_INVALID_ARG;
        }

        char command;
        if (svg_is_command(*parser.cursor)) {
            command = *parser.cursor++;
            if (command == 'Z' || command == 'z') {
                esp_err_t ret = grape_path_close(path);
                if (ret != ESP_OK) {
                    return ret;
                }
                repeat_command = '\0';
                parsed_any = true;
                continue;
            }
            repeat_command = command;
        } else {
            if (svg_is_alpha(*parser.cursor) || repeat_command == '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            command = repeat_command;
        }

        esp_err_t ret = svg_execute_command(path, &parser, command, &repeat_command);
        if (ret != ESP_OK) {
            return ret;
        }
        parsed_any = true;
    }
}


typedef struct {
    grape_texture_t *texture;
    grape_surface_t *surface;
} grape_svg_layer_t;

struct grape_svg_document {
    grape_context_t *context;
    grape_svg_layer_t *layers;
    size_t layer_count;
    size_t layer_capacity;
    grape_svg_view_box_t view_box;
};

typedef struct {
    grape_svg_document_t *document;
    grape_svg_document_config_t config;
    XML_Parser parser;
    esp_err_t error;
    bool root_seen;
    bool view_box_ready;
    float scale;
    float output_origin_x;
    float output_origin_y;
} grape_svg_xml_state_t;

static const char *svg_xml_attr(const XML_Char **attributes, const char *name)
{
    if (!attributes || !name) {
        return NULL;
    }

    for (size_t i = 0; attributes[i] && attributes[i + 1U]; i += 2U) {
        if (strcmp(attributes[i], name) == 0) {
            return attributes[i + 1U];
        }
    }
    return NULL;
}

static bool svg_parse_view_box(const char *text, grape_svg_view_box_t *out_view_box)
{
    if (!text || !out_view_box) {
        return false;
    }

    grape_svg_path_parser_t parser = { .cursor = text };
    float values[4];
    for (size_t i = 0; i < 4U; ++i) {
        if (svg_parse_number(&parser, &values[i]) != ESP_OK) {
            return false;
        }
    }

    svg_skip_separators(&parser);
    if (*parser.cursor != '\0' || values[2] <= 0.0f || values[3] <= 0.0f) {
        return false;
    }

    *out_view_box = (grape_svg_view_box_t){
        .min_x = values[0],
        .min_y = values[1],
        .width = values[2],
        .height = values[3],
    };
    return true;
}

static int svg_hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool svg_hex_byte(const char *text, uint8_t *out_value)
{
    int high = svg_hex_digit(text[0]);
    int low = svg_hex_digit(text[1]);
    if (high < 0 || low < 0) {
        return false;
    }
    *out_value = (uint8_t)((high << 4) | low);
    return true;
}

static esp_err_t svg_parse_fill(const char *fill, grape_color_t *out_color, bool *out_visible)
{
    if (!out_color || !out_visible) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_visible = true;
    *out_color = (grape_color_t){ .r = 0, .g = 0, .b = 0, .a = 255 };

    if (!fill || fill[0] == '\0' || strcmp(fill, "black") == 0) {
        return ESP_OK;
    }
    if (strcmp(fill, "none") == 0) {
        *out_visible = false;
        return ESP_OK;
    }
    if (strcmp(fill, "white") == 0) {
        *out_color = (grape_color_t){ .r = 255, .g = 255, .b = 255, .a = 255 };
        return ESP_OK;
    }
    if (fill[0] != '#') {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t length = strlen(fill + 1);
    if (length == 3U || length == 4U) {
        int r = svg_hex_digit(fill[1]);
        int g = svg_hex_digit(fill[2]);
        int b = svg_hex_digit(fill[3]);
        int a = length == 4U ? svg_hex_digit(fill[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        *out_color = (grape_color_t){
            .r = (uint8_t)(r * 17),
            .g = (uint8_t)(g * 17),
            .b = (uint8_t)(b * 17),
            .a = (uint8_t)(a * 17),
        };
        return ESP_OK;
    }

    if (length == 6U || length == 8U) {
        grape_color_t color = { .a = 255 };
        if (!svg_hex_byte(fill + 1, &color.r) ||
            !svg_hex_byte(fill + 3, &color.g) ||
            !svg_hex_byte(fill + 5, &color.b) ||
            (length == 8U && !svg_hex_byte(fill + 7, &color.a))) {
            return ESP_ERR_INVALID_ARG;
        }
        *out_color = color;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t svg_document_reserve(grape_svg_document_t *document, size_t required)
{
    if (required <= document->layer_capacity) {
        return ESP_OK;
    }

    size_t capacity = document->layer_capacity ? document->layer_capacity : 8U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*document->layers)) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_svg_layer_t *layers = realloc(document->layers, capacity * sizeof(*layers));
    if (!layers) {
        return ESP_ERR_NO_MEM;
    }

    document->layers = layers;
    document->layer_capacity = capacity;
    return ESP_OK;
}

static void svg_xml_fail(grape_svg_xml_state_t *state, esp_err_t error)
{
    if (state->error == ESP_OK) {
        state->error = error;
        XML_StopParser(state->parser, XML_FALSE);
    }
}

static esp_err_t svg_xml_configure_view_box(grape_svg_xml_state_t *state,
                                             const XML_Char **attributes)
{
    const char *view_box_text = svg_xml_attr(attributes, "viewBox");
    if (!view_box_text || !svg_parse_view_box(view_box_text, &state->document->view_box)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    float target_width = state->config.width > 0.0f
                       ? state->config.width
                       : state->document->view_box.width;
    float target_height = state->config.height > 0.0f
                        ? state->config.height
                        : state->document->view_box.height;
    float scale_x = target_width / state->document->view_box.width;
    float scale_y = target_height / state->document->view_box.height;
    state->scale = fminf(scale_x, scale_y);
    if (!isfinite(state->scale) || state->scale <= 0.0f) {
        return ESP_ERR_INVALID_SIZE;
    }

    float rendered_width = state->document->view_box.width * state->scale;
    float rendered_height = state->document->view_box.height * state->scale;
    state->output_origin_x = state->config.x + (target_width - rendered_width) * 0.5f;
    state->output_origin_y = state->config.y + (target_height - rendered_height) * 0.5f;
    state->view_box_ready = true;
    return ESP_OK;
}

static esp_err_t svg_xml_add_path(grape_svg_xml_state_t *state, const XML_Char **attributes)
{
    const char *path_data = svg_xml_attr(attributes, "d");
    if (!path_data || path_data[0] == '\0') {
        return ESP_OK;
    }

    grape_color_t fill;
    bool visible;
    esp_err_t ret = svg_parse_fill(svg_xml_attr(attributes, "fill"), &fill, &visible);
    if (ret != ESP_OK || !visible) {
        return ret;
    }

    grape_path_t *path = NULL;
    grape_path_raster_t raster = {0};
    grape_surface_t *surface = NULL;

    ret = grape_path_create(&path);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = grape_svg_parse_path_data(path, path_data);
    if (ret != ESP_OK) {
        grape_path_destroy(path);
        return ret;
    }

    grape_path_rasterize_config_t raster_config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    raster_config.pixels_per_unit = state->scale;
    raster_config.samples_per_axis = state->config.samples_per_axis;
    raster_config.padding_pixels = state->config.padding_pixels;
    raster_config.memory = state->config.memory;

    ret = grape_path_rasterize_a8(state->document->context, path, &raster_config, &raster);
    grape_path_destroy(path);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_surface_create(state->document->context, raster.texture, &surface);
    if (ret != ESP_OK) {
        grape_texture_destroy(raster.texture);
        return ret;
    }

    float x = state->output_origin_x +
              (raster.path_origin_x - state->document->view_box.min_x) * state->scale;
    float y = state->output_origin_y +
              (raster.path_origin_y - state->document->view_box.min_y) * state->scale;

    ret = grape_surface_set_position(surface, x, y);
    if (ret == ESP_OK) {
        ret = grape_surface_set_tint(surface, fill);
    }
    if (ret == ESP_OK) {
        int64_t z = (int64_t)state->config.z_base +
                    (int64_t)state->document->layer_count;
        if (z > INT32_MAX) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            ret = grape_surface_set_z(surface, (int32_t)z);
        }
    }
    if (ret != ESP_OK) {
        grape_surface_destroy(surface);
        grape_texture_destroy(raster.texture);
        return ret;
    }

    ret = svg_document_reserve(state->document, state->document->layer_count + 1U);
    if (ret != ESP_OK) {
        grape_surface_destroy(surface);
        grape_texture_destroy(raster.texture);
        return ret;
    }

    state->document->layers[state->document->layer_count++] = (grape_svg_layer_t){
        .texture = raster.texture,
        .surface = surface,
    };
    return ESP_OK;
}

static void XMLCALL svg_xml_start_element(void *user_data,
                                          const XML_Char *name,
                                          const XML_Char **attributes)
{
    grape_svg_xml_state_t *state = user_data;
    if (state->error != ESP_OK) {
        return;
    }

    if (!state->root_seen) {
        state->root_seen = true;
        if (strcmp(name, "svg") != 0) {
            svg_xml_fail(state, ESP_ERR_INVALID_ARG);
            return;
        }

        esp_err_t ret = svg_xml_configure_view_box(state, attributes);
        if (ret != ESP_OK) {
            svg_xml_fail(state, ret);
        }
        return;
    }

    if (strcmp(name, "path") == 0) {
        if (!state->view_box_ready) {
            svg_xml_fail(state, ESP_ERR_INVALID_STATE);
            return;
        }
        esp_err_t ret = svg_xml_add_path(state, attributes);
        if (ret != ESP_OK) {
            svg_xml_fail(state, ret);
        }
    }
}

esp_err_t grape_svg_document_create(grape_context_t *context,
                                    const char *svg_text,
                                    const grape_svg_document_config_t *config,
                                    grape_svg_document_t **out_document)
{
    GRAPE_TIME_SCOPE(SVG_DOCUMENT_CREATE);
    if (!context || !svg_text || !config || !out_document ||
        !isfinite(config->x) || !isfinite(config->y) ||
        !isfinite(config->width) || !isfinite(config->height) ||
        config->width < 0.0f || config->height < 0.0f ||
        config->samples_per_axis == 0U || config->samples_per_axis > 8U) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t svg_length = strlen(svg_text);
    if (svg_length > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_document = NULL;
    grape_svg_document_t *document = calloc(1, sizeof(*document));
    if (!document) {
        return ESP_ERR_NO_MEM;
    }
    document->context = context;

    XML_Parser parser = XML_ParserCreate(NULL);
    if (!parser) {
        free(document);
        return ESP_ERR_NO_MEM;
    }

    grape_svg_xml_state_t state = {
        .document = document,
        .config = *config,
        .parser = parser,
        .error = ESP_OK,
    };
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, svg_xml_start_element, NULL);

    enum XML_Status status = XML_Parse(parser, svg_text, (int)svg_length, XML_TRUE);
    esp_err_t ret = state.error;
    if (ret == ESP_OK && status != XML_STATUS_OK) {
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK && (!state.root_seen || !state.view_box_ready)) {
        ret = ESP_ERR_INVALID_ARG;
    }

    XML_ParserFree(parser);
    if (ret != ESP_OK) {
        grape_svg_document_destroy(document);
        return ret;
    }

    *out_document = document;
    return ESP_OK;
}

esp_err_t grape_svg_document_destroy(grape_svg_document_t *document)
{
    if (!document) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_error = ESP_OK;
    for (size_t i = document->layer_count; i > 0U; --i) {
        grape_svg_layer_t *layer = &document->layers[i - 1U];
        if (layer->surface) {
            esp_err_t ret = grape_surface_destroy(layer->surface);
            if (first_error == ESP_OK && ret != ESP_OK) {
                first_error = ret;
            }
        }
        if (layer->texture) {
            esp_err_t ret = grape_texture_destroy(layer->texture);
            if (first_error == ESP_OK && ret != ESP_OK) {
                first_error = ret;
            }
        }
    }

    free(document->layers);
    free(document);
    return first_error;
}

size_t grape_svg_document_layer_count(const grape_svg_document_t *document)
{
    return document ? document->layer_count : 0U;
}

const grape_svg_view_box_t *grape_svg_document_view_box(const grape_svg_document_t *document)
{
    return document ? &document->view_box : NULL;
}
