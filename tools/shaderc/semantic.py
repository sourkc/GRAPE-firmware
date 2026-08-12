from __future__ import annotations

from dataclasses import dataclass

from .ast_nodes import (
    AssignmentExpression,
    BinaryExpression,
    Block,
    BoolLiteral,
    BreakStatement,
    CallExpression,
    ConstructorExpression,
    EmptyStatement,
    Expression,
    ExpressionStatement,
    FloatLiteral,
    ForStatement,
    FunctionDecl,
    IfStatement,
    IntLiteral,
    NameExpression,
    Program,
    ReturnStatement,
    Statement,
    SwizzleExpression,
    TernaryExpression,
    UnaryExpression,
    UpdateExpression,
    VariableDecl,
    VariableDeclGroup,
)
from .builtins import BUILTIN_FUNCTION_NAMES, resolve_builtin
from .diagnostics import SourceFile, fail
from .shader_types import ShaderType


@dataclass(frozen=True)
class Symbol:
    name: str
    type: ShaderType
    kind: str
    index: int | None = None
    is_const: bool = False


@dataclass(frozen=True)
class LValue:
    type: ShaderType
    kind: str
    index: int
    components: tuple[int, ...] | None = None


@dataclass(frozen=True)
class FunctionSymbol:
    id: int
    name: str
    return_type: ShaderType
    parameter_types: tuple[ShaderType, ...]
    declaration: FunctionDecl


_SWIZZLE_SETS = (
    "xyzw",
    "rgba",
    "stpq",
)

_SWIZZLE_INDEX = {
    "x": 0, "r": 0, "s": 0,
    "y": 1, "g": 1, "t": 1,
    "z": 2, "b": 2, "p": 2,
    "w": 3, "a": 3, "q": 3,
}

_MAX_STATIC_LOOP_ITERATIONS = 1_000_000


