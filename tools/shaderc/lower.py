from __future__ import annotations

from .ast_nodes import (
    AssignmentExpression,
    BinaryExpression,
    Block,
    BoolLiteral,
    BreakStatement,
    CallExpression,
    ConstructorExpression,
    Expression,
    ExpressionStatement,
    FloatLiteral,
    ForStatement,
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
    IRFunction,
    IRIf,
    IRInstruction,
    IRIntConstant,
    IRLoadBuiltin,
    IRLoadUniform,
    IRLoadVariable,
    IRLogical,
    IRModule,
    IRParameter,
    IRReturn,
    IRStoreVariable,
    IRStoreSwizzle,
    IRSwizzle,
    IRUnary,
    IRUniform,
    IRValue,
)
from .shader_types import ShaderType


_BUILTINS = {
    "source_color",
    "uv",
    "local_position",
    "surface_size",
}

_SWIZZLE_INDEX = {
    "x": 0, "r": 0, "s": 0,
    "y": 1, "g": 1, "t": 1,
    "z": 2, "b": 2, "p": 2,
    "w": 3, "a": 3, "q": 3,
}


class Lowerer:
    def __init__(self, program: Program):
        self.program = program
        self.uniform_indices = {uniform.name: index for index, uniform in enumerate(program.uniforms)}
        self.instructions: list[IRInstruction] = []
        self.next_value_id = 0

    def lower(self) -> IRModule:
        uniforms = [IRUniform(index, uniform.name, uniform.type) for index, uniform in enumerate(self.program.uniforms)]
        functions = [self._lower_function(function) for function in self.program.functions]
        main = next(function for function in self.program.functions if function.name == "main")
        assert main.resolved_id is not None
        return IRModule(uniforms, functions, main.resolved_id)

    def _lower_function(self, function) -> IRFunction:
        assert function.resolved_id is not None
        self.instructions = []
        self.next_value_id = 0
        parameters = [
            IRParameter(index, parameter.name, parameter.type)
            for index, parameter in enumerate(function.parameters)
        ]
        body = self._lower_statement_to_block(function.body)
        return IRFunction(function.resolved_id, function.name, function.return_type, parameters, body)

    def _lower_statement_to_block(self, statement: Statement) -> IRBlock:
        previous = self.instructions
        self.instructions = []
        self._lower_statement(statement)
        block = IRBlock(self.instructions)
        self.instructions = previous
        return block

    def _lower_expression_to_block(self, expression: Expression) -> IRBlock:
        previous = self.instructions
        self.instructions = []
        value = self._lower_expression(expression)
        block = IRBlock(self.instructions, value)
        self.instructions = previous
        return block

    def _lower_statement(self, statement: Statement) -> None:
        if isinstance(statement, Block):
            for child in statement.statements:
                self._lower_statement(child)
            return

        if isinstance(statement, VariableDecl):
            assert statement.resolved_local_index is not None
            initializer = self._lower_expression(statement.initializer) if statement.initializer is not None else None
            self.instructions.append(
                IRDeclareLocal(
                    None,
                    statement.resolved_local_index,
                    statement.name,
                    statement.type,
                    initializer,
                    statement.is_const,
                )
            )
            return

        if isinstance(statement, VariableDeclGroup):
            for declaration in statement.declarations:
                self._lower_statement(declaration)
            return

        if isinstance(statement, ExpressionStatement):
            self._lower_expression(statement.expression)
            return

        if isinstance(statement, ReturnStatement):
            value = self._lower_expression(statement.expression)
            self.instructions.append(IRReturn(None, value))
            return

        if isinstance(statement, IfStatement):
            condition = self._lower_expression(statement.condition)
            then_block = self._lower_statement_to_block(statement.then_branch)
            else_block = (
                self._lower_statement_to_block(statement.else_branch)
                if statement.else_branch is not None
                else None
            )
            self.instructions.append(IRIf(None, condition, then_block, else_block))
            return

        if isinstance(statement, ForStatement):
            assert statement.condition is not None
            assert statement.increment is not None
            assert statement.max_iterations is not None
            initializer = (
                self._lower_statement_to_block(statement.initializer)
                if statement.initializer is not None
                else IRBlock([])
            )
            condition = self._lower_expression_to_block(statement.condition)
            increment = self._lower_expression_to_block(statement.increment)
            body = self._lower_statement_to_block(statement.body)
            self.instructions.append(
                IRFor(None, initializer, condition, increment, body, statement.max_iterations)
            )
            return

        if isinstance(statement, BreakStatement):
            self.instructions.append(IRBreak(None))
            return

        raise AssertionError(f"unhandled statement type: {type(statement).__name__}")

    def _lower_expression(self, expression: Expression) -> IRValue:
        assert expression.resolved_type is not None

        if isinstance(expression, FloatLiteral):
            result = self._new_value(ShaderType.FLOAT)
            self.instructions.append(IRConstant(result, expression.text))
            return result

        if isinstance(expression, IntLiteral):
            result = self._new_value(ShaderType.INT)
            self.instructions.append(IRIntConstant(result, expression.value))
            return result

        if isinstance(expression, BoolLiteral):
            result = self._new_value(ShaderType.BOOL)
            self.instructions.append(IRBoolConstant(result, expression.value))
            return result

        if isinstance(expression, NameExpression):
            result = self._new_value(expression.resolved_type)
            kind = expression.resolved_symbol_kind
            index = expression.resolved_symbol_index
            if kind == "builtin":
                self.instructions.append(IRLoadBuiltin(result, expression.name))
            elif kind == "uniform":
                assert index is not None
                self.instructions.append(IRLoadUniform(result, index))
            elif kind in ("local", "parameter"):
                assert index is not None
                self.instructions.append(IRLoadVariable(result, kind, index))
            else:
                raise AssertionError(f"unresolved name '{expression.name}'")
            return result

        if isinstance(expression, UnaryExpression):
            operand = self._lower_expression(expression.operand)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRUnary(result, expression.operator, operand))
            return result

        if isinstance(expression, BinaryExpression):
            left = self._lower_expression(expression.left)
            if expression.operator in ("&&", "||"):
                right = self._lower_expression_to_block(expression.right)
                result = self._new_value(expression.resolved_type)
                self.instructions.append(IRLogical(result, expression.operator, left, right))
                return result
            right = self._lower_expression(expression.right)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRBinary(result, expression.operator, left, right))
            return result

        if isinstance(expression, AssignmentExpression):
            kind, index, components = self._lvalue_info(expression.target)
            if expression.operator == "=":
                value = self._lower_expression(expression.value)
                self._store_lvalue(kind, index, components, value)
                return value

            current = self._lower_expression(expression.target)
            rhs = self._lower_expression(expression.value)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRBinary(result, expression.operator[0], current, rhs))
            self._store_lvalue(kind, index, components, result)
            return result

        if isinstance(expression, UpdateExpression):
            kind, index, components = self._lvalue_info(expression.operand)
            old = self._lower_expression(expression.operand)
            one = self._new_value(expression.resolved_type)
            if expression.resolved_type == ShaderType.INT:
                self.instructions.append(IRIntConstant(one, 1))
            else:
                self.instructions.append(IRConstant(one, "1.0"))
            new = self._new_value(expression.resolved_type)
            operator = "+" if expression.operator == "++" else "-"
            self.instructions.append(IRBinary(new, operator, old, one))
            self._store_lvalue(kind, index, components, new)
            return new if expression.prefix else old

        if isinstance(expression, TernaryExpression):
            condition = self._lower_expression(expression.condition)
            when_true = self._lower_expression_to_block(expression.when_true)
            when_false = self._lower_expression_to_block(expression.when_false)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRConditional(result, condition, when_true, when_false))
            return result

        if isinstance(expression, CallExpression):
            assert expression.resolved_function_id is not None
            arguments = tuple(self._lower_expression(argument) for argument in expression.arguments)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRCall(result, expression.resolved_function_id, arguments))
            return result

        if isinstance(expression, ConstructorExpression):
            arguments = tuple(self._lower_expression(argument) for argument in expression.arguments)
            if expression.resolved_type.is_scalar:
                operand = arguments[0]
                if operand.type == expression.resolved_type:
                    return operand
                result = self._new_value(expression.resolved_type)
                self.instructions.append(IRCast(result, operand))
                return result
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRConstruct(result, arguments))
            return result

        if isinstance(expression, SwizzleExpression):
            base = self._lower_expression(expression.base)
            result = self._new_value(expression.resolved_type)
            components = tuple(_SWIZZLE_INDEX[field] for field in expression.fields)
            self.instructions.append(IRSwizzle(result, base, components))
            return result

        raise AssertionError(f"unhandled expression type: {type(expression).__name__}")


    def _lvalue_info(self, expression: Expression) -> tuple[str, int, tuple[int, ...] | None]:
        if isinstance(expression, NameExpression):
            kind = expression.resolved_symbol_kind
            index = expression.resolved_symbol_index
            assert kind in ("local", "parameter") and index is not None
            return kind, index, None
        if isinstance(expression, SwizzleExpression):
            assert isinstance(expression.base, NameExpression)
            kind = expression.base.resolved_symbol_kind
            index = expression.base.resolved_symbol_index
            assert kind in ("local", "parameter") and index is not None
            return kind, index, tuple(_SWIZZLE_INDEX[field] for field in expression.fields)
        raise AssertionError(f"unsupported l-value type: {type(expression).__name__}")

    def _store_lvalue(
        self,
        kind: str,
        index: int,
        components: tuple[int, ...] | None,
        value: IRValue,
    ) -> None:
        if components is None:
            self.instructions.append(IRStoreVariable(None, kind, index, value))
        else:
            self.instructions.append(IRStoreSwizzle(None, kind, index, components, value))

    def _new_value(self, shader_type: ShaderType) -> IRValue:
        value = IRValue(self.next_value_id, shader_type)
        self.next_value_id += 1
        return value
