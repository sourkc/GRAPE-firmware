from __future__ import annotations

from .ast_nodes import (
    Block,
    ConstructorExpression,
    FloatLiteral,
    FunctionDecl,
    NameExpression,
    Program,
    ReturnStatement,
    UniformDecl,
    VariableDecl,
)
from .diagnostics import SourceFile, fail
from .shader_types import ShaderType


_SHADERTOY_UNIFORMS = (
    ("iResolution", ShaderType.VEC3),
    ("iTime", ShaderType.FLOAT),
    ("iFrame", ShaderType.INT),
    ("iMouse", ShaderType.VEC4),
)


def adapt_shadertoy(source: SourceFile, program: Program) -> Program:
    if any(function.name == "main" for function in program.functions):
        return program

    candidates = [function for function in program.functions if function.name == "mainImage"]
    if len(candidates) != 1:
        return program

    function = candidates[0]
    if function.return_type != ShaderType.VOID or len(function.parameters) != 2:
        fail(
            source,
            function.span,
            "Shadertoy mainImage must have signature 'void mainImage(out vec4, in vec2)'",
        )

    color_parameter, coord_parameter = function.parameters
    if color_parameter.qualifier != "out" or color_parameter.type != ShaderType.VEC4:
        fail(source, color_parameter.span, "first mainImage parameter must be 'out vec4'")
    if coord_parameter.qualifier != "in" or coord_parameter.type != ShaderType.VEC2:
        fail(source, coord_parameter.span, "second mainImage parameter must be 'in vec2'")

    existing_uniforms = {uniform.name for uniform in program.uniforms}
    synthetic_uniforms = list(program.uniforms)
    for name, shader_type in _SHADERTOY_UNIFORMS:
        if name in existing_uniforms:
            fail(source, program.span, f"Shadertoy builtin '{name}' must not be redeclared")
        synthetic_uniforms.append(UniformDecl(program.span, shader_type, name))

    zero = FloatLiteral(color_parameter.span, "0.0", 0.0)
    color_initializer = ConstructorExpression(color_parameter.span, ShaderType.VEC4, [zero])
    color_local = VariableDecl(
        color_parameter.span,
        ShaderType.VEC4,
        color_parameter.name,
        color_initializer,
    )
    coord_local = VariableDecl(
        coord_parameter.span,
        ShaderType.VEC2,
        coord_parameter.name,
        NameExpression(coord_parameter.span, "__grape_frag_coord"),
    )
    result = NameExpression(color_parameter.span, color_parameter.name)
    return_statement = ReturnStatement(function.body.span, result)
    body = Block(
        function.body.span,
        [color_local, coord_local, *function.body.statements, return_statement],
    )
    replacement = FunctionDecl(function.span, ShaderType.VEC4, "main", [], body)

    functions = [replacement if item is function else item for item in program.functions]
    return Program(program.span, synthetic_uniforms, functions)
