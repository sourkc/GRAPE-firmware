from __future__ import annotations

from .ast_nodes import BinaryExpression, Expression, FloatLiteral, NameExpression, Program, ReturnStatement, UnaryExpression
from .ir import (
    IRBinary,
    IRConstant,
    IRFunction,
    IRInstruction,
    IRLoadBuiltin,
    IRLoadUniform,
    IRModule,
    IRReturn,
    IRUnary,
    IRUniform,
    IRValue,
)


class Lowerer:
    def __init__(self, program: Program):
        self.program = program
        self.instructions: list[IRInstruction] = []
        self.next_value_id = 0
        self.uniform_indices = {uniform.name: index for index, uniform in enumerate(program.uniforms)}

    def lower(self) -> IRModule:
        uniforms = [IRUniform(index, uniform.name, uniform.type) for index, uniform in enumerate(self.program.uniforms)]
        statement = self.program.main.body.statements[0]
        assert isinstance(statement, ReturnStatement)
        value = self._lower_expression(statement.expression)
        self.instructions.append(IRReturn(None, value))
        main = IRFunction(self.program.main.name, self.program.main.return_type, self.instructions)
        return IRModule(uniforms, main)

    def _lower_expression(self, expression: Expression) -> IRValue:
        assert expression.resolved_type is not None

        if isinstance(expression, FloatLiteral):
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRConstant(result, expression.text))
            return result

        if isinstance(expression, NameExpression):
            result = self._new_value(expression.resolved_type)
            if expression.name == "source_color":
                self.instructions.append(IRLoadBuiltin(result, expression.name))
            else:
                self.instructions.append(IRLoadUniform(result, self.uniform_indices[expression.name]))
            return result

        if isinstance(expression, UnaryExpression):
            operand = self._lower_expression(expression.operand)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRUnary(result, expression.operator, operand))
            return result

        if isinstance(expression, BinaryExpression):
            left = self._lower_expression(expression.left)
            right = self._lower_expression(expression.right)
            result = self._new_value(expression.resolved_type)
            self.instructions.append(IRBinary(result, expression.operator, left, right))
            return result

        raise AssertionError(f"unhandled expression type: {type(expression).__name__}")

    def _new_value(self, shader_type):
        value = IRValue(self.next_value_id, shader_type)
        self.next_value_id += 1
        return value
