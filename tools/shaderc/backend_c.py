from __future__ import annotations

import re

from .ir import IRBinary, IRConstant, IRLoadBuiltin, IRLoadUniform, IRModule, IRReturn, IRUnary, IRValue
from .shader_types import ShaderType


_C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def validate_shader_name(name: str) -> None:
    if not _C_IDENTIFIER.fullmatch(name):
        raise ValueError(f"shader filename stem '{name}' is not a valid C identifier")


def generate_c(module: IRModule, shader_name: str, banner: str) -> tuple[str, str]:
    validate_shader_name(shader_name)
    prefix = f"generated_{shader_name}"
    guard = f"GRAPE_{prefix.upper()}_H"

    header_lines = [
        banner.rstrip(),
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        '#include "grape/grape_shader.h"',
        "",
        "typedef struct {",
    ]

    if module.uniforms:
        for uniform in module.uniforms:
            header_lines.append(f"    {_c_type(uniform.type)} {uniform.name};")
    else:
        header_lines.append("    unsigned char _unused;")

    header_lines.extend(
        [
            f"}} {prefix}_uniforms_t;",
            "",
            f"extern const grape_shader_program_t {prefix}_program;",
            "",
            f"#endif /* {guard} */",
            "",
        ]
    )

    c_lines = [
        banner.rstrip(),
        "",
        f'#include "{prefix}.h"',
        '#include "grape_shader_runtime.h"',
        "",
    ]
    c_lines.extend(_emit_eval(module, prefix))
    c_lines.append("")

    routes = [
        ("a8", "rgb565", "grape_shader_source_a8", "grape_shader_composite_rgb565", 2),
        ("a8", "rgb888", "grape_shader_source_a8", "grape_shader_composite_rgb888", 3),
        ("rgb565", "rgb565", "grape_shader_source_rgb565", "grape_shader_composite_rgb565", 2),
        ("rgb565", "rgb888", "grape_shader_source_rgb565", "grape_shader_composite_rgb888", 3),
        ("rgb888", "rgb565", "grape_shader_source_rgb888", "grape_shader_composite_rgb565", 2),
        ("rgb888", "rgb888", "grape_shader_source_rgb888", "grape_shader_composite_rgb888", 3),
    ]
    for source_name, target_name, source_fn, composite_fn, bpp in routes:
        c_lines.extend(
            _emit_raster_route(prefix, source_name, target_name, source_fn, composite_fn, bpp)
        )
        c_lines.append("")

    c_lines.extend(_emit_kernel_dispatch(prefix, bool(module.uniforms)))
    c_lines.extend(
        [
            "",
            f"const grape_shader_program_t {prefix}_program = {{",
            f"    .kernel = {prefix}_kernel,",
            f"    .uniform_size = {'sizeof(' + prefix + '_uniforms_t)' if module.uniforms else '0U'},",
            "};",
            "",
        ]
    )

    return "\n".join(c_lines), "\n".join(header_lines)


def _emit_eval(module: IRModule, prefix: str) -> list[str]:
    lines = [
        f"static inline grape_shader_vec4_t {prefix}_eval(",
        "    grape_shader_vec4_t source_color,",
        f"    const {prefix}_uniforms_t *uniforms)",
        "{",
    ]

    uses_source_color = any(isinstance(instruction, IRLoadBuiltin) for instruction in module.main.instructions)
    uses_uniforms = any(isinstance(instruction, IRLoadUniform) for instruction in module.main.instructions)
    if not uses_source_color:
        lines.append("    (void)source_color;")
    if not uses_uniforms:
        lines.append("    (void)uniforms;")

    uniforms_by_index = {uniform.index: uniform for uniform in module.uniforms}
    value_exprs: dict[int, str] = {}

    for instruction in module.main.instructions:
        if isinstance(instruction, IRReturn):
            lines.append(f"    return {value_exprs[instruction.value.id]};")
            continue

        assert instruction.result is not None
        result = instruction.result
        name = f"_v{result.id}"

        if isinstance(instruction, IRConstant):
            expression = _float_literal(instruction.text)
        elif isinstance(instruction, IRLoadBuiltin):
            expression = instruction.name
        elif isinstance(instruction, IRLoadUniform):
            expression = f"uniforms->{uniforms_by_index[instruction.uniform_index].name}"
        elif isinstance(instruction, IRUnary):
            expression = _emit_unary(instruction.operator, instruction.operand, value_exprs)
        elif isinstance(instruction, IRBinary):
            expression = _emit_binary(instruction.operator, instruction.left, instruction.right, value_exprs)
        else:
            raise AssertionError(f"unhandled IR instruction: {type(instruction).__name__}")

        lines.append(f"    const {_c_type(result.type)} {name} = {expression};")
        value_exprs[result.id] = name

    lines.append("}")
    return lines


