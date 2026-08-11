from __future__ import annotations

import re

from .ir import (
    IRBinary,
    IRBlock,
    IRBoolConstant,
    IRBreak,
    IRCall,
    IRCast,
    IRConditional,
    IRConstant,
    IRConstruct,
    IRDeclareLocal,
    IRFor,
    IRIf,
    IRInstruction,
    IRIntConstant,
    IRLoadBuiltin,
    IRLoadUniform,
    IRLoadVariable,
    IRLogical,
    IRModule,
    IRReturn,
    IRStoreVariable,
    IRStoreSwizzle,
    IRSwizzle,
    IRUnary,
    IRValue,
)
from .shader_types import ShaderType


_C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_COMPONENTS = "xyzw"
_BUILTIN_TYPES = {
    "source_color": ShaderType.VEC4,
    "uv": ShaderType.VEC2,
    "local_position": ShaderType.VEC2,
    "surface_size": ShaderType.VEC2,
}


def validate_shader_name(name: str) -> None:
    if not _C_IDENTIFIER.fullmatch(name):
        raise ValueError(f"shader filename stem '{name}' is not a valid C identifier")


def generate_c(module: IRModule, shader_name: str, banner: str) -> tuple[str, str]:
    validate_shader_name(shader_name)
    prefix = f"generated_{shader_name}"
    guard = f"GRAPE_{prefix.upper()}_H"
    uses_source_color = _uses_builtin(module, "source_color")

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
    c_lines.extend(_emit_functions(module, prefix))
    c_lines.append("")
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
        c_lines.extend(_emit_raster_route(prefix, source_name, target_name, source_fn, composite_fn, bpp))
        c_lines.append("")

    if not uses_source_color:
        c_lines.extend(_emit_procedural_route(prefix, "rgb565", "grape_shader_composite_rgb565", 2))
        c_lines.append("")
        c_lines.extend(_emit_procedural_route(prefix, "rgb888", "grape_shader_composite_rgb888", 3))
        c_lines.append("")

    c_lines.extend(_emit_kernel_dispatch(prefix, bool(module.uniforms), uses_source_color))
    flags = "GRAPE_SHADER_PROGRAM_USES_SOURCE_COLOR" if uses_source_color else "0U"
    c_lines.extend(
        [
            "",
            f"const grape_shader_program_t {prefix}_program = {{",
            f"    .kernel = {prefix}_kernel,",
            f"    .uniform_size = {'sizeof(' + prefix + '_uniforms_t)' if module.uniforms else '0U'},",
            f"    .flags = {flags},",
            "};",
            "",
        ]
    )

    return "\n".join(c_lines), "\n".join(header_lines)


def _emit_functions(module: IRModule, prefix: str) -> list[str]:
    lines = [
        "typedef struct {",
        "    grape_shader_vec4_t source_color;",
        "    grape_shader_vec2_t uv;",
        "    grape_shader_vec2_t local_position;",
        "    grape_shader_vec2_t surface_size;",
        f"    const {prefix}_uniforms_t *uniforms;",
        f"}} {prefix}_context_t;",
        "",
    ]

    function_names = {function.id: f"{prefix}_fn_{function.id}" for function in module.functions}
    for function in module.functions:
        lines.append(_function_signature(function, prefix, function_names[function.id]) + ";")
    lines.append("")

    uniforms_by_index = {uniform.index: uniform for uniform in module.uniforms}
    for function in module.functions:
        lines.append(_function_signature(function, prefix, function_names[function.id]))
        lines.append("{")
        lines.append("    (void)ctx;")
        for parameter in function.parameters:
            lines.append(f"    (void)_p{parameter.index};")
        emitter = _FunctionEmitter(prefix, function_names, uniforms_by_index)
        lines.extend(emitter.emit_block(function.body, 1))
        lines.append("}")
        lines.append("")
    return lines[:-1]


def _function_signature(function, prefix: str, c_name: str) -> str:
    parameters = [f"const {prefix}_context_t *ctx"]
    parameters.extend(f"{_c_type(parameter.type)} _p{parameter.index}" for parameter in function.parameters)
    return f"static inline {_c_type(function.return_type)} {c_name}({', '.join(parameters)})"


