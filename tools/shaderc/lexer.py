from __future__ import annotations

from .diagnostics import SourceFile, fail
from .tokens import SourceLocation, SourceSpan, Token, TokenKind


_KEYWORDS = {
    "uniform": TokenKind.KW_UNIFORM,
    "float": TokenKind.KW_FLOAT,
    "vec4": TokenKind.KW_VEC4,
    "return": TokenKind.KW_RETURN,
}

_SINGLE_CHAR_TOKENS = {
    "(": TokenKind.LEFT_PAREN,
    ")": TokenKind.RIGHT_PAREN,
    "{": TokenKind.LEFT_BRACE,
    "}": TokenKind.RIGHT_BRACE,
    ";": TokenKind.SEMICOLON,
    "+": TokenKind.PLUS,
    "-": TokenKind.MINUS,
    "*": TokenKind.STAR,
    "/": TokenKind.SLASH,
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
