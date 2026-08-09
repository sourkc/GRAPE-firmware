#include <math.h>
#include <stdlib.h>

#include "grape/grape_svg.h"

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
