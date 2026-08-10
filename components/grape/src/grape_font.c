#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "grape/grape_font.h"

typedef struct {
    uint32_t offset;
    uint32_t length;
} grape_font_table_t;

typedef struct {
    int32_t x;
    int32_t y;
    bool on_curve;
} grape_font_point_t;

struct grape_font {
    const uint8_t *data;
    size_t size;
    grape_font_table_t head;
    grape_font_table_t maxp;
    grape_font_table_t loca;
    grape_font_table_t glyf;
    grape_font_table_t cmap;
    grape_font_table_t cmap_subtable;
    uint16_t units_per_em;
    uint16_t glyph_count;
    int16_t loca_format;
    uint16_t cmap_format;
};

#define TTF_TAG(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

#define TTF_FLAG_ON_CURVE 0x01U
#define TTF_FLAG_X_SHORT 0x02U
#define TTF_FLAG_Y_SHORT 0x04U
#define TTF_FLAG_REPEAT 0x08U
#define TTF_FLAG_X_SAME_OR_POSITIVE 0x10U
#define TTF_FLAG_Y_SAME_OR_POSITIVE 0x20U

static bool range_valid(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static bool read_u16(const uint8_t *data, size_t size, size_t offset, uint16_t *out)
{
    if (!out || !range_valid(size, offset, 2U)) {
        return false;
    }

    *out = ((uint16_t)data[offset] << 8) | data[offset + 1U];
    return true;
}

static bool read_i16(const uint8_t *data, size_t size, size_t offset, int16_t *out)
{
    uint16_t value = 0;
    if (!out || !read_u16(data, size, offset, &value)) {
        return false;
    }

    *out = (int16_t)value;
    return true;
}

static bool read_u32(const uint8_t *data, size_t size, size_t offset, uint32_t *out)
{
    if (!out || !range_valid(size, offset, 4U)) {
        return false;
    }

    *out = ((uint32_t)data[offset] << 24) |
           ((uint32_t)data[offset + 1U] << 16) |
           ((uint32_t)data[offset + 2U] << 8) |
           data[offset + 3U];
    return true;
}

static bool table_range_valid(const grape_font_t *font, grape_font_table_t table)
{
    return range_valid(font->size, table.offset, table.length);
}

static esp_err_t find_tables(grape_font_t *font)
{
    uint32_t scaler = 0;
    uint16_t table_count = 0;
    if (!read_u32(font->data, font->size, 0U, &scaler) ||
        !read_u16(font->data, font->size, 4U, &table_count)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (scaler != 0x00010000U && scaler != TTF_TAG('t', 'r', 'u', 'e')) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!range_valid(font->size, 12U, (size_t)table_count * 16U)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint16_t i = 0; i < table_count; ++i) {
        size_t record = 12U + (size_t)i * 16U;
        uint32_t tag = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!read_u32(font->data, font->size, record, &tag) ||
            !read_u32(font->data, font->size, record + 8U, &offset) ||
            !read_u32(font->data, font->size, record + 12U, &length)) {
            return ESP_ERR_INVALID_SIZE;
        }

        grape_font_table_t table = {
            .offset = offset,
            .length = length,
        };
        if (!table_range_valid(font, table)) {
            return ESP_ERR_INVALID_SIZE;
        }

        switch (tag) {
            case TTF_TAG('h', 'e', 'a', 'd'):
                font->head = table;
                break;
            case TTF_TAG('m', 'a', 'x', 'p'):
                font->maxp = table;
                break;
            case TTF_TAG('l', 'o', 'c', 'a'):
                font->loca = table;
                break;
            case TTF_TAG('g', 'l', 'y', 'f'):
                font->glyf = table;
                break;
            case TTF_TAG('c', 'm', 'a', 'p'):
                font->cmap = table;
                break;
            default:
                break;
        }
    }

    if (font->head.length == 0U || font->maxp.length == 0U ||
        font->loca.length == 0U || font->glyf.length == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static esp_err_t parse_metadata(grape_font_t *font)
{
    if (font->head.length < 54U || font->maxp.length < 6U) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!read_u16(font->data, font->size, (size_t)font->head.offset + 18U,
                  &font->units_per_em) ||
        !read_i16(font->data, font->size, (size_t)font->head.offset + 50U,
                  &font->loca_format) ||
        !read_u16(font->data, font->size, (size_t)font->maxp.offset + 4U,
                  &font->glyph_count)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (font->units_per_em == 0U || font->glyph_count == 0U ||
        (font->loca_format != 0 && font->loca_format != 1)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t loca_entries = (size_t)font->glyph_count + 1U;
    size_t loca_stride = font->loca_format == 0 ? 2U : 4U;
    if (loca_entries > SIZE_MAX / loca_stride ||
        loca_entries * loca_stride > font->loca.length) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static int cmap_candidate_rank(uint16_t platform_id,
                               uint16_t encoding_id,
                               uint16_t format)
{
    int platform_rank = 0;
    if (platform_id == 0U) {
        platform_rank = 1;
    } else if (platform_id == 3U &&
               ((format == 4U && encoding_id == 1U) ||
                (format == 12U && encoding_id == 10U))) {
        platform_rank = 2;
    } else {
        return 0;
    }

    if (format == 12U) {
        return 100 + platform_rank;
    }
    if (format == 4U) {
        return 50 + platform_rank;
    }
    return 0;
}

static esp_err_t validate_cmap_format4(const grape_font_t *font,
                                       grape_font_table_t subtable)
{
    if (subtable.length < 16U) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t seg_count_x2 = 0;
    if (!read_u16(font->data, font->size, (size_t)subtable.offset + 6U,
                  &seg_count_x2) ||
        seg_count_x2 == 0U || (seg_count_x2 & 1U) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t seg_count = (size_t)seg_count_x2 / 2U;
    uint64_t required = 16ULL + (uint64_t)seg_count * 8ULL;
    if (required > subtable.length) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t end_codes = (size_t)subtable.offset + 14U;
    size_t start_codes = end_codes + seg_count * 2U + 2U;
    uint16_t previous_end = 0U;
    for (size_t i = 0; i < seg_count; ++i) {
        uint16_t start = 0;
        uint16_t end = 0;
        if (!read_u16(font->data, font->size, end_codes + i * 2U, &end) ||
            !read_u16(font->data, font->size, start_codes + i * 2U, &start) ||
            start > end || (i > 0U && start <= previous_end)) {
            return ESP_ERR_INVALID_SIZE;
        }
        previous_end = end;
    }

    return ESP_OK;
}

static esp_err_t validate_cmap_format12(const grape_font_t *font,
                                        grape_font_table_t subtable)
{
    if (subtable.length < 16U) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t group_count = 0;
    if (!read_u32(font->data, font->size, (size_t)subtable.offset + 12U,
                  &group_count)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t required = 16ULL + (uint64_t)group_count * 12ULL;
    if (required > subtable.length) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t previous_end = 0;
    for (uint32_t i = 0; i < group_count; ++i) {
        size_t group = (size_t)subtable.offset + 16U + (size_t)i * 12U;
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t start_glyph = 0;
        if (!read_u32(font->data, font->size, group, &start) ||
            !read_u32(font->data, font->size, group + 4U, &end) ||
            !read_u32(font->data, font->size, group + 8U, &start_glyph) ||
            start > end || end > 0x10ffffU ||
            (i > 0U && start <= previous_end)) {
            return ESP_ERR_INVALID_SIZE;
        }

        uint64_t last_glyph = (uint64_t)start_glyph + (uint64_t)(end - start);
        if (last_glyph >= font->glyph_count) {
            return ESP_ERR_INVALID_SIZE;
        }
        previous_end = end;
    }

    return ESP_OK;
}

static esp_err_t select_cmap(grape_font_t *font)
{
    if (font->cmap.length == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    if (font->cmap.length < 4U) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t version = 0;
    uint16_t record_count = 0;
    if (!read_u16(font->data, font->size, font->cmap.offset, &version) ||
        !read_u16(font->data, font->size, (size_t)font->cmap.offset + 2U,
                  &record_count) ||
        version != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t records_size = 4ULL + (uint64_t)record_count * 8ULL;
    if (records_size > font->cmap.length) {
        return ESP_ERR_INVALID_SIZE;
    }

    int best_rank = 0;
    grape_font_table_t best = {0};
    uint16_t best_format = 0;

    for (uint16_t i = 0; i < record_count; ++i) {
        size_t record = (size_t)font->cmap.offset + 4U + (size_t)i * 8U;
        uint16_t platform_id = 0;
        uint16_t encoding_id = 0;
        uint32_t relative_offset = 0;
        if (!read_u16(font->data, font->size, record, &platform_id) ||
            !read_u16(font->data, font->size, record + 2U, &encoding_id) ||
            !read_u32(font->data, font->size, record + 4U, &relative_offset)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (relative_offset > font->cmap.length - 2U) {
            return ESP_ERR_INVALID_SIZE;
        }

        uint64_t absolute_offset = (uint64_t)font->cmap.offset + relative_offset;
        if (absolute_offset > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t offset = (size_t)absolute_offset;
        uint16_t format = 0;
        if (!read_u16(font->data, font->size, offset, &format)) {
            return ESP_ERR_INVALID_SIZE;
        }

        int rank = cmap_candidate_rank(platform_id, encoding_id, format);
        if (rank <= best_rank) {
            continue;
        }

        uint32_t length = 0;
        if (format == 4U) {
            uint16_t short_length = 0;
            if (!read_u16(font->data, font->size, offset + 2U, &short_length)) {
                return ESP_ERR_INVALID_SIZE;
            }
            length = short_length;
        } else if (format == 12U) {
            if (!read_u32(font->data, font->size, offset + 4U, &length)) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else {
            continue;
        }

        if (length == 0U || length > font->cmap.length ||
            relative_offset > font->cmap.length - length) {
            return ESP_ERR_INVALID_SIZE;
        }

        grape_font_table_t candidate = {
            .offset = (uint32_t)offset,
            .length = length,
        };
        esp_err_t ret = format == 4U
            ? validate_cmap_format4(font, candidate)
            : validate_cmap_format12(font, candidate);
        if (ret != ESP_OK) {
            return ret;
        }

        best = candidate;
        best_format = format;
        best_rank = rank;
    }

    if (best_rank == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    font->cmap_subtable = best;
    font->cmap_format = best_format;
    return ESP_OK;
}

static esp_err_t lookup_cmap_format12(const grape_font_t *font,
                                      uint32_t codepoint,
                                      uint16_t *out_glyph_id)
{
    uint32_t group_count = 0;
    if (!read_u32(font->data, font->size,
                  (size_t)font->cmap_subtable.offset + 12U, &group_count)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t left = 0;
    uint32_t right = group_count;
    while (left < right) {
        uint32_t mid = left + (right - left) / 2U;
        size_t group = (size_t)font->cmap_subtable.offset +
                       16U + (size_t)mid * 12U;
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t start_glyph = 0;
        if (!read_u32(font->data, font->size, group, &start) ||
            !read_u32(font->data, font->size, group + 4U, &end) ||
            !read_u32(font->data, font->size, group + 8U, &start_glyph)) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (codepoint < start) {
            right = mid;
        } else if (codepoint > end) {
            left = mid + 1U;
        } else {
            uint32_t glyph_id = start_glyph + (codepoint - start);
            if (glyph_id == 0U) {
                return ESP_ERR_NOT_FOUND;
            }
            if (glyph_id >= font->glyph_count || glyph_id > UINT16_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            *out_glyph_id = (uint16_t)glyph_id;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t lookup_cmap_format4(const grape_font_t *font,
                                     uint32_t codepoint,
                                     uint16_t *out_glyph_id)
{
    if (codepoint > UINT16_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t seg_count_x2 = 0;
    if (!read_u16(font->data, font->size,
                  (size_t)font->cmap_subtable.offset + 6U, &seg_count_x2)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t seg_count = (size_t)seg_count_x2 / 2U;
    size_t end_codes = (size_t)font->cmap_subtable.offset + 14U;
    size_t start_codes = end_codes + seg_count * 2U + 2U;
    size_t deltas = start_codes + seg_count * 2U;
    size_t range_offsets = deltas + seg_count * 2U;

    size_t left = 0U;
    size_t right = seg_count;
    while (left < right) {
        size_t mid = left + (right - left) / 2U;
        uint16_t end = 0;
        if (!read_u16(font->data, font->size, end_codes + mid * 2U, &end)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (codepoint > end) {
            left = mid + 1U;
        } else {
            right = mid;
        }
    }

    if (left >= seg_count) {
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t start = 0;
    uint16_t end = 0;
    int16_t delta = 0;
    uint16_t range_offset = 0;
    if (!read_u16(font->data, font->size, start_codes + left * 2U, &start) ||
        !read_u16(font->data, font->size, end_codes + left * 2U, &end) ||
        !read_i16(font->data, font->size, deltas + left * 2U, &delta) ||
        !read_u16(font->data, font->size, range_offsets + left * 2U,
                  &range_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (codepoint < start || codepoint > end) {
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t glyph_id;
    if (range_offset == 0U) {
        glyph_id = (uint16_t)((int32_t)(uint16_t)codepoint + delta);
    } else {
        size_t range_word = range_offsets + left * 2U;
        size_t character_delta = (size_t)((uint16_t)codepoint - start) * 2U;
        if ((size_t)range_offset > SIZE_MAX - range_word ||
            character_delta > SIZE_MAX - range_word - (size_t)range_offset) {
            return ESP_ERR_INVALID_SIZE;
        }

        size_t glyph_offset_value = range_word + (size_t)range_offset +
                                    character_delta;
        size_t subtable_end = (size_t)font->cmap_subtable.offset +
                              font->cmap_subtable.length;
        if (!range_valid(subtable_end, glyph_offset_value, 2U) ||
            !read_u16(font->data, font->size, glyph_offset_value, &glyph_id)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (glyph_id != 0U) {
            glyph_id = (uint16_t)((int32_t)glyph_id + delta);
        }
    }

    if (glyph_id == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    if (glyph_id >= font->glyph_count) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_glyph_id = glyph_id;
    return ESP_OK;
}

static esp_err_t glyph_offset(const grape_font_t *font,
                              uint16_t glyph_id,
                              uint32_t *out_offset)
{
    if (!font || !out_offset || glyph_id > font->glyph_count) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t entry_offset;
    uint32_t offset = 0;
    if (font->loca_format == 0) {
        entry_offset = (size_t)font->loca.offset + (size_t)glyph_id * 2U;
        uint16_t short_offset = 0;
        if (!read_u16(font->data, font->size, entry_offset, &short_offset)) {
            return ESP_ERR_INVALID_SIZE;
        }
        offset = (uint32_t)short_offset * 2U;
    } else {
        entry_offset = (size_t)font->loca.offset + (size_t)glyph_id * 4U;
        if (!read_u32(font->data, font->size, entry_offset, &offset)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (offset > font->glyf.length) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_offset = offset;
    return ESP_OK;
}

static esp_err_t glyph_range(const grape_font_t *font,
                             uint16_t glyph_id,
                             size_t *out_offset,
                             size_t *out_length)
{
    if (!font || !out_offset || !out_length || glyph_id >= font->glyph_count) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t start = 0;
    uint32_t end = 0;
    esp_err_t ret = glyph_offset(font, glyph_id, &start);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = glyph_offset(font, (uint16_t)(glyph_id + 1U), &end);
    if (ret != ESP_OK) {
        return ret;
    }

    if (end < start || end > font->glyf.length) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_offset = (size_t)font->glyf.offset + start;
    *out_length = (size_t)(end - start);
    return ESP_OK;
}

static esp_err_t read_glyph_info(const grape_font_t *font,
                                 uint16_t glyph_id,
                                 grape_font_glyph_info_t *out_info,
                                 size_t *out_offset,
                                 size_t *out_length)
{
    size_t offset = 0;
    size_t length = 0;
    esp_err_t ret = glyph_range(font, glyph_id, &offset, &length);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_font_glyph_info_t info = {0};
    if (length == 0U) {
        info.kind = GRAPE_FONT_GLYPH_EMPTY;
    } else {
        if (length < 10U ||
            !read_i16(font->data, font->size, offset, &info.contour_count) ||
            !read_i16(font->data, font->size, offset + 2U, &info.x_min) ||
            !read_i16(font->data, font->size, offset + 4U, &info.y_min) ||
            !read_i16(font->data, font->size, offset + 6U, &info.x_max) ||
            !read_i16(font->data, font->size, offset + 8U, &info.y_max)) {
            return ESP_ERR_INVALID_SIZE;
        }

        info.kind = info.contour_count < 0
            ? GRAPE_FONT_GLYPH_COMPOSITE
            : GRAPE_FONT_GLYPH_SIMPLE;
    }

    if (out_info) {
        *out_info = info;
    }
    if (out_offset) {
        *out_offset = offset;
    }
    if (out_length) {
        *out_length = length;
    }
    return ESP_OK;
}

static esp_err_t expand_flags(const uint8_t *data,
                              size_t size,
                              size_t *cursor,
                              uint8_t *flags,
                              size_t point_count)
{
    size_t point = 0;
    while (point < point_count) {
        if (!range_valid(size, *cursor, 1U)) {
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t flag = data[(*cursor)++];
        flags[point++] = flag;

        if (flag & TTF_FLAG_REPEAT) {
            if (!range_valid(size, *cursor, 1U)) {
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t repeat = data[(*cursor)++];
            if ((size_t)repeat > point_count - point) {
                return ESP_ERR_INVALID_SIZE;
            }

            for (uint8_t i = 0; i < repeat; ++i) {
                flags[point++] = flag;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t add_delta(int32_t *value, int32_t delta)
{
    int64_t result = (int64_t)*value + delta;
    if (result < INT32_MIN || result > INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *value = (int32_t)result;
    return ESP_OK;
}

static esp_err_t decode_axis(const uint8_t *data,
                             size_t size,
                             size_t *cursor,
                             const uint8_t *flags,
                             grape_font_point_t *points,
                             size_t point_count,
                             bool x_axis)
{
    int32_t value = 0;
    const uint8_t short_mask = x_axis ? TTF_FLAG_X_SHORT : TTF_FLAG_Y_SHORT;
    const uint8_t same_mask = x_axis
        ? TTF_FLAG_X_SAME_OR_POSITIVE
        : TTF_FLAG_Y_SAME_OR_POSITIVE;

    for (size_t i = 0; i < point_count; ++i) {
        uint8_t flag = flags[i];
        int32_t delta = 0;

        if (flag & short_mask) {
            if (!range_valid(size, *cursor, 1U)) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint8_t magnitude = data[(*cursor)++];
            delta = (flag & same_mask) ? (int32_t)magnitude : -(int32_t)magnitude;
        } else if (!(flag & same_mask)) {
            int16_t long_delta = 0;
            if (!read_i16(data, size, *cursor, &long_delta)) {
                return ESP_ERR_INVALID_SIZE;
            }
            *cursor += 2U;
            delta = long_delta;
        }

        esp_err_t ret = add_delta(&value, delta);
        if (ret != ESP_OK) {
            return ret;
        }

        if (x_axis) {
            points[i].x = value;
        } else {
            points[i].y = value;
        }
    }

    return ESP_OK;
}

static float path_x(const grape_font_point_t *point)
{
    return (float)point->x;
}

static float path_y(const grape_font_point_t *point)
{
    return -(float)point->y;
}

static esp_err_t append_contour(grape_path_t *path,
                                const grape_font_point_t *points,
                                size_t count)
{
    if (count == 0U) {
        return ESP_OK;
    }

    const grape_font_point_t *first = &points[0];
    const grape_font_point_t *last = &points[count - 1U];
    float start_x;
    float start_y;
    size_t index;
    size_t remaining;

    if (first->on_curve) {
        start_x = path_x(first);
        start_y = path_y(first);
        index = 1U;
        remaining = count - 1U;
    } else if (last->on_curve) {
        start_x = path_x(last);
        start_y = path_y(last);
        index = 0U;
        remaining = count - 1U;
    } else {
        start_x = ((float)last->x + (float)first->x) * 0.5f;
        start_y = -((float)last->y + (float)first->y) * 0.5f;
        index = 0U;
        remaining = count;
    }

    esp_err_t ret = grape_path_move_to(path, start_x, start_y);
    if (ret != ESP_OK) {
        return ret;
    }

    while (remaining > 0U) {
        const grape_font_point_t *point = &points[index];
        if (point->on_curve) {
            ret = grape_path_line_to(path, path_x(point), path_y(point));
            if (ret != ESP_OK) {
                return ret;
            }
            index = (index + 1U) % count;
            remaining--;
            continue;
        }

        float control_x = path_x(point);
        float control_y = path_y(point);
        if (remaining == 1U) {
            ret = grape_path_quad_to(path, control_x, control_y, start_x, start_y);
            if (ret != ESP_OK) {
                return ret;
            }
            index = (index + 1U) % count;
            remaining--;
            continue;
        }

        size_t next_index = (index + 1U) % count;
        const grape_font_point_t *next = &points[next_index];
        if (next->on_curve) {
            ret = grape_path_quad_to(
                path,
                control_x,
                control_y,
                path_x(next),
                path_y(next)
            );
            if (ret != ESP_OK) {
                return ret;
            }
            index = (next_index + 1U) % count;
            remaining -= 2U;
        } else {
            float midpoint_x = ((float)point->x + (float)next->x) * 0.5f;
            float midpoint_y = -((float)point->y + (float)next->y) * 0.5f;
            ret = grape_path_quad_to(
                path,
                control_x,
                control_y,
                midpoint_x,
                midpoint_y
            );
            if (ret != ESP_OK) {
                return ret;
            }
            index = next_index;
            remaining--;
        }
    }

    return grape_path_close(path);
}

esp_err_t grape_font_load_memory(const void *data, size_t size, grape_font_t **out_font)
{
    if (!data || !out_font || size < 12U) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_font = NULL;
    grape_font_t *font = calloc(1, sizeof(*font));
    if (!font) {
        return ESP_ERR_NO_MEM;
    }

    font->data = data;
    font->size = size;

    esp_err_t ret = find_tables(font);
    if (ret == ESP_OK) {
        ret = parse_metadata(font);
    }
    if (ret == ESP_OK) {
        esp_err_t cmap_ret = select_cmap(font);
        if (cmap_ret != ESP_OK && cmap_ret != ESP_ERR_NOT_FOUND) {
            ret = cmap_ret;
        }
    }
    if (ret != ESP_OK) {
        free(font);
        return ret;
    }

    *out_font = font;
    return ESP_OK;
}

esp_err_t grape_font_destroy(grape_font_t *font)
{
    if (!font) {
        return ESP_ERR_INVALID_ARG;
    }

    free(font);
    return ESP_OK;
}

uint16_t grape_font_units_per_em(const grape_font_t *font)
{
    return font ? font->units_per_em : 0U;
}

uint16_t grape_font_glyph_count(const grape_font_t *font)
{
    return font ? font->glyph_count : 0U;
}

uint16_t grape_font_cmap_format(const grape_font_t *font)
{
    return font ? font->cmap_format : 0U;
}

esp_err_t grape_font_get_glyph_id(const grape_font_t *font,
                                  uint32_t codepoint,
                                  uint16_t *out_glyph_id)
{
    if (!font || !out_glyph_id || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (font->cmap_format == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    if (font->cmap_format == 12U) {
        return lookup_cmap_format12(font, codepoint, out_glyph_id);
    }
    if (font->cmap_format == 4U) {
        return lookup_cmap_format4(font, codepoint, out_glyph_id);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t grape_font_get_glyph_info(const grape_font_t *font,
                                    uint16_t glyph_id,
                                    grape_font_glyph_info_t *out_info)
{
    if (!font || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    return read_glyph_info(font, glyph_id, out_info, NULL, NULL);
}

esp_err_t grape_font_get_glyph_path(const grape_font_t *font,
                                    uint16_t glyph_id,
                                    grape_path_t *path)
{
    if (!font || !path || glyph_id >= font->glyph_count) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_font_glyph_info_t info = {0};
    size_t glyph_offset_value = 0;
    size_t glyph_length = 0;
    esp_err_t ret = read_glyph_info(
        font,
        glyph_id,
        &info,
        &glyph_offset_value,
        &glyph_length
    );
    if (ret != ESP_OK) {
        return ret;
    }
    if (info.kind == GRAPE_FONT_GLYPH_EMPTY || info.contour_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (info.kind == GRAPE_FONT_GLYPH_COMPOSITE) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = grape_path_clear(path);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t contour_count = (size_t)info.contour_count;
    if (contour_count > (SIZE_MAX - 10U) / 2U ||
        10U + contour_count * 2U > glyph_length) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t *end_points = malloc(contour_count * sizeof(*end_points));
    if (!end_points) {
        return ESP_ERR_NO_MEM;
    }

    size_t cursor = glyph_offset_value + 10U;
    for (size_t i = 0; i < contour_count; ++i) {
        if (!read_u16(font->data, font->size, cursor, &end_points[i])) {
            free(end_points);
            return ESP_ERR_INVALID_SIZE;
        }
        if (i > 0U && end_points[i] <= end_points[i - 1U]) {
            free(end_points);
            return ESP_ERR_INVALID_SIZE;
        }
        cursor += 2U;
    }

    size_t point_count = (size_t)end_points[contour_count - 1U] + 1U;
    size_t glyph_end = glyph_offset_value + glyph_length;
    uint16_t instruction_length = 0;
    if (!range_valid(glyph_end, cursor, 2U) ||
        !read_u16(font->data, font->size, cursor, &instruction_length)) {
        free(end_points);
        return ESP_ERR_INVALID_SIZE;
    }
    cursor += 2U;

    if (!range_valid(glyph_end, cursor, instruction_length)) {
        free(end_points);
        return ESP_ERR_INVALID_SIZE;
    }
    cursor += instruction_length;

    uint8_t *flags = malloc(point_count);
    grape_font_point_t *points = calloc(point_count, sizeof(*points));
    if (!flags || !points) {
        free(points);
        free(flags);
        free(end_points);
        return ESP_ERR_NO_MEM;
    }

    ret = expand_flags(font->data, glyph_end, &cursor, flags, point_count);
    if (ret == ESP_OK) {
        for (size_t i = 0; i < point_count; ++i) {
            points[i].on_curve = (flags[i] & TTF_FLAG_ON_CURVE) != 0U;
        }
        ret = decode_axis(
            font->data, glyph_end, &cursor, flags, points, point_count, true
        );
    }
    if (ret == ESP_OK) {
        ret = decode_axis(
            font->data, glyph_end, &cursor, flags, points, point_count, false
        );
    }

    if (ret == ESP_OK) {
        size_t contour_start = 0U;
        for (size_t i = 0; i < contour_count; ++i) {
            size_t contour_end = (size_t)end_points[i];
            if (contour_end < contour_start || contour_end >= point_count) {
                ret = ESP_ERR_INVALID_SIZE;
                break;
            }

            ret = append_contour(
                path,
                &points[contour_start],
                contour_end - contour_start + 1U
            );
            if (ret != ESP_OK) {
                break;
            }
            contour_start = contour_end + 1U;
        }
    }

    free(points);
    free(flags);
    free(end_points);

    if (ret != ESP_OK) {
        grape_path_clear(path);
    }
    return ret;
}
