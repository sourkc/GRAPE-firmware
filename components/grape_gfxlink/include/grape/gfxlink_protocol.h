#pragma once

#include <stdint.h>

#define GFXLINK_MAGIC 0x50415247u
#define GFXLINK_PROTOCOL_VERSION 5u
#define GFXLINK_MAX_PAYLOAD (16u * 1024u)
#define GFXLINK_MAX_RESOURCE_SIZE (16u * 1024u * 1024u)

#define GFXLINK_USB_VID 0xCAFEu
#define GFXLINK_USB_PID 0x4750u
#define GFXLINK_USB_INTERFACE 0u
#define GFXLINK_USB_EP_OUT 0x01u
#define GFXLINK_USB_EP_IN 0x81u

#define GFXLINK_FLAG_RESPONSE 0x0001u
#define GFXLINK_FLAG_NO_RESPONSE 0x0002u

#define GFXLINK_CAP_SOLID_SURFACE (1u << 0)
#define GFXLINK_CAP_SURFACE_POSITION (1u << 1)
#define GFXLINK_CAP_SURFACE_COLOR (1u << 2)
#define GFXLINK_CAP_SURFACE_DESTROY (1u << 3)
#define GFXLINK_CAP_EXPLICIT_PRESENT (1u << 4)
#define GFXLINK_CAP_RESOURCE_STREAM (1u << 5)
#define GFXLINK_CAP_RELIABLE_RESOURCE_STREAM (1u << 6)
#define GFXLINK_CAP_TEXTURES (1u << 7)
#define GFXLINK_CAP_TEXTURE_UPDATE (1u << 8)
#define GFXLINK_CAP_SURFACE_TEXTURE (1u << 9)
#define GFXLINK_CAP_SURFACE_FULL_CONTROL (1u << 10)
#define GFXLINK_CAP_VECTOR_PATHS (1u << 11)
#define GFXLINK_CAP_SVG (1u << 12)
#define GFXLINK_CAP_FONTS (1u << 13)
#define GFXLINK_CAP_TEXT (1u << 14)

#define GFXLINK_RESOURCE_WRITE_HEADER_SIZE 16u
#define GFXLINK_RESOURCE_CHUNK_SIZE (GFXLINK_MAX_PAYLOAD - GFXLINK_RESOURCE_WRITE_HEADER_SIZE)
#define GFXLINK_RESOURCE_MAX_CHUNKS \
    ((GFXLINK_MAX_RESOURCE_SIZE + GFXLINK_RESOURCE_CHUNK_SIZE - 1u) / GFXLINK_RESOURCE_CHUNK_SIZE)
#define GFXLINK_RESOURCE_BITMAP_BYTES ((GFXLINK_RESOURCE_MAX_CHUNKS + 7u) / 8u)

typedef enum {
    GFXLINK_OP_HELLO = 0x01,
    GFXLINK_OP_GET_INFO = 0x02,
    GFXLINK_OP_PRESENT = 0x03,

    GFXLINK_OP_CREATE_SOLID_SURFACE = 0x10,
    GFXLINK_OP_SET_SURFACE_POSITION = 0x11,
    GFXLINK_OP_SET_SURFACE_COLOR = 0x12,
    GFXLINK_OP_DESTROY_SURFACE = 0x13,
    GFXLINK_OP_CREATE_SURFACE = 0x14,
    GFXLINK_OP_SET_SURFACE_TEXTURE = 0x15,
    GFXLINK_OP_SET_SURFACE_TRANSFORM = 0x16,
    GFXLINK_OP_SET_SURFACE_SCALE = 0x17,
    GFXLINK_OP_SET_SURFACE_ROTATION = 0x18,
    GFXLINK_OP_SET_SURFACE_ORIGIN = 0x19,
    GFXLINK_OP_SET_SURFACE_Z = 0x1A,
    GFXLINK_OP_SET_SURFACE_OPACITY = 0x1B,
    GFXLINK_OP_SET_SURFACE_VISIBLE = 0x1C,

    GFXLINK_OP_RESOURCE_CREATE = 0x20,
    GFXLINK_OP_RESOURCE_WRITE = 0x21,
    GFXLINK_OP_RESOURCE_COMMIT = 0x22,
    GFXLINK_OP_RESOURCE_DESTROY = 0x23,

    GFXLINK_OP_TEXTURE_CREATE = 0x30,
    GFXLINK_OP_TEXTURE_UPDATE = 0x31,
    GFXLINK_OP_TEXTURE_DESTROY = 0x32,

    GFXLINK_OP_PATH_CREATE = 0x40,
    GFXLINK_OP_PATH_RASTERIZE = 0x41,
    GFXLINK_OP_PATH_DESTROY = 0x42,

    GFXLINK_OP_SVG_CREATE = 0x50,
    GFXLINK_OP_SVG_DESTROY = 0x51,

    GFXLINK_OP_FONT_CREATE = 0x60,
    GFXLINK_OP_FONT_DESTROY = 0x61,
    GFXLINK_OP_TEXT_RASTERIZE = 0x62,
} gfxlink_opcode_t;

