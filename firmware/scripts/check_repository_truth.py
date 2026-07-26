#!/usr/bin/env python3
"""Verify that repository entry points name the same authoritative documents."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = Path("Documentation/CURRENT.json")
VERSION_HEADER = re.compile(r"^Version:\s*([0-9]+(?:\.[0-9]+)*)\s*$", re.MULTILINE)


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ValueError(f"{path} is missing") from None
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path} is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def _relative_path(value: object, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty repository-relative path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"{field} must stay inside the repository: {value!r}")
    return path


def _version_tuple(value: str) -> tuple[int, ...]:
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", value):
        raise ValueError(f"invalid numeric version {value!r}")
    return tuple(int(part) for part in value.split("."))


def _version_from_name(path: Path, prefix: str) -> str | None:
    name = path.name
    if not name.startswith(prefix) or not name.endswith(".md"):
        return None
    value = name[len(prefix):-3]
    try:
        _version_tuple(value)
    except ValueError:
        return None
    return value


def _manifest_parts(
    repo_root: Path,
) -> tuple[
    dict[str, Path],
    dict[str, dict[str, str]],
    dict[Path, list[str]],
    dict[str, list[str]],
    dict[str, dict[str, object]],
]:
    manifest = _load_json(repo_root / MANIFEST_PATH)
    if manifest.get("schema") != 1:
        raise ValueError(f"{MANIFEST_PATH} has unsupported schema {manifest.get('schema')!r}")

    raw_documents = manifest.get("documents")
    raw_series = manifest.get("versioned_series")
    raw_surfaces = manifest.get("reference_surfaces")
    raw_cross_references = manifest.get("required_cross_references", {})
    raw_retired = manifest.get("retired_series", {})
    if not isinstance(raw_documents, dict):
        raise ValueError("documents must be an object")
    if not isinstance(raw_series, dict):
        raise ValueError("versioned_series must be an object")
    if not isinstance(raw_surfaces, dict):
        raise ValueError("reference_surfaces must be an object")
    if not isinstance(raw_cross_references, dict):
        raise ValueError("required_cross_references must be an object")
    if not isinstance(raw_retired, dict):
        raise ValueError("retired_series must be an object")

    documents = {
        str(key): _relative_path(value, f"documents.{key}")
        for key, value in raw_documents.items()
    }
    series: dict[str, dict[str, str]] = {}
    for name, raw in raw_series.items():
        if not isinstance(raw, dict):
            raise ValueError(f"versioned_series.{name} must be an object")
        prefix = raw.get("prefix")
        document = raw.get("document")
        if not isinstance(prefix, str) or not prefix:
            raise ValueError(f"versioned_series.{name}.prefix must be non-empty")
        if not isinstance(document, str) or document not in documents:
            raise ValueError(
                f"versioned_series.{name}.document must name a manifest document"
            )
        series[str(name)] = {"prefix": prefix, "document": document}

    surfaces: dict[Path, list[str]] = {}
    for raw_path, raw_keys in raw_surfaces.items():
        path = _relative_path(raw_path, f"reference_surfaces.{raw_path}")
        if not isinstance(raw_keys, list) or not raw_keys:
            raise ValueError(f"reference_surfaces.{raw_path} must be a non-empty list")
        keys: list[str] = []
        for key in raw_keys:
            if not isinstance(key, str) or key not in documents:
                raise ValueError(
                    f"reference_surfaces.{raw_path} names unknown document {key!r}"
                )
            keys.append(key)
        surfaces[path] = keys

    cross_references: dict[str, list[str]] = {}
    for source, raw_targets in raw_cross_references.items():
        if source not in documents:
            raise ValueError(f"required_cross_references names unknown source {source!r}")
        if not isinstance(raw_targets, list) or not raw_targets:
            raise ValueError(
                f"required_cross_references.{source} must be a non-empty list"
            )
        targets: list[str] = []
        for target in raw_targets:
            if not isinstance(target, str) or target not in documents:
                raise ValueError(
                    f"required_cross_references.{source} names unknown target {target!r}"
                )
            targets.append(target)
        cross_references[source] = targets

    retired: dict[str, dict[str, object]] = {}
    for name, raw in raw_retired.items():
        if not isinstance(raw, dict):
            raise ValueError(f"retired_series.{name} must be an object")
        replacement = raw.get("replacement")
        forbidden = raw.get("forbidden_current_references")
        last_version = raw.get("last_version")
        deleted_in = raw.get("deleted_in")
        if not isinstance(replacement, str) or replacement not in documents:
            raise ValueError(
                f"retired_series.{name}.replacement must name a manifest document"
            )
        if (
            not isinstance(forbidden, list)
            or not forbidden
            or any(not isinstance(value, str) or not value for value in forbidden)
        ):
            raise ValueError(
                f"retired_series.{name}.forbidden_current_references "
                "must be a non-empty string list"
            )
        if not isinstance(last_version, str) or not last_version:
            raise ValueError(f"retired_series.{name}.last_version must be non-empty")
        if not isinstance(deleted_in, str) or not deleted_in:
            raise ValueError(f"retired_series.{name}.deleted_in must be non-empty")
        retired[str(name)] = raw

    return documents, series, surfaces, cross_references, retired


def check_repository(repo_root: Path = REPO_ROOT) -> list[str]:
    issues: list[str] = []
    try:
        documents, series, surfaces, cross_references, retired = _manifest_parts(
            repo_root
        )
    except ValueError as exc:
        return [str(exc)]

    for key, relative in documents.items():
        if not (repo_root / relative).is_file():
            issues.append(f"documents.{key} points to missing file {relative}")

    series_files: dict[str, list[tuple[tuple[int, ...], str, Path]]] = {}
    documentation_dir = repo_root / "Documentation"
    for name, config in series.items():
        prefix = config["prefix"]
        candidates: list[tuple[tuple[int, ...], str, Path]] = []
        for path in documentation_dir.glob(f"{prefix}*.md"):
            version = _version_from_name(path, prefix)
            if version is not None:
                candidates.append((_version_tuple(version), version, path))
        candidates.sort()
        series_files[name] = candidates
        if not candidates:
            issues.append(f"versioned_series.{name} has no matching documents")
            continue

        current = documents[config["document"]]
        latest = candidates[-1][2].relative_to(repo_root)
        if current != latest:
            issues.append(
                f"documents.{config['document']} is {current}, but latest {name} is {latest}"
            )

        current_path = repo_root / current
        if current_path.is_file():
            match = VERSION_HEADER.search(current_path.read_text(encoding="utf-8"))
            expected = _version_from_name(current_path, prefix)
            if match is None:
                issues.append(f"{current} has no Version header")
            elif expected is None or match.group(1) != expected:
                issues.append(
                    f"{current} declares Version {match.group(1)}, expected {expected}"
                )

    for surface, keys in surfaces.items():
        surface_path = repo_root / surface
        if not surface_path.is_file():
            issues.append(f"reference surface is missing: {surface}")
            continue
        text = surface_path.read_text(encoding="utf-8")
        for key in keys:
            current = documents[key]
            if current.name not in text:
                issues.append(f"{surface} does not reference current {key}: {current}")

        relevant_series = [
            (name, config)
            for name, config in series.items()
            if config["document"] in keys
        ]
        for name, config in relevant_series:
            current = documents[config["document"]]
            for _, _, candidate in series_files.get(name, []):
                relative = candidate.relative_to(repo_root)
                if relative != current and candidate.name in text:
                    issues.append(
                        f"{surface} still references superseded {name}: {relative}"
                    )

    for source, targets in cross_references.items():
        source_relative = documents[source]
        source_path = repo_root / source_relative
        if not source_path.is_file():
            continue
        text = source_path.read_text(encoding="utf-8")
        for target in targets:
            target_relative = documents[target]
            if target_relative.stem not in text:
                issues.append(
                    f"{source_relative} does not reference canonical {target}: "
                    f"{target_relative}"
                )

    current_texts: dict[Path, str] = {}
    for relative in documents.values():
        path = repo_root / relative
        if path.is_file():
            current_texts[relative] = path.read_text(encoding="utf-8")
    for surface in surfaces:
        path = repo_root / surface
        if path.is_file():
            current_texts[surface] = path.read_text(encoding="utf-8")
    for name, config in retired.items():
        replacement = documents[str(config["replacement"])]
        for forbidden in config["forbidden_current_references"]:
            for relative, text in current_texts.items():
                if forbidden in text:
                    issues.append(
                        f"{relative} references retired {name} target {forbidden!r}; "
                        f"use {replacement}"
                    )

    return issues


def main() -> int:
    issues = check_repository()
    if issues:
        print("repository source-of-truth check failed:", file=sys.stderr)
        for issue in issues:
            print(f"  {issue}", file=sys.stderr)
        return 1
    print("repository source-of-truth check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
