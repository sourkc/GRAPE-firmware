from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto


@dataclass(frozen=True)
class SourceLocation:
    offset: int
    line: int
    column: int


@dataclass(frozen=True)
class SourceSpan:
    start: SourceLocation
    end: SourceLocation


class TokenKind(Enum):
    EOF = auto()
    IDENTIFIER = auto()
    NUMBER = auto()

    KW_UNIFORM = auto()
    KW_CONST = auto()
    KW_IN = auto()
    KW_BOOL = auto()
    KW_INT = auto()
    KW_FLOAT = auto()
    KW_VEC2 = auto()
    KW_VEC3 = auto()
    KW_VEC4 = auto()
    KW_RETURN = auto()
    KW_IF = auto()
    KW_ELSE = auto()
    KW_FOR = auto()
    KW_BREAK = auto()
    KW_TRUE = auto()
    KW_FALSE = auto()

    LEFT_PAREN = auto()
    RIGHT_PAREN = auto()
    LEFT_BRACE = auto()
    RIGHT_BRACE = auto()
    SEMICOLON = auto()
    COMMA = auto()
    DOT = auto()
    QUESTION = auto()
    COLON = auto()

    PLUS = auto()
    MINUS = auto()
    STAR = auto()
    SLASH = auto()
    BANG = auto()
    EQUAL = auto()
    LESS = auto()
    GREATER = auto()

    PLUS_PLUS = auto()
    MINUS_MINUS = auto()
    PLUS_EQUAL = auto()
    MINUS_EQUAL = auto()
    STAR_EQUAL = auto()
    SLASH_EQUAL = auto()
    EQUAL_EQUAL = auto()
    BANG_EQUAL = auto()
    LESS_EQUAL = auto()
    GREATER_EQUAL = auto()
    AND_AND = auto()
    OR_OR = auto()


@dataclass(frozen=True)
class Token:
    kind: TokenKind
    text: str
    span: SourceSpan
