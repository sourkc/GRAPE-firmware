from __future__ import annotations

from .ast_nodes import (
    AssignmentExpression,
    BinaryExpression,
    Block,
    BoolLiteral,
    BreakStatement,
    CallExpression,
    ConstructorExpression,
    EmptyStatement,
    ExpressionStatement,
    FloatLiteral,
    ForStatement,
    FunctionDecl,
    FunctionParameter,
    IfStatement,
    IntLiteral,
    NameExpression,
    Program,
    ReturnStatement,
    SwizzleExpression,
    TernaryExpression,
    UnaryExpression,
    UniformDecl,
    UpdateExpression,
    VariableDecl,
    VariableDeclGroup,
)
from .diagnostics import SourceFile, fail
from .shader_types import ShaderType
from .tokens import SourceSpan, Token, TokenKind


_BINARY_PRECEDENCE = {
    TokenKind.OR_OR: 10,
    TokenKind.AND_AND: 20,
    TokenKind.PIPE: 23,
    TokenKind.CARET: 24,
    TokenKind.AMPERSAND: 25,
    TokenKind.EQUAL_EQUAL: 30,
    TokenKind.BANG_EQUAL: 30,
    TokenKind.LESS: 40,
    TokenKind.LESS_EQUAL: 40,
    TokenKind.GREATER: 40,
    TokenKind.GREATER_EQUAL: 40,
    TokenKind.LEFT_SHIFT: 45,
    TokenKind.RIGHT_SHIFT: 45,
    TokenKind.PLUS: 50,
    TokenKind.MINUS: 50,
    TokenKind.STAR: 60,
    TokenKind.SLASH: 60,
}

_ASSIGNMENT_TOKENS = {
    TokenKind.EQUAL,
    TokenKind.PLUS_EQUAL,
    TokenKind.MINUS_EQUAL,
    TokenKind.STAR_EQUAL,
    TokenKind.SLASH_EQUAL,
}

_TYPE_TOKENS = {
    TokenKind.KW_VOID: ShaderType.VOID,
    TokenKind.KW_BOOL: ShaderType.BOOL,
    TokenKind.KW_INT: ShaderType.INT,
    TokenKind.KW_FLOAT: ShaderType.FLOAT,
    TokenKind.KW_VEC2: ShaderType.VEC2,
    TokenKind.KW_VEC3: ShaderType.VEC3,
    TokenKind.KW_VEC4: ShaderType.VEC4,
    TokenKind.KW_MAT2: ShaderType.MAT2,
    TokenKind.KW_MAT3: ShaderType.MAT3,
}


