from __future__ import annotations

from dataclasses import dataclass

from .shader_types import ShaderType


@dataclass(frozen=True)
class BuiltinResolution:
    name: str
    return_type: ShaderType


_FLOAT_OR_VECTOR = (
    ShaderType.FLOAT,
    ShaderType.VEC2,
    ShaderType.VEC3,
    ShaderType.VEC4,
)

_NUMERIC_SCALAR_OR_VECTOR = (
    ShaderType.INT,
    ShaderType.FLOAT,
    ShaderType.VEC2,
    ShaderType.VEC3,
    ShaderType.VEC4,
)

BUILTIN_FUNCTION_NAMES = frozenset({
    "abs",
    "ceil",
    "clamp",
    "cos",
    "cross",
    "dot",
    "exp",
    "floor",
    "fract",
    "length",
    "max",
    "min",
    "mix",
    "normalize",
    "pow",
    "radians",
    "reflect",
    "sign",
    "sin",
    "smoothstep",
    "sqrt",
})


def resolve_builtin(name: str, arguments: tuple[ShaderType, ...]) -> BuiltinResolution | None:
    if name not in BUILTIN_FUNCTION_NAMES:
        return None

    if name in {"abs", "sign"}:
        if len(arguments) == 1 and arguments[0] in _NUMERIC_SCALAR_OR_VECTOR:
            return BuiltinResolution(name, arguments[0])
        return None

    if name in {"ceil", "cos", "exp", "floor", "fract", "radians", "sin", "sqrt"}:
        if len(arguments) == 1 and arguments[0] in _FLOAT_OR_VECTOR:
            return BuiltinResolution(name, arguments[0])
        return None

    if name in {"min", "max"}:
        if len(arguments) != 2:
            return None
        x, y = arguments
        if x == y and x in _NUMERIC_SCALAR_OR_VECTOR:
            return BuiltinResolution(name, x)
        if x.is_vector and y == ShaderType.FLOAT:
            return BuiltinResolution(name, x)
        return None

    if name == "clamp":
        if len(arguments) != 3:
            return None
        x, low, high = arguments
        if x == low == high and x in _NUMERIC_SCALAR_OR_VECTOR:
            return BuiltinResolution(name, x)
        if x.is_vector and low == high == ShaderType.FLOAT:
            return BuiltinResolution(name, x)
        return None

    if name == "mix":
        if len(arguments) != 3:
            return None
        x, y, amount = arguments
        if x != y or x not in _FLOAT_OR_VECTOR:
            return None
        if amount == x or (x.is_vector and amount == ShaderType.FLOAT):
            return BuiltinResolution(name, x)
        return None

    if name == "smoothstep":
        if len(arguments) != 3:
            return None
        edge0, edge1, x = arguments
        if x not in _FLOAT_OR_VECTOR:
            return None
        if edge0 == edge1 == x:
            return BuiltinResolution(name, x)
        if x.is_vector and edge0 == edge1 == ShaderType.FLOAT:
            return BuiltinResolution(name, x)
        return None

    if name == "pow":
        if len(arguments) == 2 and arguments[0] == arguments[1] and arguments[0] in _FLOAT_OR_VECTOR:
            return BuiltinResolution(name, arguments[0])
        return None

    if name == "dot":
        if len(arguments) == 2 and arguments[0] == arguments[1] and arguments[0] in _FLOAT_OR_VECTOR:
            return BuiltinResolution(name, ShaderType.FLOAT)
        return None

    if name == "length":
        if len(arguments) == 1 and arguments[0] in _FLOAT_OR_VECTOR:
            return BuiltinResolution(name, ShaderType.FLOAT)
        return None

    if name == "normalize":
        if len(arguments) == 1 and arguments[0] in _FLOAT_OR_VECTOR:
            return BuiltinResolution(name, arguments[0])
        return None

    if name == "cross":
        if arguments == (ShaderType.VEC3, ShaderType.VEC3):
            return BuiltinResolution(name, ShaderType.VEC3)
        return None

    if name == "reflect":
        if len(arguments) == 2 and arguments[0] == arguments[1] and arguments[0] in _FLOAT_OR_VECTOR:
            return BuiltinResolution(name, arguments[0])
        return None

    raise AssertionError(f"unhandled builtin '{name}'")