class _FunctionEmitter:
    def __init__(self, prefix: str, function_names: dict[int, str], uniforms_by_index):
        self.prefix = prefix
        self.function_names = function_names
        self.uniforms_by_index = uniforms_by_index
        self.values: dict[int, str] = {}
        self.loop_counter = 0

    def emit_block(self, block: IRBlock, indent: int) -> list[str]:
        lines: list[str] = []
        for instruction in block.instructions:
            lines.extend(self._emit_instruction(instruction, indent))
        return lines

    def _emit_instruction(self, instruction: IRInstruction, indent: int) -> list[str]:
        pad = "    " * indent

        if isinstance(instruction, IRStoreVariable):
            return [f"{pad}{self._variable_name(instruction.kind, instruction.variable_index)} = {self.values[instruction.value.id]};"]

        if isinstance(instruction, IRStoreSwizzle):
            target = self._variable_name(instruction.kind, instruction.variable_index)
            source = self.values[instruction.value.id]
            if len(instruction.components) == 1:
                component = _COMPONENTS[instruction.components[0]]
                return [f"{pad}{target}.{component} = {source};"]
            lines = []
            for source_index, target_index in enumerate(instruction.components):
                target_component = _COMPONENTS[target_index]
                source_component = _COMPONENTS[source_index]
                lines.append(f"{pad}{target}.{target_component} = {source}.{source_component};")
            return lines

        if isinstance(instruction, IRDeclareLocal):
            qualifier = "const " if instruction.is_const else ""
            text = f"{pad}{qualifier}{_c_type(instruction.type)} _l{instruction.local_index}"
            if instruction.initializer is not None:
                text += f" = {self.values[instruction.initializer.id]}"
            return [text + ";"]

        if isinstance(instruction, IRIf):
            lines = [f"{pad}if ({self.values[instruction.condition.id]}) {{"]
            lines.extend(self.emit_block(instruction.then_block, indent + 1))
            if instruction.else_block is None:
                lines.append(f"{pad}}}")
            else:
                lines.append(f"{pad}}} else {{")
                lines.extend(self.emit_block(instruction.else_block, indent + 1))
                lines.append(f"{pad}}}")
            return lines

        if isinstance(instruction, IRFor):
            loop_id = self.loop_counter
            self.loop_counter += 1
            lines = [f"{pad}{{"]
            lines.extend(self.emit_block(instruction.initializer, indent + 1))
            lines.append(
                f"{pad}    for (uint32_t _loop_guard{loop_id} = 0U; "
                f"_loop_guard{loop_id} < {instruction.max_iterations}U; ++_loop_guard{loop_id}) {{"
            )
            lines.extend(self.emit_block(instruction.condition, indent + 2))
            assert instruction.condition.result is not None
            lines.append(f"{pad}        if (!{self.values[instruction.condition.result.id]}) {{")
            lines.append(f"{pad}            break;")
            lines.append(f"{pad}        }}")
            lines.extend(self.emit_block(instruction.body, indent + 2))
            lines.extend(self.emit_block(instruction.increment, indent + 2))
            lines.append(f"{pad}    }}")
            lines.append(f"{pad}}}")
            return lines

        if isinstance(instruction, IRBreak):
            return [f"{pad}break;"]

        if isinstance(instruction, IRReturn):
            return [f"{pad}return {self.values[instruction.value.id]};"]

        assert instruction.result is not None
        result = instruction.result
        name = f"_v{result.id}"

        if isinstance(instruction, IRLogical):
            lines = [f"{pad}bool {name};"]
            if instruction.operator == "&&":
                lines.append(f"{pad}if ({self.values[instruction.left.id]}) {{")
                lines.extend(self.emit_block(instruction.right, indent + 1))
                assert instruction.right.result is not None
                lines.append(f"{pad}    {name} = {self.values[instruction.right.result.id]};")
                lines.append(f"{pad}}} else {{")
                lines.append(f"{pad}    {name} = false;")
                lines.append(f"{pad}}}")
            else:
                lines.append(f"{pad}if ({self.values[instruction.left.id]}) {{")
                lines.append(f"{pad}    {name} = true;")
                lines.append(f"{pad}}} else {{")
                lines.extend(self.emit_block(instruction.right, indent + 1))
                assert instruction.right.result is not None
                lines.append(f"{pad}    {name} = {self.values[instruction.right.result.id]};")
                lines.append(f"{pad}}}")
            self.values[result.id] = name
            return lines

        if isinstance(instruction, IRConditional):
            lines = [f"{pad}{_c_type(result.type)} {name};", f"{pad}if ({self.values[instruction.condition.id]}) {{"]
            lines.extend(self.emit_block(instruction.when_true, indent + 1))
            assert instruction.when_true.result is not None
            lines.append(f"{pad}    {name} = {self.values[instruction.when_true.result.id]};")
            lines.append(f"{pad}}} else {{")
            lines.extend(self.emit_block(instruction.when_false, indent + 1))
            assert instruction.when_false.result is not None
            lines.append(f"{pad}    {name} = {self.values[instruction.when_false.result.id]};")
            lines.append(f"{pad}}}")
            self.values[result.id] = name
            return lines

        expression = self._instruction_expression(instruction, result)
        self.values[result.id] = name
        return [f"{pad}const {_c_type(result.type)} {name} = {expression};"]

    def _instruction_expression(self, instruction: IRInstruction, result: IRValue) -> str:
        if isinstance(instruction, IRConstant):
            return _float_literal(instruction.text)
        if isinstance(instruction, IRIntConstant):
            return str(instruction.value)
        if isinstance(instruction, IRBoolConstant):
            return "true" if instruction.value else "false"
        if isinstance(instruction, IRLoadBuiltin):
            return f"ctx->{instruction.name}"
        if isinstance(instruction, IRLoadUniform):
            return f"ctx->uniforms->{self.uniforms_by_index[instruction.uniform_index].name}"
        if isinstance(instruction, IRLoadVariable):
            return self._variable_name(instruction.kind, instruction.variable_index)
        if isinstance(instruction, IRUnary):
            return _emit_unary(instruction.operator, instruction.operand, self.values)
        if isinstance(instruction, IRBinary):
            return _emit_binary(instruction.operator, instruction.left, instruction.right, result.type, self.values)
        if isinstance(instruction, IRCast):
            return f"({_c_type(result.type)})({self.values[instruction.operand.id]})"
        if isinstance(instruction, IRConstruct):
            return _emit_constructor(result.type, instruction.arguments, self.values)
        if isinstance(instruction, IRSwizzle):
            return _emit_swizzle(result.type, instruction.base, instruction.components, self.values)
        if isinstance(instruction, IRCall):
            arguments = ["ctx"] + [self.values[value.id] for value in instruction.arguments]
            return f"{self.function_names[instruction.function_id]}({', '.join(arguments)})"
        raise AssertionError(f"unhandled IR instruction: {type(instruction).__name__}")

    @staticmethod
    def _variable_name(kind: str, index: int) -> str:
        if kind == "local":
            return f"_l{index}"
        if kind == "parameter":
            return f"_p{index}"
        raise AssertionError(f"unknown variable kind {kind}")


