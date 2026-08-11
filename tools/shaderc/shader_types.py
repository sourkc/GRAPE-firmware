from __future__ import annotations

from enum import Enum


class ShaderType(Enum):
    BOOL = "bool"
    INT = "int"
    FLOAT = "float"
    VEC2 = "vec2"
    VEC3 = "vec3"
    VEC4 = "vec4"

    @property
    def component_count(self) -> int:
        return {
            ShaderType.BOOL: 1,
            ShaderType.INT: 1,
            ShaderType.FLOAT: 1,
            ShaderType.VEC2: 2,
            ShaderType.VEC3: 3,
            ShaderType.VEC4: 4,
        }[self]

    @property
    def is_vector(self) -> bool:
        return self in (ShaderType.VEC2, ShaderType.VEC3, ShaderType.VEC4)

    @property
    def is_scalar(self) -> bool:
        return not self.is_vector

    @property
    def is_numeric_scalar(self) -> bool:
        return self in (ShaderType.INT, ShaderType.FLOAT)

    @staticmethod
    def vector(component_count: int) -> ShaderType:
        try:
            return {
                2: ShaderType.VEC2,
                3: ShaderType.VEC3,
                4: ShaderType.VEC4,
            }[component_count]
        except KeyError as error:
            raise ValueError(f"unsupported vector width {component_count}") from error

    def __str__(self) -> str:
        return self.value