typedef enum {
    GFXLINK_STATUS_OK = 0,
    GFXLINK_STATUS_INVALID_PACKET = -1,
    GFXLINK_STATUS_UNSUPPORTED = -2,
    GFXLINK_STATUS_INVALID_ARGUMENT = -3,
    GFXLINK_STATUS_NO_MEMORY = -4,
    GFXLINK_STATUS_NOT_FOUND = -5,
    GFXLINK_STATUS_BUSY = -6,
    GFXLINK_STATUS_INTERNAL = -7,
    GFXLINK_STATUS_INCOMPLETE = -8,
    GFXLINK_STATUS_CHECKSUM_MISMATCH = -9,
} gfxlink_status_t;

typedef enum {
    GFXLINK_RESOURCE_GENERIC = 0,
    GFXLINK_RESOURCE_TEXTURE = 1,
    GFXLINK_RESOURCE_VECTOR = 2,
    GFXLINK_RESOURCE_FONT = 3,
    GFXLINK_RESOURCE_SVG = 4,
    GFXLINK_RESOURCE_TEXT = 5,
} gfxlink_resource_kind_t;

typedef enum {
    GFXLINK_PIXEL_FORMAT_RGB565 = 0,
    GFXLINK_PIXEL_FORMAT_RGB888 = 1,
    GFXLINK_PIXEL_FORMAT_A8 = 2,
} gfxlink_pixel_format_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t opcode;
    uint16_t flags;
    uint32_t sequence;
    uint32_t payload_size;
} gfxlink_header_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint8_t protocol_version;
    uint8_t reserved[3];
    uint32_t capabilities;
    uint32_t max_payload;
    uint32_t max_resource_size;
} gfxlink_hello_response_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t pixel_format;
    uint32_t max_surfaces;
    uint32_t max_resources;
    uint32_t max_textures;
    uint32_t max_paths;
    uint32_t max_svg_documents;
    uint32_t max_fonts;
} gfxlink_info_response_t;

typedef struct __attribute__((packed)) {
    uint32_t width;
    uint32_t height;
    uint32_t x_bits;
    uint32_t y_bits;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} gfxlink_create_solid_surface_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
} gfxlink_create_surface_response_t;

typedef struct __attribute__((packed)) {
    uint32_t texture_handle;
} gfxlink_create_surface_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t texture_handle;
} gfxlink_set_surface_texture_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t x_bits;
    uint32_t y_bits;
} gfxlink_set_surface_position_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t x_bits;
    uint32_t y_bits;
    uint32_t scale_x_bits;
    uint32_t scale_y_bits;
    uint32_t rotation_bits;
    uint32_t origin_x_bits;
    uint32_t origin_y_bits;
} gfxlink_set_surface_transform_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t scale_x_bits;
    uint32_t scale_y_bits;
} gfxlink_set_surface_scale_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t rotation_bits;
} gfxlink_set_surface_rotation_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t origin_x_bits;
    uint32_t origin_y_bits;
} gfxlink_set_surface_origin_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    int32_t z;
} gfxlink_set_surface_z_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint8_t opacity;
    uint8_t reserved[3];
} gfxlink_set_surface_opacity_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint8_t visible;
    uint8_t reserved[3];
} gfxlink_set_surface_visible_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} gfxlink_set_surface_color_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_destroy_surface_request_t;

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t total_size;
    uint32_t flags;
} gfxlink_resource_create_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
    uint32_t chunk_size;
    uint32_t chunk_count;
} gfxlink_resource_create_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t chunk_index;
    uint32_t data_size;
    uint32_t crc32;
} gfxlink_resource_write_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t expected_crc32;
} gfxlink_resource_commit_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t chunk_count;
    uint32_t resource_crc32;
    uint8_t missing_bitmap[GFXLINK_RESOURCE_BITMAP_BYTES];
    uint8_t corrupt_bitmap[GFXLINK_RESOURCE_BITMAP_BYTES];
} gfxlink_resource_commit_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_resource_handle_request_t;

