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
        "typedef struct {",
        "    float x;",
        "    float y;",
        "    float z;",
        "    float w;",
        f"}} {prefix}_vec4_t;",
        "",
        "typedef struct {",
    ]

    if module.uniforms:
        for uniform in module.uniforms:
            header_lines.append(f"    {_c_type(uniform.type, prefix)} {uniform.name};")
    else:
        header_lines.append("    unsigned char _unused;")

    header_lines.extend(
        [
            f"}} {prefix}_uniforms_t;",
            "",
            f"{prefix}_vec4_t {prefix}_eval(",
            f"    {prefix}_vec4_t source_color,",
            f"    const {prefix}_uniforms_t *uniforms);",
            "",
            f"#endif /* {guard} */",
            "",
        ]
    )

    c_lines = [
        banner.rstrip(),
        "",
        f'#include "{prefix}.h"',
        "",
        f"{prefix}_vec4_t {prefix}_eval(",
        f"    {prefix}_vec4_t source_color,",
        f"    const {prefix}_uniforms_t *uniforms)",
        "{",
    ]

    uses_source_color = any(isinstance(instruction, IRLoadBuiltin) for instruction in module.main.instructions)
    uses_uniforms = any(isinstance(instruction, IRLoadUniform) for instruction in module.main.instructions)
    if not uses_source_color:
        c_lines.append("    (void)source_color;")
    if not uses_uniforms:
        c_lines.append("    (void)uniforms;")

    uniforms_by_index = {uniform.index: uniform for uniform in module.uniforms}
    value_exprs: dict[int, str] = {}

    for instruction in module.main.instructions:
        if isinstance(instruction, IRReturn):
            c_lines.append(f"    return {value_exprs[instruction.value.id]};")
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
            expression = _emit_unary(prefix, instruction.operator, instruction.operand, value_exprs)
        elif isinstance(instruction, IRBinary):
            expression = _emit_binary(prefix, instruction.operator, instruction.left, instruction.right, value_exprs)
        else:
            raise AssertionError(f"unhandled IR instruction: {type(instruction).__name__}")

        c_lines.append(f"    const {_c_type(result.type, prefix)} {name} = {expression};")
        value_exprs[result.id] = name

    c_lines.extend(["}", ""])
    return "\n".join(c_lines), "\n".join(header_lines)


def _c_type(shader_type: ShaderType, prefix: str) -> str:
    if shader_type == ShaderType.FLOAT:
        return "float"
    if shader_type == ShaderType.VEC4:
        return f"{prefix}_vec4_t"
    raise AssertionError(f"unhandled shader type: {shader_type}")


def _float_literal(text: str) -> str:
    lowered = text.lower()
    if "." not in lowered and "e" not in lowered:
        return f"{text}.0f"
    return f"{text}f"


def _emit_unary(prefix: str, operator: str, operand: IRValue, values: dict[int, str]) -> str:
    source = values[operand.id]
    if operand.type == ShaderType.FLOAT:
        return f"({operator}{source})"
    return _vec4_literal(prefix, [f"{operator}{source}.{component}" for component in "xyzw"])


def _emit_binary(
    prefix: str,
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
    return _vec4_literal(prefix, components)


def _vec4_literal(prefix: str, components: list[str]) -> str:
    return f"({prefix}_vec4_t){{ {', '.join(components)} }}"