class SemanticAnalyzer:
    def __init__(self, source: SourceFile):
        self.source = source
        self.globals: dict[str, Symbol] = {
            "source_color": Symbol("source_color", ShaderType.VEC4, "builtin"),
            "uv": Symbol("uv", ShaderType.VEC2, "builtin"),
            "local_position": Symbol("local_position", ShaderType.VEC2, "builtin"),
            "surface_size": Symbol("surface_size", ShaderType.VEC2, "builtin"),
            "__grape_frag_coord": Symbol("__grape_frag_coord", ShaderType.VEC2, "builtin"),
        }
        self.functions: dict[str, list[FunctionSymbol]] = {}
        self.function_by_id: dict[int, FunctionSymbol] = {}
        self.scopes: list[dict[str, Symbol]] = []
        self.current_function: FunctionSymbol | None = None
        self.next_local_index = 0
        self.loop_depth = 0
        self.call_graph: dict[int, set[int]] = {}

    def analyze(self, program: Program) -> Program:
        for index, uniform in enumerate(program.uniforms):
            if uniform.name in self.globals:
                fail(self.source, uniform.span, f"redefinition of '{uniform.name}'")
            self.globals[uniform.name] = Symbol(uniform.name, uniform.type, "uniform", index, True)

        self._collect_functions(program)
        main_overloads = self.functions.get("main", [])
        if len(main_overloads) != 1:
            fail(self.source, program.span, "shader must define exactly one main() function")
        main = main_overloads[0]
        if main.return_type != ShaderType.VEC4 or main.parameter_types:
            fail(self.source, main.declaration.span, "main() must have signature 'vec4 main()'")

        for function in self.function_by_id.values():
            self._analyze_function(function)

        self._reject_recursion()
        return program

    def _collect_functions(self, program: Program) -> None:
        for function_id, declaration in enumerate(program.functions):
            if declaration.return_type == ShaderType.VOID:
                fail(self.source, declaration.span, "void functions are currently supported only for Shadertoy mainImage")
            if any(parameter.qualifier != "in" for parameter in declaration.parameters):
                fail(self.source, declaration.span, "out parameters are currently supported only for Shadertoy mainImage")
            declaration.resolved_id = function_id
            parameter_types = tuple(parameter.type for parameter in declaration.parameters)
            overloads = self.functions.setdefault(declaration.name, [])
            if any(existing.parameter_types == parameter_types for existing in overloads):
                signature = self._format_signature(declaration.name, parameter_types)
                fail(self.source, declaration.span, f"redefinition of function '{signature}'")
            symbol = FunctionSymbol(
                function_id,
                declaration.name,
                declaration.return_type,
                parameter_types,
                declaration,
            )
            overloads.append(symbol)
            self.function_by_id[function_id] = symbol
            self.call_graph[function_id] = set()

    def _analyze_function(self, function: FunctionSymbol) -> None:
        self.current_function = function
        self.next_local_index = 0
        self.loop_depth = 0
        self.scopes = [{}]

        for parameter_index, parameter in enumerate(function.declaration.parameters):
            if parameter.name in self.scopes[-1]:
                fail(self.source, parameter.span, f"redefinition of parameter '{parameter.name}'")
            self.scopes[-1][parameter.name] = Symbol(
                parameter.name,
                parameter.type,
                "parameter",
                parameter_index,
            )

        self._analyze_block(function.declaration.body, create_scope=False)
        if not self._always_returns(function.declaration.body):
            fail(
                self.source,
                function.declaration.body.span,
                f"function '{function.name}' does not return a value on every path",
            )

        self.current_function = None
        self.scopes = []

    def _analyze_block(self, block: Block, *, create_scope: bool = True) -> None:
        if create_scope:
            self.scopes.append({})
        try:
            for statement in block.statements:
                self._analyze_statement(statement)
        finally:
            if create_scope:
                self.scopes.pop()

    def _analyze_statement(self, statement: Statement) -> None:
        if isinstance(statement, EmptyStatement):
            return

        if isinstance(statement, Block):
            self._analyze_block(statement)
            return

        if isinstance(statement, VariableDecl):
            if statement.is_const and statement.initializer is None:
                fail(self.source, statement.span, "const variable requires an initializer")
            initializer_type = None
            if statement.initializer is not None:
                initializer_type = self._analyze_expression(statement.initializer)
                if initializer_type != statement.type:
                    fail(
                        self.source,
                        statement.initializer.span,
                        f"cannot initialize {statement.type} with {initializer_type}",
                    )
            scope = self.scopes[-1]
            if statement.name in scope:
                fail(self.source, statement.span, f"redefinition of local variable '{statement.name}'")
            local_index = self.next_local_index
            self.next_local_index += 1
            statement.resolved_local_index = local_index
            scope[statement.name] = Symbol(
                statement.name,
                statement.type,
                "local",
                local_index,
                statement.is_const,
            )
            return

        if isinstance(statement, VariableDeclGroup):
            for declaration in statement.declarations:
                self._analyze_statement(declaration)
            return

        if isinstance(statement, ExpressionStatement):
            self._analyze_expression(statement.expression)
            return

        if isinstance(statement, ReturnStatement):
            assert self.current_function is not None
            result_type = self._analyze_expression(statement.expression)
            if result_type != self.current_function.return_type:
                fail(
                    self.source,
                    statement.expression.span,
                    f"cannot return {result_type} from function returning {self.current_function.return_type}",
                )
            return

        if isinstance(statement, IfStatement):
            condition_type = self._analyze_expression(statement.condition)
            if condition_type != ShaderType.BOOL:
                fail(self.source, statement.condition.span, f"if condition must be bool, got {condition_type}")
            self._analyze_scoped_statement(statement.then_branch)
            if statement.else_branch is not None:
                self._analyze_scoped_statement(statement.else_branch)
            return

        if isinstance(statement, ForStatement):
            self.scopes.append({})
            try:
                if statement.initializer is not None:
                    self._analyze_statement(statement.initializer)
                if statement.condition is None:
                    fail(self.source, statement.span, "for loop must have a statically bounded condition")
                condition_type = self._analyze_expression(statement.condition)
                if condition_type != ShaderType.BOOL:
                    fail(self.source, statement.condition.span, f"for condition must be bool, got {condition_type}")
                if statement.increment is None:
                    fail(self.source, statement.span, "for loop must have a statically bounded increment")
                self._analyze_expression(statement.increment)
                statement.max_iterations = self._prove_loop_bound(statement)
                self.loop_depth += 1
                try:
                    self._analyze_scoped_statement(statement.body)
                finally:
                    self.loop_depth -= 1
            finally:
                self.scopes.pop()
            return

        if isinstance(statement, BreakStatement):
            if self.loop_depth == 0:
                fail(self.source, statement.span, "break is only valid inside a loop")
            return

        raise AssertionError(f"unhandled statement type: {type(statement).__name__}")

    def _analyze_scoped_statement(self, statement: Statement) -> None:
        if isinstance(statement, Block):
            self._analyze_block(statement)
            return
        self.scopes.append({})
        try:
            self._analyze_statement(statement)
        finally:
            self.scopes.pop()

    def _analyze_expression(self, expression: Expression) -> ShaderType:
        if isinstance(expression, FloatLiteral):
            expression.resolved_type = ShaderType.FLOAT
            return ShaderType.FLOAT

        if isinstance(expression, IntLiteral):
            expression.resolved_type = ShaderType.INT
            return ShaderType.INT

        if isinstance(expression, BoolLiteral):
            expression.resolved_type = ShaderType.BOOL
            return ShaderType.BOOL

        if isinstance(expression, NameExpression):
            symbol = self._resolve_symbol(expression.name)
            if symbol is None:
                fail(self.source, expression.span, f"undeclared identifier '{expression.name}'")
            expression.resolved_symbol_kind = symbol.kind
            expression.resolved_symbol_index = symbol.index
            expression.resolved_type = symbol.type
            return symbol.type

        if isinstance(expression, UnaryExpression):
            operand_type = self._analyze_expression(expression.operand)
            if expression.operator in ("+", "-"):
                if not (operand_type.is_numeric_scalar or operand_type.is_vector or operand_type.is_matrix):
                    fail(self.source, expression.span, f"cannot apply '{expression.operator}' to {operand_type}")
                expression.resolved_type = operand_type
                return operand_type
            if expression.operator == "!":
                if operand_type != ShaderType.BOOL:
                    fail(self.source, expression.span, f"cannot apply '!' to {operand_type}")
                expression.resolved_type = ShaderType.BOOL
                return ShaderType.BOOL
            fail(self.source, expression.span, f"unsupported unary operator '{expression.operator}'")

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

        if isinstance(expression, AssignmentExpression):
            target = self._analyze_lvalue(expression.target)
            value_type = self._analyze_expression(expression.value)
            if expression.operator == "=":
                if target.type != value_type:
                    fail(self.source, expression.span, f"cannot assign {value_type} to {target.type}")
            else:
                arithmetic = expression.operator[0]
                result = self._binary_result_type(arithmetic, target.type, value_type)
                if result != target.type:
                    fail(
                        self.source,
                        expression.span,
                        f"cannot apply '{expression.operator}' to {target.type} and {value_type}",
                    )
            expression.resolved_type = target.type
            return target.type

        if isinstance(expression, UpdateExpression):
            target = self._analyze_lvalue(expression.operand)
            if target.type not in (ShaderType.INT, ShaderType.FLOAT):
                fail(self.source, expression.span, f"cannot apply '{expression.operator}' to {target.type}")
            expression.resolved_type = target.type
            return target.type

        if isinstance(expression, TernaryExpression):
            condition_type = self._analyze_expression(expression.condition)
            if condition_type != ShaderType.BOOL:
                fail(self.source, expression.condition.span, f"conditional expression requires bool, got {condition_type}")
            true_type = self._analyze_expression(expression.when_true)
            false_type = self._analyze_expression(expression.when_false)
            if true_type != false_type:
                fail(
                    self.source,
                    expression.span,
                    f"conditional branches must have the same type, got {true_type} and {false_type}",
                )
            expression.resolved_type = true_type
            return true_type

        if isinstance(expression, CallExpression):
            argument_types = tuple(self._analyze_expression(argument) for argument in expression.arguments)
            overloads = self.functions.get(expression.name, [])
            matches = [function for function in overloads if function.parameter_types == argument_types]
            if len(matches) == 1:
                function = matches[0]
                expression.resolved_function_id = function.id
                expression.resolved_type = function.return_type
                assert self.current_function is not None
                self.call_graph[self.current_function.id].add(function.id)
                return function.return_type

            builtin = resolve_builtin(expression.name, argument_types)
            if builtin is not None:
                expression.resolved_builtin_name = builtin.name
                expression.resolved_type = builtin.return_type
                return builtin.return_type

            signature = self._format_signature(expression.name, argument_types)
            if expression.name in BUILTIN_FUNCTION_NAMES:
                fail(self.source, expression.span, f"no matching builtin overload for '{signature}'")
            if not overloads:
                fail(self.source, expression.span, f"unknown function '{expression.name}'")
            candidates = ", ".join(
                self._format_signature(function.name, function.parameter_types) for function in overloads
            )
            fail(
                self.source,
                expression.span,
                f"no matching overload for '{signature}'; candidates: {candidates}",
            )

        if isinstance(expression, ConstructorExpression):
            result_type = self._analyze_constructor(expression)
            expression.resolved_type = result_type
            return result_type

        if isinstance(expression, SwizzleExpression):
            base_type = self._analyze_expression(expression.base)
            result_type = self._analyze_swizzle(expression, base_type)
            expression.resolved_type = result_type
            return result_type

        raise AssertionError(f"unhandled expression type: {type(expression).__name__}")

    def _analyze_lvalue(self, expression: Expression) -> LValue:
        if isinstance(expression, NameExpression):
            symbol = self._resolve_symbol(expression.name)
            if symbol is None:
                fail(self.source, expression.span, f"undeclared identifier '{expression.name}'")
            if symbol.kind not in ("local", "parameter"):
                fail(self.source, expression.span, f"'{expression.name}' is not writable")
            if symbol.is_const:
                fail(self.source, expression.span, f"cannot modify const variable '{expression.name}'")
            assert symbol.index is not None
            expression.resolved_symbol_kind = symbol.kind
            expression.resolved_symbol_index = symbol.index
            expression.resolved_type = symbol.type
            return LValue(symbol.type, symbol.kind, symbol.index)

        if isinstance(expression, SwizzleExpression):
            if not isinstance(expression.base, NameExpression):
                fail(self.source, expression.span, "writable component selection must be applied to a named variable")
            base = self._analyze_lvalue(expression.base)
            if not base.type.is_vector:
                fail(self.source, expression.span, f"component selection is not valid on {base.type}")
            result_type = self._analyze_swizzle(expression, base.type)
            components = tuple(_SWIZZLE_INDEX[field] for field in expression.fields)
            if len(set(components)) != len(components):
                fail(self.source, expression.span, "writable component selection cannot contain duplicate components")
            expression.resolved_type = result_type
            return LValue(result_type, base.kind, base.index, components)

        fail(self.source, expression.span, "expression is not a writable l-value")

    def _analyze_constructor(self, expression: ConstructorExpression) -> ShaderType:
        target = expression.target_type
        if not expression.arguments:
            fail(self.source, expression.span, f"{target} constructor requires arguments")

        argument_types = [self._analyze_expression(argument) for argument in expression.arguments]

        if target.is_scalar:
            if len(argument_types) != 1 or not argument_types[0].is_scalar:
                fail(self.source, expression.span, f"{target} constructor requires exactly one scalar argument")
            return target

        if target.is_matrix:
            if len(argument_types) == 1 and argument_types[0].is_numeric_scalar:
                return target
            if len(argument_types) == 1 and argument_types[0].is_matrix:
                return target
            if any(argument_type.is_matrix for argument_type in argument_types):
                fail(self.source, expression.span, "matrix constructor cannot combine a matrix argument with other arguments")
            self._validate_floating_composite_constructor(expression, target, argument_types)
            return target

        if len(argument_types) == 1 and argument_types[0].is_scalar:
            return target

        self._validate_floating_composite_constructor(expression, target, argument_types)
        return target

    def _validate_floating_composite_constructor(
        self,
        expression: ConstructorExpression,
        target: ShaderType,
        argument_types: list[ShaderType],
    ) -> None:
        needed = target.component_count
        provided = 0
        for index, argument_type in enumerate(argument_types):
            if argument_type == ShaderType.BOOL:
                fail(
                    self.source,
                    expression.arguments[index].span,
                    f"bool cannot provide components to a {target} constructor",
                )
            if provided >= needed:
                fail(self.source, expression.arguments[index].span, f"too many arguments for {target} constructor")
            provided += argument_type.component_count

        if provided < needed:
            fail(
                self.source,
                expression.span,
                f"not enough components for {target} constructor: need {needed}, got {provided}",
            )

    def _analyze_swizzle(self, expression: SwizzleExpression, base_type: ShaderType) -> ShaderType:
        if not base_type.is_vector:
            fail(self.source, expression.span, f"component selection is not valid on {base_type}")
        fields = expression.fields
        if not fields or len(fields) > 4:
            fail(self.source, expression.span, "component selection must contain between 1 and 4 components")

        selected_set = None
        for candidate in _SWIZZLE_SETS:
            if fields[0] in candidate:
                selected_set = candidate
                break
        if selected_set is None or any(field not in selected_set for field in fields):
            fail(self.source, expression.span, f"invalid component selection '.{fields}'")

        width = base_type.component_count
        for field in fields:
            if selected_set.index(field) >= width:
                fail(self.source, expression.span, f"component '{field}' is not available on {base_type}")

        if len(fields) == 1:
            return ShaderType.FLOAT
        return ShaderType.vector(len(fields))

    def _resolve_symbol(self, name: str) -> Symbol | None:
        for scope in reversed(self.scopes):
            symbol = scope.get(name)
            if symbol is not None:
                return symbol
        return self.globals.get(name)

    def _prove_loop_bound(self, statement: ForStatement) -> int:
        initializer = statement.initializer
        if not isinstance(initializer, VariableDecl) or initializer.type != ShaderType.INT:
            fail(self.source, statement.span, "bounded for loop must declare an int induction variable")
        if initializer.resolved_local_index is None or initializer.initializer is None:
            fail(self.source, statement.span, "bounded for loop induction variable requires an initializer")
        start = self._constant_int(initializer.initializer)
        if start is None:
            fail(self.source, initializer.initializer.span, "for loop induction variable must start from a constant int")

        step = self._loop_step(statement.increment, initializer.resolved_local_index)
        if step == 0:
            fail(self.source, statement.increment.span, "for loop increment does not advance the induction variable")

        bound = self._find_loop_bound(statement.condition, initializer.resolved_local_index)
        if bound is None:
            fail(
                self.source,
                statement.condition.span,
                "for loop condition must contain a static bound on its induction variable",
            )
        operator, limit = bound
        iterations = self._iteration_count(start, step, operator, limit)
        if iterations is None:
            fail(self.source, statement.span, "for loop increment moves away from its static bound")
        if iterations > _MAX_STATIC_LOOP_ITERATIONS:
            fail(
                self.source,
                statement.span,
                f"for loop may execute {iterations} iterations; maximum is {_MAX_STATIC_LOOP_ITERATIONS}",
            )
        return max(iterations, 1)

    def _loop_step(self, expression: Expression | None, local_index: int) -> int:
        if isinstance(expression, UpdateExpression):
            if self._is_local_name(expression.operand, local_index):
                return 1 if expression.operator == "++" else -1
            return 0
        if isinstance(expression, AssignmentExpression) and self._is_local_name(expression.target, local_index):
            if expression.operator in ("+=", "-="):
                amount = self._constant_int(expression.value)
                if amount is None:
                    return 0
                return amount if expression.operator == "+=" else -amount
        return 0

    def _find_loop_bound(self, expression: Expression | None, local_index: int) -> tuple[str, int] | None:
        if isinstance(expression, BinaryExpression) and expression.operator == "&&":
            return self._find_loop_bound(expression.left, local_index) or self._find_loop_bound(expression.right, local_index)
        if not isinstance(expression, BinaryExpression) or expression.operator not in ("<", "<=", ">", ">="):
            return None
        if self._is_local_name(expression.left, local_index):
            limit = self._constant_int(expression.right)
            return (expression.operator, limit) if limit is not None else None
        if self._is_local_name(expression.right, local_index):
            limit = self._constant_int(expression.left)
            if limit is None:
                return None
            flipped = {"<": ">", "<=": ">=", ">": "<", ">=": "<="}[expression.operator]
            return flipped, limit
        return None

    @staticmethod
    def _iteration_count(start: int, step: int, operator: str, limit: int) -> int | None:
        if step > 0:
            if operator == "<":
                return 0 if start >= limit else (limit - start + step - 1) // step
            if operator == "<=":
                return 0 if start > limit else (limit - start) // step + 1
            return None
        magnitude = -step
        if operator == ">":
            return 0 if start <= limit else (start - limit + magnitude - 1) // magnitude
        if operator == ">=":
            return 0 if start < limit else (start - limit) // magnitude + 1
        return None

    def _constant_int(self, expression: Expression) -> int | None:
        if isinstance(expression, IntLiteral):
            return expression.value
        if isinstance(expression, UnaryExpression) and expression.operator in ("+", "-"):
            value = self._constant_int(expression.operand)
            if value is not None:
                return value if expression.operator == "+" else -value
        if isinstance(expression, CallExpression) and expression.name == "min" and len(expression.arguments) == 2:
            left, right = expression.arguments
            left_value = self._constant_int(left)
            right_value = self._constant_int(right)
            if left_value is not None and right_value is not None:
                return min(left_value, right_value)
            if self._is_iframe(left) and right_value == 0:
                return 0
            if self._is_iframe(right) and left_value == 0:
                return 0
        return None

    @staticmethod
    def _is_iframe(expression: Expression) -> bool:
        return (
            isinstance(expression, NameExpression)
            and expression.name == "iFrame"
            and expression.resolved_symbol_kind == "uniform"
        )

    @staticmethod
    def _is_local_name(expression: Expression, local_index: int) -> bool:
        return (
            isinstance(expression, NameExpression)
            and expression.resolved_symbol_kind == "local"
            and expression.resolved_symbol_index == local_index
        )

    def _reject_recursion(self) -> None:
        state: dict[int, int] = {}
        stack: list[int] = []

        def visit(function_id: int) -> None:
            marker = state.get(function_id, 0)
            if marker == 2:
                return
            if marker == 1:
                cycle_start = stack.index(function_id)
                names = [self.function_by_id[item].name for item in stack[cycle_start:]]
                names.append(self.function_by_id[function_id].name)
                function = self.function_by_id[function_id]
                fail(self.source, function.declaration.span, f"recursive shader calls are not supported: {' -> '.join(names)}")
            state[function_id] = 1
            stack.append(function_id)
            for callee in self.call_graph[function_id]:
                visit(callee)
            stack.pop()
            state[function_id] = 2

        for function_id in self.function_by_id:
            visit(function_id)

    @staticmethod
    def _always_returns(statement: Statement) -> bool:
        if isinstance(statement, ReturnStatement):
            return True
        if isinstance(statement, Block):
            return any(SemanticAnalyzer._always_returns(child) for child in statement.statements)
        if isinstance(statement, IfStatement):
            return (
                statement.else_branch is not None
                and SemanticAnalyzer._always_returns(statement.then_branch)
                and SemanticAnalyzer._always_returns(statement.else_branch)
            )
        return False

    @staticmethod
    def _format_signature(name: str, parameter_types: tuple[ShaderType, ...]) -> str:
        return f"{name}({', '.join(str(item) for item in parameter_types)})"

    @staticmethod
    def _binary_result_type(operator: str, left: ShaderType, right: ShaderType) -> ShaderType | None:
        if operator in ("+", "-", "*", "/"):
            if left.is_matrix or right.is_matrix:
                if left.is_matrix and right == ShaderType.FLOAT:
                    return left
                if right.is_matrix and left == ShaderType.FLOAT:
                    return right
                if left.is_matrix and right.is_matrix:
                    if left != right:
                        return None
                    return left
                if operator == "*" and left.is_matrix and right.is_vector:
                    return right if left.matrix_size == right.component_count else None
                if operator == "*" and left.is_vector and right.is_matrix:
                    return left if right.matrix_size == left.component_count else None
                return None

            if left == right:
                if left == ShaderType.BOOL:
                    return None
                return left
            if left == ShaderType.FLOAT and right.is_vector:
                return right
            if right == ShaderType.FLOAT and left.is_vector:
                return left
            return None

        if operator in ("&", "|", "^", "<<", ">>"):
            return ShaderType.INT if left == ShaderType.INT and right == ShaderType.INT else None

        if operator in ("<", "<=", ">", ">="):
            if left == right and left.is_numeric_scalar:
                return ShaderType.BOOL
            return None

        if operator in ("==", "!="):
            return ShaderType.BOOL if left == right else None

        if operator in ("&&", "||"):
            return ShaderType.BOOL if left == ShaderType.BOOL and right == ShaderType.BOOL else None

        return None
