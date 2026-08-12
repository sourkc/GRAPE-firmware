from __future__ import annotations

import ast
import operator
import re
from dataclasses import dataclass

from .diagnostics import SourceFile, fail
from .tokens import SourceLocation, SourceSpan


_IDENTIFIER = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
_DIRECTIVE = re.compile(r"^\s*#\s*([A-Za-z_][A-Za-z0-9_]*)\b(.*)$")
_DEFINE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)(.*)$")


@dataclass
class _Conditional:
    parent_active: bool
    condition_true: bool
    else_seen: bool = False


class Preprocessor:
    def __init__(self, source: SourceFile):
        self.source = source
        self.macros: dict[str, str] = {}
        self.conditionals: list[_Conditional] = []
        self.active = True

    def process(self) -> SourceFile:
        lines = self.source.text.splitlines(keepends=True)
        output: list[str] = []
        offset = 0

        for line_number, line in enumerate(lines, 1):
            match = _DIRECTIVE.match(line)
            if match:
                self._directive(match.group(1), match.group(2).strip(), line_number, offset)
                output.append(_line_ending(line))
            elif self.active:
                output.append(self._expand(line))
            else:
                output.append(_line_ending(line))
            offset += len(line)

        if self.conditionals:
            line_number = max(len(lines), 1)
            self._error(line_number, 1, offset, "unterminated #if block")

        return SourceFile(self.source.path, "".join(output))

    def _directive(
        self,
        name: str,
        argument: str,
        line_number: int,
        line_offset: int,
    ) -> None:
        if not self.active and name not in {"if", "ifdef", "ifndef", "else", "endif"}:
            return

        if name == "define":
            if not self.active:
                return
            match = _DEFINE.match(argument)
            if not match:
                self._error(line_number, 1, line_offset, "expected macro name after #define")
            macro_name = match.group(1)
            tail = match.group(2)
            if tail.startswith("("):
                self._error(
                    line_number,
                    1,
                    line_offset,
                    "function-like macros are not supported",
                )
            replacement = tail.split("//", 1)[0].strip()
            self.macros[macro_name] = replacement or "1"
            return

        if name == "if":
            parent_active = self.active
            condition_true = self._eval_if(argument, line_number, line_offset) if parent_active else False
            self.conditionals.append(_Conditional(parent_active, condition_true))
            self.active = parent_active and condition_true
            return

        if name in {"ifdef", "ifndef"}:
            parent_active = self.active
            macro_name = self._conditional_macro_name(name, argument, line_number, line_offset)
            condition_true = macro_name in self.macros
            if name == "ifndef":
                condition_true = not condition_true
            self.conditionals.append(_Conditional(parent_active, condition_true))
            self.active = parent_active and condition_true
            return

        if name == "else":
            if argument:
                self._error(line_number, 1, line_offset, "unexpected tokens after #else")
            if not self.conditionals:
                self._error(line_number, 1, line_offset, "#else without matching #if")
            conditional = self.conditionals[-1]
            if conditional.else_seen:
                self._error(line_number, 1, line_offset, "duplicate #else")
            conditional.else_seen = True
            self.active = conditional.parent_active and not conditional.condition_true
            return

        if name == "endif":
            if argument:
                self._error(line_number, 1, line_offset, "unexpected tokens after #endif")
            if not self.conditionals:
                self._error(line_number, 1, line_offset, "#endif without matching #if")
            conditional = self.conditionals.pop()
            self.active = conditional.parent_active
            return

        self._error(line_number, 1, line_offset, f"unsupported preprocessor directive '#{name}'")

    def _conditional_macro_name(
        self,
        directive: str,
        argument: str,
        line_number: int,
        line_offset: int,
    ) -> str:
        candidate = argument.split("//", 1)[0].strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", candidate):
            self._error(
                line_number,
                1,
                line_offset,
                f"expected exactly one macro name after #{directive}",
            )
        return candidate

    def _expand(self, text: str) -> str:
        result = text
        for _ in range(64):
            changed = False

            def replace(match: re.Match[str]) -> str:
                nonlocal changed
                replacement = self.macros.get(match.group(0))
                if replacement is None:
                    return match.group(0)
                changed = True
                return replacement

            result = _IDENTIFIER.sub(replace, result)
            if not changed:
                return result
        raise RuntimeError("shader preprocessor macro expansion exceeded 64 passes")

    def _eval_if(self, expression: str, line_number: int, line_offset: int) -> bool:
        expanded = self._expand(expression.split("//", 1)[0])
        expanded = _IDENTIFIER.sub("0", expanded)
        expanded = expanded.replace("&&", " and ").replace("||", " or ")
        expanded = re.sub(r"!(?!=)", " not ", expanded)
        try:
            tree = ast.parse(expanded.strip() or "0", mode="eval")
            value = _eval_constant_expression(tree.body)
        except (SyntaxError, ValueError, ZeroDivisionError):
            self._error(line_number, 1, line_offset, f"invalid #if expression '{expression}'")
        return bool(value)

    def _error(self, line: int, column: int, offset: int, message: str) -> None:
        start = SourceLocation(offset, line, column)
        end = SourceLocation(offset + 1, line, column + 1)
        fail(self.source, SourceSpan(start, end), message)


def _line_ending(line: str) -> str:
    if line.endswith("\r\n"):
        return "\r\n"
    if line.endswith("\n"):
        return "\n"
    if line.endswith("\r"):
        return "\r"
    return ""


_BINARY_OPS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: lambda a, b: int(a / b),
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
    ast.BitAnd: operator.and_,
    ast.BitOr: operator.or_,
    ast.BitXor: operator.xor,
}

_UNARY_OPS = {
    ast.UAdd: operator.pos,
    ast.USub: operator.neg,
    ast.Not: lambda value: int(not value),
    ast.Invert: operator.invert,
}

_COMPARE_OPS = {
    ast.Eq: operator.eq,
    ast.NotEq: operator.ne,
    ast.Lt: operator.lt,
    ast.LtE: operator.le,
    ast.Gt: operator.gt,
    ast.GtE: operator.ge,
}


def _eval_constant_expression(node: ast.AST) -> int:
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, bool)):
        return int(node.value)

    if isinstance(node, ast.UnaryOp) and type(node.op) in _UNARY_OPS:
        return int(_UNARY_OPS[type(node.op)](_eval_constant_expression(node.operand)))

    if isinstance(node, ast.BinOp) and type(node.op) in _BINARY_OPS:
        left = _eval_constant_expression(node.left)
        right = _eval_constant_expression(node.right)
        return int(_BINARY_OPS[type(node.op)](left, right))

    if isinstance(node, ast.BoolOp) and isinstance(node.op, (ast.And, ast.Or)):
        values = [_eval_constant_expression(value) != 0 for value in node.values]
        return int(all(values) if isinstance(node.op, ast.And) else any(values))

    if isinstance(node, ast.Compare):
        left = _eval_constant_expression(node.left)
        for operation, comparator in zip(node.ops, node.comparators):
            right = _eval_constant_expression(comparator)
            function = _COMPARE_OPS.get(type(operation))
            if function is None or not function(left, right):
                return 0
            left = right
        return 1

    raise ValueError("unsupported preprocessor expression")
