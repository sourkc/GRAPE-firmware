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
    functions: list[FunctionDecl]


@dataclass
class UniformDecl(Node):
    type: ShaderType
    name: str


@dataclass
class FunctionParameter(Node):
    type: ShaderType
    name: str
    qualifier: str = "in"


@dataclass
class FunctionDecl(Node):
    return_type: ShaderType
    name: str
    parameters: list[FunctionParameter]
    body: Block
    resolved_id: int | None = field(default=None, init=False)


@dataclass
class Statement(Node):
    pass


@dataclass
class Block(Statement):
    statements: list[Statement]


@dataclass
class VariableDecl(Statement):
    type: ShaderType
    name: str
    initializer: Expression | None
    is_const: bool = False
    resolved_local_index: int | None = field(default=None, init=False)


@dataclass
class VariableDeclGroup(Statement):
    declarations: list[VariableDecl]


@dataclass
class EmptyStatement(Statement):
    pass


@dataclass
class ExpressionStatement(Statement):
    expression: Expression


@dataclass
class ReturnStatement(Statement):
    expression: Expression


@dataclass
class IfStatement(Statement):
    condition: Expression
    then_branch: Statement
    else_branch: Statement | None


@dataclass
class ForStatement(Statement):
    initializer: Statement | None
    condition: Expression | None
    increment: Expression | None
    body: Statement
    max_iterations: int | None = field(default=None, init=False)


@dataclass
class BreakStatement(Statement):
    pass


@dataclass
class Expression(Node):
    resolved_type: ShaderType | None = field(default=None, init=False)


@dataclass
class FloatLiteral(Expression):
    text: str
    value: float


@dataclass
class IntLiteral(Expression):
    text: str
    value: int


@dataclass
class BoolLiteral(Expression):
    value: bool


@dataclass
class NameExpression(Expression):
    name: str
    resolved_symbol_kind: str | None = field(default=None, init=False)
    resolved_symbol_index: int | None = field(default=None, init=False)


@dataclass
class UnaryExpression(Expression):
    operator: str
    operand: Expression


@dataclass
class BinaryExpression(Expression):
    left: Expression
    operator: str
    right: Expression


@dataclass
class AssignmentExpression(Expression):
    target: Expression
    operator: str
    value: Expression


@dataclass
class UpdateExpression(Expression):
    operand: Expression
    operator: str
    prefix: bool


@dataclass
class TernaryExpression(Expression):
    condition: Expression
    when_true: Expression
    when_false: Expression


@dataclass
class CallExpression(Expression):
    name: str
    arguments: list[Expression]
    resolved_function_id: int | None = field(default=None, init=False)
    resolved_builtin_name: str | None = field(default=None, init=False)


@dataclass
class ConstructorExpression(Expression):
    target_type: ShaderType
    arguments: list[Expression]


@dataclass
class SwizzleExpression(Expression):
    base: Expression
    fields: str