typedef struct __attribute__((packed)) {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t resource_handle;
} gfxlink_texture_create_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
    uint32_t stride;
    uint32_t size;
} gfxlink_texture_create_response_t;

typedef struct __attribute__((packed)) {
    uint32_t texture_handle;
    uint32_t resource_handle;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} gfxlink_texture_update_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_texture_handle_request_t;

typedef enum {
    GFXLINK_PATH_MOVE_TO = 1,
    GFXLINK_PATH_LINE_TO = 2,
    GFXLINK_PATH_QUAD_TO = 3,
    GFXLINK_PATH_CUBIC_TO = 4,
    GFXLINK_PATH_ARC_TO = 5,
    GFXLINK_PATH_CLOSE = 6,
} gfxlink_path_command_type_t;

#define GFXLINK_PATH_FLAG_LARGE_ARC 0x01u
#define GFXLINK_PATH_FLAG_SWEEP 0x02u
#define GFXLINK_PATH_COMMAND_VALUE_COUNT 6u

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    uint16_t reserved;
    uint32_t values[GFXLINK_PATH_COMMAND_VALUE_COUNT];
} gfxlink_path_command_t;

typedef struct __attribute__((packed)) {
    uint32_t resource_handle;
    uint32_t command_count;
} gfxlink_path_create_request_t;

typedef struct __attribute__((packed)) {
    uint32_t path_handle;
    uint32_t pixels_per_unit_bits;
    uint8_t samples_per_axis;
    uint8_t reserved[3];
    uint32_t padding_pixels;
} gfxlink_path_rasterize_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t texture_handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t origin_x_bits;
    uint32_t origin_y_bits;
    uint32_t pixels_per_unit_bits;
} gfxlink_raster_texture_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_path_handle_request_t;

typedef struct __attribute__((packed)) {
    uint32_t resource_handle;
    uint32_t x_bits;
    uint32_t y_bits;
    uint32_t width_bits;
    uint32_t height_bits;
    uint8_t samples_per_axis;
    uint8_t reserved[3];
    uint32_t padding_pixels;
    int32_t z_base;
} gfxlink_svg_create_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
    uint32_t layer_count;
    uint32_t view_box_min_x_bits;
    uint32_t view_box_min_y_bits;
    uint32_t view_box_width_bits;
    uint32_t view_box_height_bits;
} gfxlink_svg_create_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_svg_handle_request_t;

typedef struct __attribute__((packed)) {
    uint32_t resource_handle;
} gfxlink_font_create_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
    uint32_t units_per_em;
    uint32_t glyph_count;
    uint32_t cmap_format;
    int32_t ascender;
    int32_t descender;
    int32_t line_gap;
} gfxlink_font_create_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_font_handle_request_t;

typedef struct __attribute__((packed)) {
    uint32_t font_handle;
    uint32_t text_resource_handle;
    uint32_t pixels_per_unit_bits;
    uint8_t samples_per_axis;
    uint8_t reserved[3];
    uint32_t padding_pixels;
} gfxlink_text_rasterize_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t texture_handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t origin_x_bits;
    uint32_t origin_y_bits;
    uint32_t pixels_per_unit_bits;
    uint32_t advance_width_bits;
    uint32_t glyph_count;
} gfxlink_text_rasterize_response_t;

typedef struct __attribute__((packed)) {
    int32_t status;
} gfxlink_status_response_t;

_Static_assert(sizeof(gfxlink_header_t) == 16, "GFXLINK header must be 16 bytes");
_Static_assert(sizeof(gfxlink_resource_write_request_t) == GFXLINK_RESOURCE_WRITE_HEADER_SIZE,
               "GFXLINK resource write header must match protocol constant");
_Static_assert(sizeof(gfxlink_path_command_t) == 28u,
               "GFXLINK path command must remain fixed-size");
_Static_assert(GFXLINK_RESOURCE_CHUNK_SIZE > 0u, "GFXLINK resource chunk size must be positive");
