from __future__ import annotations

from dataclasses import dataclass

from .shader_types import ShaderType


@dataclass(frozen=True)
class IRUniform:
    index: int
    name: str
    type: ShaderType


@dataclass(frozen=True)
class IRParameter:
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
class IRIntConstant(IRInstruction):
    value: int


@dataclass(frozen=True)
class IRBoolConstant(IRInstruction):
    value: bool


@dataclass(frozen=True)
class IRLoadBuiltin(IRInstruction):
    name: str


@dataclass(frozen=True)
class IRLoadUniform(IRInstruction):
    uniform_index: int


@dataclass(frozen=True)
class IRLoadVariable(IRInstruction):
    kind: str
    variable_index: int


@dataclass(frozen=True)
class IRStoreVariable(IRInstruction):
    kind: str
    variable_index: int
    value: IRValue


@dataclass(frozen=True)
class IRStoreSwizzle(IRInstruction):
    kind: str
    variable_index: int
    components: tuple[int, ...]
    value: IRValue


@dataclass(frozen=True)
class IRDeclareLocal(IRInstruction):
    local_index: int
    name: str
    type: ShaderType
    initializer: IRValue | None
    is_const: bool


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
class IRCast(IRInstruction):
    operand: IRValue


@dataclass(frozen=True)
class IRConstruct(IRInstruction):
    arguments: tuple[IRValue, ...]


@dataclass(frozen=True)
class IRSwizzle(IRInstruction):
    base: IRValue
    components: tuple[int, ...]


@dataclass(frozen=True)
class IRCall(IRInstruction):
    function_id: int
    arguments: tuple[IRValue, ...]


@dataclass(frozen=True)
class IRBlock:
    instructions: list[IRInstruction]
    result: IRValue | None = None


@dataclass(frozen=True)
class IRLogical(IRInstruction):
    operator: str
    left: IRValue
    right: IRBlock


@dataclass(frozen=True)
class IRConditional(IRInstruction):
    condition: IRValue
    when_true: IRBlock
    when_false: IRBlock


@dataclass(frozen=True)
class IRIf(IRInstruction):
    condition: IRValue
    then_block: IRBlock
    else_block: IRBlock | None


@dataclass(frozen=True)
class IRFor(IRInstruction):
    initializer: IRBlock
    condition: IRBlock
    increment: IRBlock
    body: IRBlock
    max_iterations: int


@dataclass(frozen=True)
class IRBreak(IRInstruction):
    pass


@dataclass(frozen=True)
class IRReturn(IRInstruction):
    value: IRValue


@dataclass(frozen=True)
class IRFunction:
    id: int
    name: str
    return_type: ShaderType
    parameters: list[IRParameter]
    body: IRBlock


@dataclass(frozen=True)
class IRModule:
    uniforms: list[IRUniform]
    functions: list[IRFunction]
    main_function_id: int
