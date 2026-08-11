from __future__ import annotations

from .ast_nodes import (
    BinaryExpression,
    Block,
    FloatLiteral,
    FunctionDecl,
    NameExpression,
    Program,
    ReturnStatement,
    UnaryExpression,
    UniformDecl,
)
from .diagnostics import SourceFile, fail
from .shader_types import ShaderType
from .tokens import SourceSpan, Token, TokenKind


_BINARY_PRECEDENCE = {
    TokenKind.PLUS: 10,
    TokenKind.MINUS: 10,
    TokenKind.STAR: 20,
    TokenKind.SLASH: 20,
}


class Parser:
    def __init__(self, source: SourceFile, tokens: list[Token]):
        self.source = source
        self.tokens = tokens
        self.index = 0

    def parse(self) -> Program:
        uniforms: list[UniformDecl] = []
        main: FunctionDecl | None = None
        start = self._current().span.start
        end = start

        while not self._check(TokenKind.EOF):
            if self._check(TokenKind.KW_UNIFORM):
                uniform = self._parse_uniform()
                uniforms.append(uniform)
                end = uniform.span.end
                continue

            if self._check(TokenKind.KW_FLOAT) or self._check(TokenKind.KW_VEC4):
                if main is not None:
                    fail(self.source, self._current().span, "shader language 0.1 supports exactly one function")
                main = self._parse_function()
                end = main.span.end
                continue

            fail(self.source, self._current().span, "expected uniform declaration or main function")

        self._expect(TokenKind.EOF, "expected end of shader")
        if main is None:
            fail(self.source, self._current().span, "shader must define main()")
        return Program(SourceSpan(start, end), uniforms, main)

    def _parse_uniform(self) -> UniformDecl:
        start = self._expect(TokenKind.KW_UNIFORM, "expected 'uniform'").span.start
        shader_type = self._parse_type()
        name = self._expect(TokenKind.IDENTIFIER, "expected uniform name")
        end = self._expect(TokenKind.SEMICOLON, "expected ';' after uniform declaration").span.end
        return UniformDecl(SourceSpan(start, end), shader_type, name.text)

    def _parse_function(self) -> FunctionDecl:
        start_token = self._current()
        return_type = self._parse_type()
        name = self._expect(TokenKind.IDENTIFIER, "expected function name")
        self._expect(TokenKind.LEFT_PAREN, "expected '(' after function name")
        self._expect(TokenKind.RIGHT_PAREN, "v0.1 main() does not accept parameters")
        body = self._parse_block()
        return FunctionDecl(SourceSpan(start_token.span.start, body.span.end), return_type, name.text, body)

    def _parse_block(self) -> Block:
        start = self._expect(TokenKind.LEFT_BRACE, "expected '{' to begin function body").span.start
        statement = self._parse_return_statement()
        end = self._expect(TokenKind.RIGHT_BRACE, "expected '}' after function body").span.end
        return Block(SourceSpan(start, end), [statement])

    def _parse_return_statement(self) -> ReturnStatement:
        start = self._expect(TokenKind.KW_RETURN, "v0.1 main() must contain a return statement").span.start
        expression = self._parse_expression()
        end = self._expect(TokenKind.SEMICOLON, "expected ';' after return value").span.end
        return ReturnStatement(SourceSpan(start, end), expression)

    def _parse_expression(self, min_precedence: int = 0):
        left = self._parse_prefix()

        while True:
            token = self._current()
            precedence = _BINARY_PRECEDENCE.get(token.kind)
            if precedence is None or precedence < min_precedence:
                break

            operator = self._advance()
            right = self._parse_expression(precedence + 1)
            left = BinaryExpression(
                SourceSpan(left.span.start, right.span.end),
                left,
                operator.text,
                right,
            )

        return left

    def _parse_prefix(self):
        token = self._current()

        if token.kind in (TokenKind.PLUS, TokenKind.MINUS):
            operator = self._advance()
            operand = self._parse_expression(30)
            return UnaryExpression(SourceSpan(operator.span.start, operand.span.end), operator.text, operand)

        if token.kind == TokenKind.NUMBER:
            self._advance()
            if "." not in token.text and "e" not in token.text.lower():
                fail(
                    self.source,
                    token.span,
                    "integer literals are not supported in shader language 0.1; use a floating-point literal such as 1.0",
                )
            return FloatLiteral(token.span, token.text, float(token.text))

        if token.kind == TokenKind.IDENTIFIER:
            self._advance()
            return NameExpression(token.span, token.text)

        if token.kind == TokenKind.LEFT_PAREN:
            start = self._advance().span.start
            expression = self._parse_expression()
            end = self._expect(TokenKind.RIGHT_PAREN, "expected ')' after expression").span.end
            expression.span = SourceSpan(start, end)
            return expression

        fail(self.source, token.span, "expected expression")

    def _parse_type(self) -> ShaderType:
        token = self._current()
        if token.kind == TokenKind.KW_FLOAT:
            self._advance()
            return ShaderType.FLOAT
        if token.kind == TokenKind.KW_VEC4:
            self._advance()
            return ShaderType.VEC4
        fail(self.source, token.span, "expected type 'float' or 'vec4'")

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
