from __future__ import annotations

from enum import Enum


class ShaderType(Enum):
    FLOAT = "float"
    VEC4 = "vec4"

    def __str__(self) -> str:
        return self.value