def _emit_raster_route(
    prefix: str,
    source_name: str,
    target_name: str,
    source_fn: str,
    composite_fn: str,
    bpp: int,
) -> list[str]:
    name = f"{prefix}_raster_{source_name}_to_{target_name}"
    return [
        f"static void {name}(",
        "    const grape_shader_kernel_args_t *args,",
        f"    const {prefix}_uniforms_t *uniforms)",
        "{",
        "    const float texture_width = (float)args->texture_width;",
        "    const float texture_height = (float)args->texture_height;",
        "",
        "    for (int32_t y = args->clipped.y; y < args->clipped.y + args->clipped.height; ++y) {",
        "        float screen_x = (float)args->clipped.x + 0.5f;",
        "        float screen_y = (float)y + 0.5f;",
        "        float local_x = args->local_x_from_screen_x * screen_x +",
        "                        args->local_x_from_screen_y * screen_y +",
        "                        args->local_x_offset;",
        "        float local_y = args->local_y_from_screen_x * screen_x +",
        "                        args->local_y_from_screen_y * screen_y +",
        "                        args->local_y_offset;",
        "        uint8_t *dst = args->target_pixels +",
        "                       (size_t)y * args->target_stride +",
        f"                       (size_t)args->clipped.x * {bpp}U;",
        "",
        "        for (int32_t x = 0; x < args->clipped.width; ++x) {",
        "            if (local_x >= 0.0f && local_y >= 0.0f &&",
        "                local_x < texture_width && local_y < texture_height) {",
        "                int32_t tx = (int32_t)local_x;",
        "                int32_t ty = (int32_t)local_y;",
        f"                grape_shader_vec4_t source_color = {source_fn}(args, tx, ty);",
        f"                grape_shader_vec4_t output = {prefix}_eval(source_color, uniforms);",
        f"                {composite_fn}(args, dst, output);",
        "            }",
        "",
        "            local_x += args->local_x_from_screen_x;",
        "            local_y += args->local_y_from_screen_x;",
        f"            dst += {bpp}U;",
        "        }",
        "    }",
        "}",
    ]


def _emit_kernel_dispatch(prefix: str, has_uniforms: bool) -> list[str]:
    lines = [
        f"static void {prefix}_kernel(",
        "    const grape_shader_kernel_args_t *args,",
        "    const void *uniform_data)",
        "{",
        "    if (!args) {",
        "        return;",
        "    }",
    ]
    if has_uniforms:
        lines.extend(
            [
                "    if (!uniform_data) {",
                "        return;",
                "    }",
            ]
        )
    lines.extend(
        [
            f"    const {prefix}_uniforms_t *uniforms = uniform_data;",
            "",
            "    switch (args->texture_format) {",
            "        case GRAPE_PIXEL_FORMAT_A8:",
            "            if (args->target_format == GRAPE_PIXEL_FORMAT_RGB565) {",
            f"                {prefix}_raster_a8_to_rgb565(args, uniforms);",
            "            } else if (args->target_format == GRAPE_PIXEL_FORMAT_RGB888) {",
            f"                {prefix}_raster_a8_to_rgb888(args, uniforms);",
            "            }",
            "            break;",
            "        case GRAPE_PIXEL_FORMAT_RGB565:",
            "            if (args->target_format == GRAPE_PIXEL_FORMAT_RGB565) {",
            f"                {prefix}_raster_rgb565_to_rgb565(args, uniforms);",
            "            } else if (args->target_format == GRAPE_PIXEL_FORMAT_RGB888) {",
            f"                {prefix}_raster_rgb565_to_rgb888(args, uniforms);",
            "            }",
            "            break;",
            "        case GRAPE_PIXEL_FORMAT_RGB888:",
            "            if (args->target_format == GRAPE_PIXEL_FORMAT_RGB565) {",
            f"                {prefix}_raster_rgb888_to_rgb565(args, uniforms);",
            "            } else if (args->target_format == GRAPE_PIXEL_FORMAT_RGB888) {",
            f"                {prefix}_raster_rgb888_to_rgb888(args, uniforms);",
            "            }",
            "            break;",
            "        default:",
            "            break;",
            "    }",
            "}",
        ]
    )
    return lines


def _c_type(shader_type: ShaderType) -> str:
    if shader_type == ShaderType.FLOAT:
        return "float"
    if shader_type == ShaderType.VEC4:
        return "grape_shader_vec4_t"
    raise AssertionError(f"unhandled shader type: {shader_type}")


def _float_literal(text: str) -> str:
    lowered = text.lower()
    if "." not in lowered and "e" not in lowered:
        return f"{text}.0f"
    return f"{text}f"


def _emit_unary(operator: str, operand: IRValue, values: dict[int, str]) -> str:
    source = values[operand.id]
    if operand.type == ShaderType.FLOAT:
        return f"({operator}{source})"
    return _vec4_literal([f"{operator}{source}.{component}" for component in "xyzw"])


def _emit_binary(
    operator: str,
    left: IRValue,
    right: IRValue,
    values: dict[int, str],
) -> str:
    left_expr = values[left.id]
    right_expr = values[right.id]

    if left.type == ShaderType.FLOAT and right.type == ShaderType.FLOAT:
        return f"({left_expr} {operator} {right_expr})"

    components: list[str] = []
    for component in "xyzw":
        lhs = left_expr if left.type == ShaderType.FLOAT else f"{left_expr}.{component}"
        rhs = right_expr if right.type == ShaderType.FLOAT else f"{right_expr}.{component}"
        components.append(f"{lhs} {operator} {rhs}")
    return _vec4_literal(components)


def _vec4_literal(components: list[str]) -> str:
    return f"(grape_shader_vec4_t){{ {', '.join(components)} }}"
