from __future__ import annotations

import hashlib
import json
from pathlib import Path


_BUILD_DOMAIN = b"GRAPE_SHADER_BUILD_V1"
_COMPILER_DOMAIN = b"GRAPE_SHADER_COMPILER_V1"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def compiler_fingerprint(shaderc_dir: Path) -> str:
    digest = hashlib.sha256()
    _feed(digest, _COMPILER_DOMAIN)

    files = sorted(path for path in shaderc_dir.rglob("*.py") if path.is_file())
    for path in files:
        relative = path.relative_to(shaderc_dir).as_posix().encode("utf-8")
        _feed(digest, relative)
        _feed(digest, path.read_bytes())

    return digest.hexdigest()


def build_fingerprint(
    source_path: str,
    source_bytes: bytes,
    compiler_hash: str,
    language_version: str,
    options: dict[str, object],
) -> str:
    digest = hashlib.sha256()
    for field in (
        _BUILD_DOMAIN,
        language_version.encode("utf-8"),
        source_path.encode("utf-8"),
        source_bytes,
        compiler_hash.encode("ascii"),
        json.dumps(options, sort_keys=True, separators=(",", ":")).encode("utf-8"),
    ):
        _feed(digest, field)
    return digest.hexdigest()


def _feed(digest, data: bytes) -> None:
    digest.update(len(data).to_bytes(8, "big"))
    digest.update(data)
