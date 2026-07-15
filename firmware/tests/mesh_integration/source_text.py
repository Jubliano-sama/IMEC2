"""Read one C translation unit together with its local implementation fragments."""

from __future__ import annotations

import re
from pathlib import Path


_FRAGMENT_INCLUDE = re.compile(
    r'^\s*#\s*include\s+"(?P<name>[A-Za-z0-9_]+\.inc)"\s*$',
    re.MULTILINE,
)


def read_composed_source(path: Path) -> str:
    """Expand repository-local ``.inc`` files used by a C owner source."""

    resolved = path.resolve()
    return _read_composed_source(resolved, set())


def _read_composed_source(path: Path, active: set[Path]) -> str:
    if path in active:
        raise ValueError(f"recursive source fragment include: {path}")

    active.add(path)
    source = path.read_text(encoding="utf-8")

    def replace(match: re.Match[str]) -> str:
        fragment = (path.parent / match.group("name")).resolve()
        if fragment.parent != path.parent:
            raise ValueError(f"source fragment escaped owner directory: {fragment}")
        return _read_composed_source(fragment, active)

    try:
        return _FRAGMENT_INCLUDE.sub(replace, source)
    finally:
        active.remove(path)
