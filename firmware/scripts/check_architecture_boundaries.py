#!/usr/bin/env python3
"""Prevent known oversized translation units from accumulating more behavior."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path("firmware/architecture_boundaries.json")
INC_DIRECTIVE = re.compile(r'^#include\s+"([^"]+\.inc)"\s*$', re.MULTILINE)


def _line_count(path: Path) -> int:
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return sum(1 for _ in stream)


def _relative_path(value: object, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"{field} must stay inside the repository: {value!r}")
    return path


def _positive_int(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def _load_manifest(repo_root: Path, manifest_path: Path) -> dict[str, Any]:
    path = repo_root / manifest_path
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ValueError(f"{manifest_path} is missing") from None
    except json.JSONDecodeError as exc:
        raise ValueError(f"{manifest_path} is not valid JSON: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema") != 1:
        raise ValueError(f"{manifest_path} must contain a schema-1 object")
    return value


def check_repository(
    repo_root: Path = REPO_ROOT,
    manifest_path: Path = DEFAULT_MANIFEST,
) -> list[str]:
    try:
        manifest = _load_manifest(repo_root, manifest_path)
        max_c_lines = _positive_int(
            manifest.get("default_c_max_lines"),
            "default_c_max_lines",
        )
        raw_roots = manifest.get("source_roots")
        raw_frozen = manifest.get("frozen_oversize_sources")
        raw_fragments = manifest.get("approved_include_fragments")
        raw_compositions = manifest.get("composed_translation_units")
        if not isinstance(raw_roots, list) or not raw_roots:
            raise ValueError("source_roots must be a non-empty list")
        if not isinstance(raw_frozen, dict):
            raise ValueError("frozen_oversize_sources must be an object")
        if not isinstance(raw_fragments, dict):
            raise ValueError("approved_include_fragments must be an object")
        if not isinstance(raw_compositions, dict):
            raise ValueError("composed_translation_units must be an object")
        source_roots = [
            _relative_path(value, f"source_roots[{index}]")
            for index, value in enumerate(raw_roots)
        ]
        frozen = {
            _relative_path(path, f"frozen_oversize_sources.{path}"):
            _positive_int(limit, f"frozen_oversize_sources.{path}")
            for path, limit in raw_frozen.items()
        }
        fragments = {
            _relative_path(path, f"approved_include_fragments.{path}"):
            _positive_int(limit, f"approved_include_fragments.{path}")
            for path, limit in raw_fragments.items()
        }
    except ValueError as exc:
        return [str(exc)]

    issues: list[str] = []
    seen_c_files: set[Path] = set()
    seen_fragments: set[Path] = set()
    for source_root in source_roots:
        root = repo_root / source_root
        if not root.is_dir():
            issues.append(f"source root is missing: {source_root}")
            continue
        for path in root.rglob("*.c"):
            relative = path.relative_to(repo_root)
            seen_c_files.add(relative)
            count = _line_count(path)
            limit = frozen.get(relative, max_c_lines)
            if count > limit:
                if relative in frozen:
                    issues.append(
                        f"{relative} grew from its frozen {limit}-line ceiling to {count}"
                    )
                else:
                    issues.append(
                        f"{relative} has {count} lines; new source ceiling is {max_c_lines}"
                    )
        for path in root.rglob("*.inc"):
            relative = path.relative_to(repo_root)
            seen_fragments.add(relative)
            if relative not in fragments:
                issues.append(
                    f"{relative} is a new include fragment; add a focused .c/.h module instead"
                )
                continue
            count = _line_count(path)
            if count > fragments[relative]:
                issues.append(
                    f"{relative} grew from its frozen {fragments[relative]}-line ceiling to {count}"
                )

    for relative in frozen:
        if relative not in seen_c_files:
            issues.append(f"frozen source is missing or outside source_roots: {relative}")
    for relative in fragments:
        if relative not in seen_fragments:
            issues.append(f"approved include fragment is missing: {relative}")

    for raw_source, raw_config in raw_compositions.items():
        try:
            source = _relative_path(
                raw_source,
                f"composed_translation_units.{raw_source}",
            )
            if not isinstance(raw_config, dict):
                raise ValueError(
                    f"composed_translation_units.{raw_source} must be an object"
                )
            limit = _positive_int(
                raw_config.get("max_composed_lines"),
                f"composed_translation_units.{raw_source}.max_composed_lines",
            )
            expected_includes = raw_config.get("includes")
            if not isinstance(expected_includes, list):
                raise ValueError(
                    f"composed_translation_units.{raw_source}.includes must be a list"
                )
            expected = [
                _relative_path(
                    value,
                    f"composed_translation_units.{raw_source}.includes",
                ).as_posix()
                for value in expected_includes
            ]
        except ValueError as exc:
            issues.append(str(exc))
            continue

        source_path = repo_root / source
        if not source_path.is_file():
            issues.append(f"composed translation unit is missing: {source}")
            continue
        actual = INC_DIRECTIVE.findall(
            source_path.read_text(encoding="utf-8", errors="replace")
        )
        if actual != expected:
            issues.append(
                f"{source} include-fragment composition changed: "
                f"expected {expected}, found {actual}"
            )
            continue
        composed_lines = _line_count(source_path)
        missing = False
        for include in actual:
            include_path = source_path.parent / include
            if not include_path.is_file():
                issues.append(f"{source} includes missing fragment {include}")
                missing = True
                continue
            composed_lines += _line_count(include_path)
        if not missing and composed_lines > limit:
            issues.append(
                f"{source} composed translation unit grew from its "
                f"{limit}-line ceiling to {composed_lines}"
            )

    return issues


def main() -> int:
    issues = check_repository()
    if issues:
        print("architecture boundary check failed:", file=sys.stderr)
        for issue in issues:
            print(f"  {issue}", file=sys.stderr)
        return 1
    print("architecture boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
