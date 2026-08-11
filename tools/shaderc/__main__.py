from __future__ import annotations

import argparse
from dataclasses import fields, is_dataclass
from enum import Enum
from pathlib import Path
import sys

from .compiler import compile_project, compile_shader
from .diagnostics import ShaderCompilerError
from .ir import IRBinary, IRConstant, IRLoadBuiltin, IRLoadUniform, IRReturn, IRUnary


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.shaderc")
    parser.add_argument("source", nargs="?", help="single .grsh source to compile")
    parser.add_argument("--project-root", default=".", help="GRAPE project root")
    parser.add_argument("--dump-tokens", action="store_true")
    parser.add_argument("--dump-ast", action="store_true")
    parser.add_argument("--dump-ir", action="store_true")
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()

    try:
        if args.source is None:
            if args.dump_tokens or args.dump_ast or args.dump_ir:
                parser.error("dump options require a source file")
            compile_project(project_root)
            return 0

        source_path = Path(args.source)
        if not source_path.is_absolute():
            source_path = (project_root / source_path).resolve()
        result = compile_shader(project_root, source_path)

        if args.dump_tokens:
            for token in result.tokens:
                start = token.span.start
                print(f"{start.line}:{start.column} {token.kind.name:<14} {token.text!r}")

        if args.dump_ast:
            print(_dump_dataclass(result.ast))

        if args.dump_ir:
            print(_dump_ir(result.ir))

        if not (args.dump_tokens or args.dump_ast or args.dump_ir):
            print(result.c_text)
        return 0
    except ShaderCompilerError as error:
        print(error.format(), file=sys.stderr)
        return 1
    except (OSError, RuntimeError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1


def _dump_dataclass(value, indent: int = 0) -> str:
    prefix = " " * indent
    if isinstance(value, Enum):
        return value.value
    if isinstance(value, list):
        if not value:
            return "[]"
        return "\n".join(_dump_dataclass(item, indent) for item in value)
    if not is_dataclass(value):
        return repr(value)

    lines = [f"{prefix}{type(value).__name__}"]
    for field in fields(value):
        if field.name == "span":
            continue
        item = getattr(value, field.name)
        if is_dataclass(item):
            lines.append(f"{prefix}  {field.name}:")
            lines.append(_dump_dataclass(item, indent + 4))
        elif isinstance(item, list):
            lines.append(f"{prefix}  {field.name}:")
            if item:
                for child in item:
                    lines.append(_dump_dataclass(child, indent + 4))
            else:
                lines.append(f"{prefix}    []")
        elif isinstance(item, Enum):
            lines.append(f"{prefix}  {field.name}: {item.value}")
        else:
            lines.append(f"{prefix}  {field.name}: {item!r}")
    return "\n".join(lines)


def _dump_ir(module) -> str:
    lines = []
    for uniform in module.uniforms:
        lines.append(f"uniform {uniform.index}: {uniform.name} : {uniform.type}")
    if lines:
        lines.append("")
    lines.append(f"function {module.main.name} -> {module.main.return_type}")

    for instruction in module.main.instructions:
        if isinstance(instruction, IRConstant):
            lines.append(f"    {_value(instruction.result)} = constant {instruction.text}")
        elif isinstance(instruction, IRLoadBuiltin):
            lines.append(f"    {_value(instruction.result)} = builtin {instruction.name}")
        elif isinstance(instruction, IRLoadUniform):
            lines.append(f"    {_value(instruction.result)} = uniform {instruction.uniform_index}")
        elif isinstance(instruction, IRUnary):
            lines.append(f"    {_value(instruction.result)} = {instruction.operator} %{instruction.operand.id}")
        elif isinstance(instruction, IRBinary):
            lines.append(
                f"    {_value(instruction.result)} = {instruction.operator} %{instruction.left.id}, %{instruction.right.id}"
            )
        elif isinstance(instruction, IRReturn):
            lines.append(f"    return %{instruction.value.id}")
    return "\n".join(lines)


def _value(value) -> str:
    return f"%{value.id} : {value.type}"


if __name__ == "__main__":
    raise SystemExit(main())
