from __future__ import annotations

import argparse
from dataclasses import fields, is_dataclass
from enum import Enum
from pathlib import Path
import sys

from .compiler import compile_project, compile_shader
from .diagnostics import ShaderCompilerError
from .ir import (
    IRBinary,
    IRBoolConstant,
    IRBreak,
    IRBuiltinCall,
    IRCall,
    IRCast,
    IRConditional,
    IRConstant,
    IRConstruct,
    IRDeclareLocal,
    IRFor,
    IRIf,
    IRIntConstant,
    IRLoadBuiltin,
    IRLoadUniform,
    IRLoadVariable,
    IRLogical,
    IRReturn,
    IRStoreVariable,
    IRStoreSwizzle,
    IRSwizzle,
    IRUnary,
)


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
    for function_index, function in enumerate(module.functions):
        params = ", ".join(f"{parameter.name}: {parameter.type}" for parameter in function.parameters)
        main_tag = " [main]" if function.id == module.main_function_id else ""
        lines.append(f"function #{function.id} {function.name}({params}) -> {function.return_type}{main_tag}")
        lines.extend(_dump_ir_block(function.body, 1))
        if function_index + 1 != len(module.functions):
            lines.append("")
    return "\n".join(lines)


def _dump_ir_block(block, indent: int) -> list[str]:
    lines = []
    prefix = "    " * indent
    for instruction in block.instructions:
        if isinstance(instruction, IRConstant):
            lines.append(f"{prefix}{_value(instruction.result)} = constant {instruction.text}")
        elif isinstance(instruction, IRIntConstant):
            lines.append(f"{prefix}{_value(instruction.result)} = constant {instruction.value}")
        elif isinstance(instruction, IRBoolConstant):
            lines.append(f"{prefix}{_value(instruction.result)} = constant {str(instruction.value).lower()}")
        elif isinstance(instruction, IRLoadBuiltin):
            lines.append(f"{prefix}{_value(instruction.result)} = builtin {instruction.name}")
        elif isinstance(instruction, IRLoadUniform):
            lines.append(f"{prefix}{_value(instruction.result)} = uniform {instruction.uniform_index}")
        elif isinstance(instruction, IRLoadVariable):
            lines.append(
                f"{prefix}{_value(instruction.result)} = {instruction.kind} {instruction.variable_index}"
            )
        elif isinstance(instruction, IRStoreVariable):
            lines.append(
                f"{prefix}store {instruction.kind} {instruction.variable_index}, %{instruction.value.id}"
            )
        elif isinstance(instruction, IRStoreSwizzle):
            components = ",".join(str(component) for component in instruction.components)
            lines.append(
                f"{prefix}store_swizzle {instruction.kind} {instruction.variable_index} "
                f"[{components}], %{instruction.value.id}"
            )
        elif isinstance(instruction, IRDeclareLocal):
            init = f" = %{instruction.initializer.id}" if instruction.initializer is not None else ""
            const = "const " if instruction.is_const else ""
            lines.append(
                f"{prefix}local {instruction.local_index}: {const}{instruction.name} : {instruction.type}{init}"
            )
        elif isinstance(instruction, IRUnary):
            lines.append(f"{prefix}{_value(instruction.result)} = {instruction.operator} %{instruction.operand.id}")
        elif isinstance(instruction, IRBinary):
            lines.append(
                f"{prefix}{_value(instruction.result)} = {instruction.operator} %{instruction.left.id}, %{instruction.right.id}"
            )
        elif isinstance(instruction, IRCast):
            lines.append(f"{prefix}{_value(instruction.result)} = cast %{instruction.operand.id}")
        elif isinstance(instruction, IRConstruct):
            operands = ", ".join(f"%{argument.id}" for argument in instruction.arguments)
            lines.append(f"{prefix}{_value(instruction.result)} = construct {operands}")
        elif isinstance(instruction, IRSwizzle):
            components = ",".join(str(component) for component in instruction.components)
            lines.append(f"{prefix}{_value(instruction.result)} = swizzle %{instruction.base.id} [{components}]")
        elif isinstance(instruction, IRCall):
            operands = ", ".join(f"%{argument.id}" for argument in instruction.arguments)
            lines.append(f"{prefix}{_value(instruction.result)} = call #{instruction.function_id}({operands})")
        elif isinstance(instruction, IRBuiltinCall):
            operands = ", ".join(f"%{argument.id}" for argument in instruction.arguments)
            lines.append(f"{prefix}{_value(instruction.result)} = builtin_call {instruction.name}({operands})")
        elif isinstance(instruction, IRLogical):
            lines.append(f"{prefix}{_value(instruction.result)} = logical {instruction.operator} %{instruction.left.id}")
            lines.extend(_dump_ir_block(instruction.right, indent + 1))
        elif isinstance(instruction, IRConditional):
            lines.append(f"{prefix}{_value(instruction.result)} = conditional %{instruction.condition.id}")
            lines.append(f"{prefix}    true:")
            lines.extend(_dump_ir_block(instruction.when_true, indent + 2))
            lines.append(f"{prefix}    false:")
            lines.extend(_dump_ir_block(instruction.when_false, indent + 2))
        elif isinstance(instruction, IRIf):
            lines.append(f"{prefix}if %{instruction.condition.id}")
            lines.extend(_dump_ir_block(instruction.then_block, indent + 1))
            if instruction.else_block is not None:
                lines.append(f"{prefix}else")
                lines.extend(_dump_ir_block(instruction.else_block, indent + 1))
        elif isinstance(instruction, IRFor):
            lines.append(f"{prefix}for [max {instruction.max_iterations}]")
            lines.append(f"{prefix}    init:")
            lines.extend(_dump_ir_block(instruction.initializer, indent + 2))
            lines.append(f"{prefix}    condition:")
            lines.extend(_dump_ir_block(instruction.condition, indent + 2))
            lines.append(f"{prefix}    body:")
            lines.extend(_dump_ir_block(instruction.body, indent + 2))
            lines.append(f"{prefix}    increment:")
            lines.extend(_dump_ir_block(instruction.increment, indent + 2))
        elif isinstance(instruction, IRBreak):
            lines.append(f"{prefix}break")
        elif isinstance(instruction, IRReturn):
            lines.append(f"{prefix}return %{instruction.value.id}")
    if block.result is not None:
        lines.append(f"{prefix}=> %{block.result.id}")
    return lines


def _value(value) -> str:
    return f"%{value.id} : {value.type}"


if __name__ == "__main__":
    raise SystemExit(main())