class Parser:
    def __init__(self, source: SourceFile, tokens: list[Token]):
        self.source = source
        self.tokens = tokens
        self.index = 0

    def parse(self) -> Program:
        uniforms: list[UniformDecl] = []
        functions: list[FunctionDecl] = []
        start = self._current().span.start
        end = start

        while not self._check(TokenKind.EOF):
            if self._check(TokenKind.KW_UNIFORM):
                uniform = self._parse_uniform()
                uniforms.append(uniform)
                end = uniform.span.end
                continue

            if self._current().kind in _TYPE_TOKENS:
                function = self._parse_function()
                functions.append(function)
                end = function.span.end
                continue

            fail(self.source, self._current().span, "expected uniform declaration or function definition")

        self._expect(TokenKind.EOF, "expected end of shader")
        if not functions:
            fail(self.source, self._current().span, "shader must define main()")
        return Program(SourceSpan(start, end), uniforms, functions)

    def _parse_uniform(self) -> UniformDecl:
        start = self._expect(TokenKind.KW_UNIFORM, "expected 'uniform'").span.start
        shader_type = self._parse_type()
        if shader_type == ShaderType.VOID:
            fail(self.source, self._current().span, "uniform cannot have type void")
        name = self._expect(TokenKind.IDENTIFIER, "expected uniform name")
        end = self._expect(TokenKind.SEMICOLON, "expected ';' after uniform declaration").span.end
        return UniformDecl(SourceSpan(start, end), shader_type, name.text)

    def _parse_function(self) -> FunctionDecl:
        start = self._current().span.start
        return_type = self._parse_type()
        name = self._expect(TokenKind.IDENTIFIER, "expected function name")
        self._expect(TokenKind.LEFT_PAREN, "expected '(' after function name")

        parameters: list[FunctionParameter] = []
        if not self._check(TokenKind.RIGHT_PAREN):
            while True:
                parameter_start = self._current().span.start
                qualifier = "in"
                if self._current().kind in (TokenKind.KW_IN, TokenKind.KW_OUT):
                    qualifier = self._advance().text
                parameter_type = self._parse_type()
                if parameter_type == ShaderType.VOID:
                    fail(self.source, self._current().span, "function parameter cannot have type void")
                parameter_name = self._expect(TokenKind.IDENTIFIER, "expected parameter name")
                parameters.append(
                    FunctionParameter(
                        SourceSpan(parameter_start, parameter_name.span.end),
                        parameter_type,
                        parameter_name.text,
                        qualifier,
                    )
                )
                if not self._check(TokenKind.COMMA):
                    break
                self._advance()

        self._expect(TokenKind.RIGHT_PAREN, "expected ')' after function parameters")
        body = self._parse_block()
        return FunctionDecl(SourceSpan(start, body.span.end), return_type, name.text, parameters, body)

    def _parse_block(self) -> Block:
        start = self._expect(TokenKind.LEFT_BRACE, "expected '{'").span.start
        statements = []
        while not self._check(TokenKind.RIGHT_BRACE):
            if self._check(TokenKind.EOF):
                fail(self.source, self._current().span, "expected '}' before end of shader")
            statements.append(self._parse_statement())
        end = self._expect(TokenKind.RIGHT_BRACE, "expected '}'").span.end
        return Block(SourceSpan(start, end), statements)

    def _parse_statement(self):
        if self._check(TokenKind.SEMICOLON):
            token = self._advance()
            return EmptyStatement(token.span)
        if self._check(TokenKind.LEFT_BRACE):
            return self._parse_block()
        if self._check(TokenKind.KW_RETURN):
            return self._parse_return_statement()
        if self._check(TokenKind.KW_IF):
            return self._parse_if_statement()
        if self._check(TokenKind.KW_FOR):
            return self._parse_for_statement()
        if self._check(TokenKind.KW_BREAK):
            start = self._advance().span.start
            end = self._expect(TokenKind.SEMICOLON, "expected ';' after break").span.end
            return BreakStatement(SourceSpan(start, end))
        if self._check(TokenKind.KW_CONST) or self._current().kind in _TYPE_TOKENS:
            return self._parse_variable_declaration_statement(expect_semicolon=True)

        expression = self._parse_expression()
        end = self._expect(TokenKind.SEMICOLON, "expected ';' after expression").span.end
        return ExpressionStatement(SourceSpan(expression.span.start, end), expression)

    def _parse_return_statement(self) -> ReturnStatement:
        start = self._expect(TokenKind.KW_RETURN, "expected 'return'").span.start
        expression = self._parse_expression()
        end = self._expect(TokenKind.SEMICOLON, "expected ';' after return value").span.end
        return ReturnStatement(SourceSpan(start, end), expression)

    def _parse_if_statement(self) -> IfStatement:
        start = self._expect(TokenKind.KW_IF, "expected 'if'").span.start
        self._expect(TokenKind.LEFT_PAREN, "expected '(' after if")
        condition = self._parse_expression()
        self._expect(TokenKind.RIGHT_PAREN, "expected ')' after if condition")
        then_branch = self._parse_statement()
        else_branch = None
        end = then_branch.span.end
        if self._check(TokenKind.KW_ELSE):
            self._advance()
            else_branch = self._parse_statement()
            end = else_branch.span.end
        return IfStatement(SourceSpan(start, end), condition, then_branch, else_branch)

    def _parse_for_statement(self) -> ForStatement:
        start = self._expect(TokenKind.KW_FOR, "expected 'for'").span.start
        self._expect(TokenKind.LEFT_PAREN, "expected '(' after for")

        initializer = None
        if not self._check(TokenKind.SEMICOLON):
            if self._check(TokenKind.KW_CONST) or self._current().kind in _TYPE_TOKENS:
                initializer = self._parse_variable_declaration_statement(expect_semicolon=False)
            else:
                expression = self._parse_expression()
                initializer = ExpressionStatement(expression.span, expression)
        self._expect(TokenKind.SEMICOLON, "expected ';' after for initializer")

        condition = None
        if not self._check(TokenKind.SEMICOLON):
            condition = self._parse_expression()
        self._expect(TokenKind.SEMICOLON, "expected ';' after for condition")

        increment = None
        if not self._check(TokenKind.RIGHT_PAREN):
            increment = self._parse_expression()
        self._expect(TokenKind.RIGHT_PAREN, "expected ')' after for clauses")
        body = self._parse_statement()
        return ForStatement(SourceSpan(start, body.span.end), initializer, condition, increment, body)

    def _parse_variable_declaration_statement(self, *, expect_semicolon: bool):
        start = self._current().span.start
        is_const = False
        if self._check(TokenKind.KW_CONST):
            self._advance()
            is_const = True
        shader_type = self._parse_type()
        if shader_type == ShaderType.VOID:
            fail(self.source, self._current().span, "local variable cannot have type void")
        declarations = []
        while True:
            name = self._expect(TokenKind.IDENTIFIER, "expected variable name")
            initializer = None
            end = name.span.end
            if self._check(TokenKind.EQUAL):
                self._advance()
                initializer = self._parse_expression()
                end = initializer.span.end
            declarations.append(
                VariableDecl(SourceSpan(name.span.start, end), shader_type, name.text, initializer, is_const)
            )
            if not self._check(TokenKind.COMMA):
                break
            self._advance()
        if expect_semicolon:
            end = self._expect(TokenKind.SEMICOLON, "expected ';' after variable declaration").span.end
        if len(declarations) == 1:
            declaration = declarations[0]
            declaration.span = SourceSpan(start, end)
            return declaration
        return VariableDeclGroup(SourceSpan(start, end), declarations)

    def _parse_expression(self):
        return self._parse_assignment()

    def _parse_assignment(self):
        left = self._parse_ternary()
        if self._current().kind in _ASSIGNMENT_TOKENS:
            operator = self._advance()
            right = self._parse_assignment()
            return AssignmentExpression(SourceSpan(left.span.start, right.span.end), left, operator.text, right)
        return left

    def _parse_ternary(self):
        condition = self._parse_binary(0)
        if not self._check(TokenKind.QUESTION):
            return condition
        self._advance()
        when_true = self._parse_expression()
        self._expect(TokenKind.COLON, "expected ':' in conditional expression")
        when_false = self._parse_assignment()
        return TernaryExpression(
            SourceSpan(condition.span.start, when_false.span.end),
            condition,
            when_true,
            when_false,
        )

    def _parse_binary(self, min_precedence: int):
        left = self._parse_postfix()

        while True:
            token = self._current()
            precedence = _BINARY_PRECEDENCE.get(token.kind)
            if precedence is None or precedence < min_precedence:
                break
            operator = self._advance()
            right = self._parse_binary(precedence + 1)
            left = BinaryExpression(SourceSpan(left.span.start, right.span.end), left, operator.text, right)

        return left

    def _parse_postfix(self):
        expression = self._parse_prefix()
        while True:
            if self._check(TokenKind.LEFT_PAREN):
                if not isinstance(expression, NameExpression):
                    fail(self.source, expression.span, "only named functions can be called in shader language 0.7")
                start = expression.span.start
                self._advance()
                arguments = []
                if not self._check(TokenKind.RIGHT_PAREN):
                    while True:
                        arguments.append(self._parse_expression())
                        if not self._check(TokenKind.COMMA):
                            break
                        self._advance()
                end = self._expect(TokenKind.RIGHT_PAREN, "expected ')' after function arguments").span.end
                expression = CallExpression(SourceSpan(start, end), expression.name, arguments)
                continue

            if self._check(TokenKind.DOT):
                self._advance()
                fields = self._expect(TokenKind.IDENTIFIER, "expected component selection after '.'")
                expression = SwizzleExpression(SourceSpan(expression.span.start, fields.span.end), expression, fields.text)
                continue

            if self._current().kind in (TokenKind.PLUS_PLUS, TokenKind.MINUS_MINUS):
                operator = self._advance()
                expression = UpdateExpression(
                    SourceSpan(expression.span.start, operator.span.end),
                    expression,
                    operator.text,
                    False,
                )
                continue
            break
        return expression

    def _parse_prefix(self):
        token = self._current()

        if token.kind in (
            TokenKind.PLUS,
            TokenKind.MINUS,
            TokenKind.BANG,
            TokenKind.PLUS_PLUS,
            TokenKind.MINUS_MINUS,
        ):
            operator = self._advance()
            operand = self._parse_postfix()
            if operator.kind in (TokenKind.PLUS_PLUS, TokenKind.MINUS_MINUS):
                return UpdateExpression(SourceSpan(operator.span.start, operand.span.end), operand, operator.text, True)
            return UnaryExpression(SourceSpan(operator.span.start, operand.span.end), operator.text, operand)

        if token.kind == TokenKind.NUMBER:
            self._advance()
            if "." in token.text or "e" in token.text.lower():
                return FloatLiteral(token.span, token.text, float(token.text))
            return IntLiteral(token.span, token.text, int(token.text, 10))

        if token.kind in (TokenKind.KW_TRUE, TokenKind.KW_FALSE):
            self._advance()
            return BoolLiteral(token.span, token.kind == TokenKind.KW_TRUE)

        if token.kind == TokenKind.IDENTIFIER:
            self._advance()
            return NameExpression(token.span, token.text)

        if token.kind in _TYPE_TOKENS and token.kind != TokenKind.KW_VOID:
            return self._parse_constructor()

        if token.kind == TokenKind.LEFT_PAREN:
            start = self._advance().span.start
            expression = self._parse_expression()
            end = self._expect(TokenKind.RIGHT_PAREN, "expected ')' after expression").span.end
            expression.span = SourceSpan(start, end)
            return expression

        fail(self.source, token.span, "expected expression")

    def _parse_constructor(self) -> ConstructorExpression:
        start = self._current().span.start
        target_type = self._parse_type()
        self._expect(TokenKind.LEFT_PAREN, "expected '(' after type name")
        arguments = []
        if not self._check(TokenKind.RIGHT_PAREN):
            while True:
                arguments.append(self._parse_expression())
                if not self._check(TokenKind.COMMA):
                    break
                self._advance()
        end = self._expect(TokenKind.RIGHT_PAREN, "expected ')' after constructor arguments").span.end
        return ConstructorExpression(SourceSpan(start, end), target_type, arguments)

    def _parse_type(self) -> ShaderType:
        token = self._current()
        shader_type = _TYPE_TOKENS.get(token.kind)
        if shader_type is None:
            fail(self.source, token.span, "expected shader type")
        self._advance()
        return shader_type

    def _expect(self, kind: TokenKind, message: str) -> Token:
        token = self._current()
        if token.kind != kind:
            fail(self.source, token.span, message)
        return self._advance()

    def _check(self, kind: TokenKind) -> bool:
        return self._current().kind == kind

    def _current(self) -> Token:
        return self.tokens[self.index]

    def _advance(self) -> Token:
        token = self.tokens[self.index]
        self.index += 1
        return token
