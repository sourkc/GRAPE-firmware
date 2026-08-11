from __future__ import annotations

from dataclasses import dataclass

from .shader_types import ShaderType


@dataclass(frozen=True)
class IRUniform:
    index: int
    name: str
    type: ShaderType


@dataclass(frozen=True)
class IRValue:
    id: int
    type: ShaderType


@dataclass(frozen=True)
class IRInstruction:
    result: IRValue | None


@dataclass(frozen=True)
class IRConstant(IRInstruction):
    text: str


@dataclass(frozen=True)
class IRLoadBuiltin(IRInstruction):
    name: str


@dataclass(frozen=True)
class IRLoadUniform(IRInstruction):
    uniform_index: int


@dataclass(frozen=True)
class IRUnary(IRInstruction):
    operator: str
    operand: IRValue


@dataclass(frozen=True)
class IRBinary(IRInstruction):
    operator: str
    left: IRValue
    right: IRValue


@dataclass(frozen=True)
class IRReturn(IRInstruction):
    value: IRValue


@dataclass(frozen=True)
class IRFunction:
    name: str
    return_type: ShaderType
    instructions: list[IRInstruction]


@dataclass(frozen=True)
class IRModule:
    uniforms: list[IRUniform]
    main: IRFunction