def _emit_eval(module: IRModule, prefix: str) -> list[str]:
    main_name = next(f"{prefix}_fn_{function.id}" for function in module.functions if function.id == module.main_function_id)
    return [
        f"static inline grape_shader_vec4_t {prefix}_eval(",
        "    grape_shader_vec4_t source_color,",
        "    grape_shader_vec2_t uv,",
        "    grape_shader_vec2_t local_position,",
        "    grape_shader_vec2_t surface_size,",
        f"    const {prefix}_uniforms_t *uniforms)",
        "{",
        f"    const {prefix}_context_t ctx = {{",
        "        .source_color = source_color,",
        "        .uv = uv,",
        "        .local_position = local_position,",
        "        .surface_size = surface_size,",
        "        .uniforms = uniforms,",
        "    };",
        f"    return {main_name}(&ctx);",
        "}",
    ]
def _emit_raster_route(
    prefix: str,
    source_name: str,
    target_name: str,
    source_fn: str,
    composite_fn: str,
    bpp: int,
) -> list[str]:
    name = f"{prefix}_raster_{source_name}_to_{target_name}"
    return _raster_function(
        prefix,
        name,
        composite_fn,
        bpp,
        [
            "                int32_t tx = (int32_t)local_x;",
            "                int32_t ty = (int32_t)local_y;",
            f"                grape_shader_vec4_t source_color = {source_fn}(args, tx, ty);",
        ],
    )


def _emit_procedural_route(prefix: str, target_name: str, composite_fn: str, bpp: int) -> list[str]:
    name = f"{prefix}_raster_procedural_to_{target_name}"
    return _raster_function(
        prefix,
        name,
        composite_fn,
        bpp,
        [
            "                grape_shader_vec4_t source_color = {0};",
        ],
    )


