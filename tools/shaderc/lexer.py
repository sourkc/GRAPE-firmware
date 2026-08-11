from __future__ import annotations

from .diagnostics import SourceFile, fail
from .tokens import SourceLocation, SourceSpan, Token, TokenKind


_KEYWORDS = {
    "uniform": TokenKind.KW_UNIFORM,
    "const": TokenKind.KW_CONST,
    "in": TokenKind.KW_IN,
    "bool": TokenKind.KW_BOOL,
    "int": TokenKind.KW_INT,
    "float": TokenKind.KW_FLOAT,
    "vec2": TokenKind.KW_VEC2,
    "vec3": TokenKind.KW_VEC3,
    "vec4": TokenKind.KW_VEC4,
    "mat2": TokenKind.KW_MAT2,
    "mat3": TokenKind.KW_MAT3,
    "return": TokenKind.KW_RETURN,
    "if": TokenKind.KW_IF,
    "else": TokenKind.KW_ELSE,
    "for": TokenKind.KW_FOR,
    "break": TokenKind.KW_BREAK,
    "true": TokenKind.KW_TRUE,
    "false": TokenKind.KW_FALSE,
}

_DOUBLE_CHAR_TOKENS = {
    "++": TokenKind.PLUS_PLUS,
    "--": TokenKind.MINUS_MINUS,
    "+=": TokenKind.PLUS_EQUAL,
    "-=": TokenKind.MINUS_EQUAL,
    "*=": TokenKind.STAR_EQUAL,
    "/=": TokenKind.SLASH_EQUAL,
    "==": TokenKind.EQUAL_EQUAL,
    "!=": TokenKind.BANG_EQUAL,
    "<=": TokenKind.LESS_EQUAL,
    ">=": TokenKind.GREATER_EQUAL,
    "&&": TokenKind.AND_AND,
    "||": TokenKind.OR_OR,
}

_SINGLE_CHAR_TOKENS = {
    "(": TokenKind.LEFT_PAREN,
    ")": TokenKind.RIGHT_PAREN,
    "{": TokenKind.LEFT_BRACE,
    "}": TokenKind.RIGHT_BRACE,
    ";": TokenKind.SEMICOLON,
    ",": TokenKind.COMMA,
    ".": TokenKind.DOT,
    "?": TokenKind.QUESTION,
    ":": TokenKind.COLON,
    "+": TokenKind.PLUS,
    "-": TokenKind.MINUS,
    "*": TokenKind.STAR,
    "/": TokenKind.SLASH,
    "!": TokenKind.BANG,
    "=": TokenKind.EQUAL,
    "<": TokenKind.LESS,
    ">": TokenKind.GREATER,
}


class Lexer:
    def __init__(self, source: SourceFile):
        self.source = source
        self.text = source.text
        self.index = 0
        self.line = 1
        self.column = 1

    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []

        while not self._at_end():
            self._skip_ignored()
            if self._at_end():
                break

            start = self._location()
            ch = self._peek()

            if ch.isalpha() or ch == "_":
                text = self._scan_identifier()
                kind = _KEYWORDS.get(text, TokenKind.IDENTIFIER)
                tokens.append(Token(kind, text, SourceSpan(start, self._location())))
                continue

            if ch.isdigit() or (ch == "." and self._peek(1).isdigit()):
                text = self._scan_number(start)
                tokens.append(Token(TokenKind.NUMBER, text, SourceSpan(start, self._location())))
                continue

            pair = ch + self._peek(1)
            kind = _DOUBLE_CHAR_TOKENS.get(pair)
            if kind is not None:
                self._advance()
                self._advance()
                tokens.append(Token(kind, pair, SourceSpan(start, self._location())))
                continue

            kind = _SINGLE_CHAR_TOKENS.get(ch)
            if kind is not None:
                self._advance()
                tokens.append(Token(kind, ch, SourceSpan(start, self._location())))
                continue

            self._advance()
            fail(self.source, SourceSpan(start, self._location()), f"unexpected character {ch!r}")

        location = self._location()
        tokens.append(Token(TokenKind.EOF, "", SourceSpan(location, location)))
        return tokens

    def _skip_ignored(self) -> None:
        while not self._at_end():
            ch = self._peek()

            if ch.isspace():
                self._advance()
                continue

            if ch == "/" and self._peek(1) == "/":
                self._advance()
                self._advance()
                while not self._at_end() and self._peek() != "\n":
                    self._advance()
                continue

            if ch == "/" and self._peek(1) == "*":
                start = self._location()
                self._advance()
                self._advance()
                while not self._at_end():
                    if self._peek() == "*" and self._peek(1) == "/":
                        self._advance()
                        self._advance()
                        break
                    self._advance()
                else:
                    fail(self.source, SourceSpan(start, self._location()), "unterminated block comment")
                continue

            break

    def _scan_identifier(self) -> str:
        start = self.index
        while not self._at_end() and (self._peek().isalnum() or self._peek() == "_"):
            self._advance()
        return self.text[start:self.index]

    def _scan_number(self, start: SourceLocation) -> str:
        begin = self.index

        if self._peek() == ".":
            self._advance()
            self._consume_digits()
        else:
            self._consume_digits()
            if self._peek() == ".":
                self._advance()
                self._consume_digits()

        if self._peek() in ("e", "E"):
            self._advance()
            if self._peek() in ("+", "-"):
                self._advance()
            if not self._peek().isdigit():
                fail(self.source, SourceSpan(start, self._location()), "expected exponent digits")
            self._consume_digits()

        if self._peek().isalpha() or self._peek() == "_":
            while not self._at_end() and (self._peek().isalnum() or self._peek() == "_"):
                self._advance()
            fail(self.source, SourceSpan(start, self._location()), "invalid numeric literal")

        return self.text[begin:self.index]

    def _consume_digits(self) -> None:
        while not self._at_end() and self._peek().isdigit():
            self._advance()

    def _peek(self, offset: int = 0) -> str:
        index = self.index + offset
        return self.text[index] if index < len(self.text) else "\0"

    def _advance(self) -> str:
        ch = self.text[self.index]
        self.index += 1
        if ch == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return ch

    def _at_end(self) -> bool:
        return self.index >= len(self.text)

    def _location(self) -> SourceLocation:
        return SourceLocation(self.index, self.line, self.column)
