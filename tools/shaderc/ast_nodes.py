from __future__ import annotations

from dataclasses import dataclass, field

from .shader_types import ShaderType
from .tokens import SourceSpan


@dataclass
class Node:
    span: SourceSpan


@dataclass
class Program(Node):
    uniforms: list[UniformDecl]
    main: FunctionDecl


@dataclass
class UniformDecl(Node):
    type: ShaderType
    name: str


@dataclass
class FunctionDecl(Node):
    return_type: ShaderType
    name: str
    body: Block


@dataclass
class Block(Node):
    statements: list[Statement]


@dataclass
class Statement(Node):
    pass


@dataclass
class ReturnStatement(Statement):
    expression: Expression


@dataclass
class Expression(Node):
    resolved_type: ShaderType | None = field(default=None, init=False)


@dataclass
class FloatLiteral(Expression):
    text: str
    value: float


@dataclass
class NameExpression(Expression):
    name: str


@dataclass
class UnaryExpression(Expression):
    operator: str
    operand: Expression


@dataclass
class BinaryExpression(Expression):
    left: Expression
    operator: str
    right: Expression
