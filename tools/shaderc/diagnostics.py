from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .tokens import SourceSpan


@dataclass(frozen=True)
class SourceFile:
    path: Path
    text: str


class ShaderCompilerError(Exception):
    def __init__(self, source: SourceFile, span: SourceSpan, message: str):
        super().__init__(message)
        self.source = source
        self.span = span
        self.message = message

    def format(self) -> str:
        start = self.span.start
        end = self.span.end
        lines = self.source.text.splitlines()
        line_text = lines[start.line - 1] if 0 < start.line <= len(lines) else ""

        caret_start = max(start.column - 1, 0)
        if end.line == start.line:
            caret_width = max(end.column - start.column, 1)
        else:
            caret_width = max(len(line_text) - caret_start, 1)

        relative_path = self.source.path.as_posix()
        return (
            f"{relative_path}:{start.line}:{start.column}: error: {self.message}\n\n"
            f"    {line_text}\n"
            f"    {' ' * caret_start}{'^' * caret_width}"
        )


def fail(source: SourceFile, span: SourceSpan, message: str) -> None:
    raise ShaderCompilerError(source, span, message)
