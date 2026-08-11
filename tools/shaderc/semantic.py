from __future__ import annotations

from dataclasses import dataclass

from .ast_nodes import BinaryExpression, Expression, FloatLiteral, NameExpression, Program, ReturnStatement, UnaryExpression
from .diagnostics import SourceFile, fail
from .shader_types import ShaderType


@dataclass(frozen=True)
class Symbol:
    name: str
    type: ShaderType
    kind: str
    index: int | None = None


class SemanticAnalyzer:
    def __init__(self, source: SourceFile):
        self.source = source
        self.symbols: dict[str, Symbol] = {
            "source_color": Symbol("source_color", ShaderType.VEC4, "builtin"),
        }

    def analyze(self, program: Program) -> Program:
        for index, uniform in enumerate(program.uniforms):
            if uniform.name in self.symbols:
                fail(self.source, uniform.span, f"redefinition of '{uniform.name}'")
            self.symbols[uniform.name] = Symbol(uniform.name, uniform.type, "uniform", index)

        if program.main.name != "main":
            fail(self.source, program.main.span, "shader entry point must be named 'main'")
        if program.main.return_type != ShaderType.VEC4:
            fail(self.source, program.main.span, "main() must return vec4")

        statement = program.main.body.statements[0]
        assert isinstance(statement, ReturnStatement)
        return_type = self._analyze_expression(statement.expression)
        if return_type != ShaderType.VEC4:
            fail(self.source, statement.expression.span, f"main() must return vec4, got {return_type}")

        return program

    def _analyze_expression(self, expression: Expression) -> ShaderType:
        if isinstance(expression, FloatLiteral):
            expression.resolved_type = ShaderType.FLOAT
            return ShaderType.FLOAT

        if isinstance(expression, NameExpression):
            symbol = self.symbols.get(expression.name)
            if symbol is None:
                fail(self.source, expression.span, f"undeclared identifier '{expression.name}'")
            expression.resolved_type = symbol.type
            return symbol.type

        if isinstance(expression, UnaryExpression):
            operand_type = self._analyze_expression(expression.operand)
            if expression.operator not in ("+", "-"):
                fail(self.source, expression.span, f"unsupported unary operator '{expression.operator}'")
            expression.resolved_type = operand_type
            return operand_type

        if isinstance(expression, BinaryExpression):
            left_type = self._analyze_expression(expression.left)
            right_type = self._analyze_expression(expression.right)
            result_type = self._binary_result_type(expression.operator, left_type, right_type)
            if result_type is None:
                fail(
                    self.source,
                    expression.span,
                    f"cannot apply '{expression.operator}' to {left_type} and {right_type}",
                )
            expression.resolved_type = result_type
            return result_type

        raise AssertionError(f"unhandled expression type: {type(expression).__name__}")

    @staticmethod
    def _binary_result_type(operator: str, left: ShaderType, right: ShaderType) -> ShaderType | None:
        if operator not in ("+", "-", "*", "/"):
            return None
        if left == right:
            return left
        if {left, right} == {ShaderType.FLOAT, ShaderType.VEC4}:
            return ShaderType.VEC4
        return None
