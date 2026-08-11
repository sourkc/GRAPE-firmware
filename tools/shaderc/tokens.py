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
    KW_FLOAT = auto()
    KW_VEC4 = auto()
    KW_RETURN = auto()

    LEFT_PAREN = auto()
    RIGHT_PAREN = auto()
    LEFT_BRACE = auto()
    RIGHT_BRACE = auto()
    SEMICOLON = auto()

    PLUS = auto()
    MINUS = auto()
    STAR = auto()
    SLASH = auto()


@dataclass(frozen=True)
class Token:
    kind: TokenKind
    text: str
    span: SourceSpan