def _raster_function(
    prefix: str,
    name: str,
    composite_fn: str,
    bpp: int,
    source_lines: list[str],
) -> list[str]:
    lines = [
        f"static void {name}(",
        "    const grape_shader_kernel_args_t *args,",
        f"    const {prefix}_uniforms_t *uniforms)",
        "{",
        "    const float surface_width = (float)args->surface_width;",
        "    const float surface_height = (float)args->surface_height;",
        "    const float inv_surface_width = 1.0f / surface_width;",
        "    const float inv_surface_height = 1.0f / surface_height;",
        "    const grape_shader_vec2_t surface_size = { surface_width, surface_height };",
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
        "                local_x < surface_width && local_y < surface_height) {",
    ]
    lines.extend(source_lines)
    lines.extend(
        [
            "                grape_shader_vec2_t local_position = { local_x, local_y };",
            "                grape_shader_vec2_t uv = {",
            "                    local_x * inv_surface_width,",
            "                    local_y * inv_surface_height,",
            "                };",
            f"                grape_shader_vec4_t output = {prefix}_eval(",
            "                    source_color, uv, local_position, surface_size, uniforms);",
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
    )
    return lines


def _emit_kernel_dispatch(prefix: str, has_uniforms: bool, uses_source_color: bool) -> list[str]:
    lines = [
        f"static void {prefix}_kernel(",
        "    const grape_shader_kernel_args_t *args,",
        "    const void *uniform_data)",
        "{",
        "    if (!args || args->surface_width == 0U || args->surface_height == 0U) {",
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
            "    if (!args->texture_pixels) {",
        ]
    )
    if uses_source_color:
        lines.extend(
            [
                "        return;",
            ]
        )
    else:
        lines.extend(
            [
                "        if (args->target_format == GRAPE_PIXEL_FORMAT_RGB565) {",
                f"            {prefix}_raster_procedural_to_rgb565(args, uniforms);",
                "        } else if (args->target_format == GRAPE_PIXEL_FORMAT_RGB888) {",
                f"            {prefix}_raster_procedural_to_rgb888(args, uniforms);",
                "        }",
                "        return;",
            ]
        )
    lines.extend(
        [
            "    }",
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



def _walk_instructions(block: IRBlock):
    for instruction in block.instructions:
        yield instruction
        if isinstance(instruction, IRIf):
            yield from _walk_instructions(instruction.then_block)
            if instruction.else_block is not None:
                yield from _walk_instructions(instruction.else_block)
        elif isinstance(instruction, IRFor):
            yield from _walk_instructions(instruction.initializer)
            yield from _walk_instructions(instruction.condition)
            yield from _walk_instructions(instruction.increment)
            yield from _walk_instructions(instruction.body)
        elif isinstance(instruction, IRLogical):
            yield from _walk_instructions(instruction.right)
        elif isinstance(instruction, IRConditional):
            yield from _walk_instructions(instruction.when_true)
            yield from _walk_instructions(instruction.when_false)


def _uses_builtin(module: IRModule, name: str) -> bool:
    return any(
        isinstance(instruction, IRLoadBuiltin) and instruction.name == name
        for function in module.functions
        for instruction in _walk_instructions(function.body)
    )


def _c_type(shader_type: ShaderType) -> str:
    if shader_type == ShaderType.BOOL:
        return "bool"
    if shader_type == ShaderType.INT:
        return "int32_t"
    if shader_type == ShaderType.FLOAT:
        return "float"
    if shader_type == ShaderType.VEC2:
        return "grape_shader_vec2_t"
    if shader_type == ShaderType.VEC3:
        return "grape_shader_vec3_t"
    if shader_type == ShaderType.VEC4:
        return "grape_shader_vec4_t"
    if shader_type == ShaderType.MAT2:
        return "grape_shader_mat2_t"
    if shader_type == ShaderType.MAT3:
        return "grape_shader_mat3_t"
    raise AssertionError(f"unhandled shader type: {shader_type}")


def _float_literal(text: str) -> str:
    lowered = text.lower()
    if "." not in lowered and "e" not in lowered:
        return f"{text}.0f"
    return f"{text}f"


def _emit_unary(operator: str, operand: IRValue, values: dict[int, str]) -> str:
    source = values[operand.id]
    if operand.type.is_scalar:
        return f"({operator}{source})"
    if operand.type.is_vector:
        return _vector_literal(
            operand.type,
            [f"{operator}{source}.{component}" for component in _COMPONENTS[:operand.type.component_count]],
        )
    if operand.type.is_matrix:
        return _matrix_literal(
            operand.type,
            [
                f"{operator}{_matrix_component_expression(operand, source, column, row)}"
                for column in range(operand.type.matrix_size)
                for row in range(operand.type.matrix_size)
            ],
        )
    raise AssertionError(f"unsupported unary operand type {operand.type}")


def _emit_binary(
    operator: str,
    left: IRValue,
    right: IRValue,
    result_type: ShaderType,
    values: dict[int, str],
) -> str:
    left_expr = values[left.id]
    right_expr = values[right.id]

    if result_type == ShaderType.BOOL and operator in ("==", "!="):
        if left.type.is_vector and right.type == left.type:
            comparisons = [
                f"({left_expr}.{component} {operator} {right_expr}.{component})"
                for component in _COMPONENTS[:left.type.component_count]
            ]
            return _join_equality(operator, comparisons)
        if left.type.is_matrix and right.type == left.type:
            comparisons = [
                f"({_matrix_component_expression(left, left_expr, column, row)} {operator} "
                f"{_matrix_component_expression(right, right_expr, column, row)})"
                for column in range(left.type.matrix_size)
                for row in range(left.type.matrix_size)
            ]
            return _join_equality(operator, comparisons)

    if operator == "*" and left.type.is_matrix and right.type.is_vector:
        return _emit_matrix_times_vector(left, right, values)

    if operator == "*" and left.type.is_vector and right.type.is_matrix:
        return _emit_vector_times_matrix(left, right, values)

    if operator == "*" and left.type.is_matrix and right.type.is_matrix:
        return _emit_matrix_times_matrix(left, right, values)

    if result_type.is_scalar:
        return f"({left_expr} {operator} {right_expr})"

    if result_type.is_vector:
        components: list[str] = []
        for component in _COMPONENTS[:result_type.component_count]:
            lhs = left_expr if left.type == ShaderType.FLOAT else f"{left_expr}.{component}"
            rhs = right_expr if right.type == ShaderType.FLOAT else f"{right_expr}.{component}"
            components.append(f"{lhs} {operator} {rhs}")
        return _vector_literal(result_type, components)

    if result_type.is_matrix:
        size = result_type.matrix_size
        components = []
        for column in range(size):
            for row in range(size):
                lhs = (
                    left_expr
                    if left.type == ShaderType.FLOAT
                    else _matrix_component_expression(left, left_expr, column, row)
                )
                rhs = (
                    right_expr
                    if right.type == ShaderType.FLOAT
                    else _matrix_component_expression(right, right_expr, column, row)
                )
                components.append(f"{lhs} {operator} {rhs}")
        return _matrix_literal(result_type, components)

    raise AssertionError(f"unsupported binary result type {result_type}")


def _join_equality(operator: str, comparisons: list[str]) -> str:
    joiner = " && " if operator == "==" else " || "
    return "(" + joiner.join(comparisons) + ")"


def _emit_matrix_times_vector(left: IRValue, right: IRValue, values: dict[int, str]) -> str:
    matrix = values[left.id]
    vector = values[right.id]
    size = left.type.matrix_size
    rows = []
    for row in range(size):
        terms = [
            f"{_matrix_component_expression(left, matrix, column, row)} * {vector}.{_COMPONENTS[column]}"
            for column in range(size)
        ]
        rows.append(" + ".join(terms))
    return _vector_literal(right.type, rows)


def _emit_vector_times_matrix(left: IRValue, right: IRValue, values: dict[int, str]) -> str:
    vector = values[left.id]
    matrix = values[right.id]
    size = right.type.matrix_size
    columns = []
    for column in range(size):
        terms = [
            f"{vector}.{_COMPONENTS[row]} * {_matrix_component_expression(right, matrix, column, row)}"
            for row in range(size)
        ]
        columns.append(" + ".join(terms))
    return _vector_literal(left.type, columns)


def _emit_matrix_times_matrix(left: IRValue, right: IRValue, values: dict[int, str]) -> str:
    left_expr = values[left.id]
    right_expr = values[right.id]
    size = left.type.matrix_size
    components = []
    for column in range(size):
        for row in range(size):
            terms = [
                f"{_matrix_component_expression(left, left_expr, inner, row)} * "
                f"{_matrix_component_expression(right, right_expr, column, inner)}"
                for inner in range(size)
            ]
            components.append(" + ".join(terms))
    return _matrix_literal(left.type, components)


def _emit_constructor(target: ShaderType, arguments: tuple[IRValue, ...], values: dict[int, str]) -> str:
    if target.is_matrix:
        return _emit_matrix_constructor(target, arguments, values)

    width = target.component_count
    if len(arguments) == 1 and arguments[0].type.is_scalar:
        source = _as_float(arguments[0], values[arguments[0].id])
        return _vector_literal(target, [source] * width)

    components: list[str] = []
    for argument in arguments:
        source = values[argument.id]
        for index in range(argument.type.component_count):
            component = _component_expression(argument, source, index)
            if argument.type in (ShaderType.INT, ShaderType.BOOL):
                component = f"(float)({component})"
            components.append(component)
            if len(components) == width:
                return _vector_literal(target, components)
    raise AssertionError("validated constructor did not provide enough components")


def _emit_matrix_constructor(
    target: ShaderType,
    arguments: tuple[IRValue, ...],
    values: dict[int, str],
) -> str:
    size = target.matrix_size

    if len(arguments) == 1 and arguments[0].type.is_numeric_scalar:
        diagonal = _as_float(arguments[0], values[arguments[0].id])
        components = [
            diagonal if column == row else "0.0f"
            for column in range(size)
            for row in range(size)
        ]
        return _matrix_literal(target, components)

    if len(arguments) == 1 and arguments[0].type.is_matrix:
        source_value = arguments[0]
        source = values[source_value.id]
        source_size = source_value.type.matrix_size
        components = []
        for column in range(size):
            for row in range(size):
                if column < source_size and row < source_size:
                    component = _matrix_component_expression(source_value, source, column, row)
                else:
                    component = "1.0f" if column == row else "0.0f"
                components.append(component)
        return _matrix_literal(target, components)

    components: list[str] = []
    needed = size * size
    for argument in arguments:
        source = values[argument.id]
        for index in range(argument.type.component_count):
            component = _component_expression(argument, source, index)
            if argument.type == ShaderType.INT:
                component = f"(float)({component})"
            components.append(component)
            if len(components) == needed:
                return _matrix_literal(target, components)
    raise AssertionError("validated matrix constructor did not provide enough components")


def _as_float(value: IRValue, expression: str) -> str:
    if value.type == ShaderType.FLOAT:
        return expression
    if value.type in (ShaderType.INT, ShaderType.BOOL):
        return f"(float)({expression})"
    raise AssertionError("expected scalar for floating vector constructor")


def _emit_swizzle(
    result_type: ShaderType,
    base: IRValue,
    components: tuple[int, ...],
    values: dict[int, str],
) -> str:
    source = values[base.id]
    selected = [_component_expression(base, source, component) for component in components]
    if result_type == ShaderType.FLOAT:
        return selected[0]
    return _vector_literal(result_type, selected)


def _component_expression(value: IRValue, expression: str, component: int) -> str:
    if value.type.is_scalar:
        if component != 0:
            raise AssertionError("scalar component out of range")
        return expression
    if value.type.is_vector:
        return f"{expression}.{_COMPONENTS[component]}"
    if value.type.is_matrix:
        size = value.type.matrix_size
        column = component // size
        row = component % size
        return _matrix_component_expression(value, expression, column, row)
    raise AssertionError(f"unsupported component source {value.type}")


def _matrix_component_expression(value: IRValue, expression: str, column: int, row: int) -> str:
    if not value.type.is_matrix:
        raise AssertionError("expected matrix value")
    return f"{expression}.c{column}.{_COMPONENTS[row]}"


def _vector_literal(shader_type: ShaderType, components: list[str]) -> str:
    return f"({_c_type(shader_type)}){{ {', '.join(components)} }}"


def _matrix_literal(shader_type: ShaderType, components: list[str]) -> str:
    size = shader_type.matrix_size
    columns = []
    for column in range(size):
        start = column * size
        values = components[start:start + size]
        columns.append(f".c{column} = {{ {', '.join(values)} }}")
    return f"({_c_type(shader_type)}){{ {', '.join(columns)} }}"
