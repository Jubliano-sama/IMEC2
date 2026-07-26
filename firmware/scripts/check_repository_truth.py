#!/usr/bin/env python3
"""Verify that repository entry points name the same authoritative documents."""

from __future__ import annotations

import fnmatch
import json
import posixpath
import re
import subprocess
import sys
from datetime import date
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit


REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = Path("Documentation/CURRENT.json")
VERSION_HEADER = re.compile(r"^Version:\s*([0-9]+(?:\.[0-9]+)*)\s*$", re.MULTILINE)
MARKDOWN_LINK = re.compile(
    r"(?<!!)\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^)\n]+?)\s*\)"
)
WIKI_LINK = re.compile(r"\[\[([^\]\n]+)\]\]")
YAML_SOURCE_FILES = re.compile(r"^(\s*)source_files:\s*(?:#.*)?$")
YAML_LIST_ITEM = re.compile(r"^\s*-\s*(.*?)\s*(?:#.*)?$")
YAML_REF_COMMIT = re.compile(r"^\s*ref_commit_hash:\s*(.*?)\s*(?:#.*)?$")
YAML_UPDATED_AT = re.compile(r"^\s*updated_at:\s*(.*?)\s*(?:#.*)?$")

REQUIRED_DOCUMENT_KEYS = frozenset(
    {
        "contract",
        "architecture",
        "protocol",
        "runtime_flow",
        "development",
        "architecture_reset",
    }
)
REQUIRED_VERSIONED_SERIES = {
    "architecture": {
        "prefix": "UWB+BLE Architecture ",
        "document": "architecture",
    },
    "protocol": {
        "prefix": "UWB+BLE Protocols and Strategies ",
        "document": "protocol",
    },
}
REQUIRED_CROSS_REFERENCES = {
    "architecture": frozenset({"protocol", "contract", "runtime_flow"}),
    "protocol": frozenset({"architecture", "contract", "runtime_flow"}),
}
REQUIRED_REFERENCE_SURFACES = {
    Path("README.md"): frozenset({"architecture", "protocol"}),
    Path("CODEMAP.md"): frozenset({"architecture", "protocol"}),
    Path("Documentation/INDEX.md"): frozenset({"architecture", "protocol"}),
    Path("firmware/README.md"): frozenset({"architecture", "protocol"}),
    Path("docs/wiki/toc.yaml"): frozenset({"architecture", "protocol"}),
}
REQUIRED_RETIRED_SERIES = {
    "state_machines": {
        "replacement": "runtime_flow",
    }
}


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


def _required_object(
    manifest: dict[str, Any],
    field: str,
    *,
    nonempty: bool = True,
) -> dict[str, Any]:
    if field not in manifest:
        raise ValueError(f"{field} section is required")
    value = manifest[field]
    if not isinstance(value, dict):
        raise ValueError(f"{field} must be an object")
    if nonempty and not value:
        raise ValueError(f"{field} must be a non-empty object")
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


def _key_set_error(field: str, actual: set[str], required: set[str]) -> ValueError:
    details: list[str] = []
    missing = sorted(required - actual)
    unexpected = sorted(actual - required)
    if missing:
        details.append(f"missing {missing}")
    if unexpected:
        details.append(f"unexpected {unexpected}")
    return ValueError(f"{field} must use the fixed schema keys: {', '.join(details)}")


def _strip_markdown_title(target: str) -> str:
    if target.startswith("<") and target.endswith(">"):
        return target[1:-1].strip()
    return re.sub(
        r"""\s+(?:"[^"]*"|'[^']*'|\([^)]*\))\s*$""",
        "",
        target,
    ).strip()


def _resolve_link(
    source: Path,
    raw_target: str,
    *,
    wiki: bool = False,
    repository_relative: bool = False,
) -> Path | None:
    target = raw_target.strip()
    if wiki:
        target = target.split("|", 1)[0].strip()
    else:
        target = _strip_markdown_title(target)
    target = unquote(target)
    target = target.split("#", 1)[0].split("?", 1)[0].strip()
    if not target:
        return None

    parsed = urlsplit(target)
    if parsed.scheme or parsed.netloc:
        return None

    if target.startswith("/"):
        combined = target.lstrip("/")
    elif repository_relative:
        combined = target
    else:
        combined = str(source.parent / target)
    normalized = posixpath.normpath(combined)
    if normalized == ".." or normalized.startswith("../"):
        return None
    if wiki and not normalized.lower().endswith(".md"):
        normalized += ".md"
    return Path(normalized)


def _markdown_links(source: Path, text: str) -> set[Path]:
    links: set[Path] = set()
    for match in MARKDOWN_LINK.finditer(text):
        target = _resolve_link(source, match.group(1))
        if target is not None:
            links.add(target)
    for match in WIKI_LINK.finditer(text):
        target = _resolve_link(source, match.group(1), wiki=True)
        if target is not None:
            links.add(target)
    return links


def _yaml_scalar(value: str) -> str | None:
    value = value.strip()
    if not value:
        return None
    if value.startswith('"') and value.endswith('"'):
        try:
            decoded = json.loads(value)
        except json.JSONDecodeError:
            return None
        return decoded if isinstance(decoded, str) else None
    if value.startswith("'") and value.endswith("'"):
        return value[1:-1].replace("''", "'")
    return value


def _wiki_source_links(source: Path, text: str) -> set[Path]:
    links: set[Path] = set()
    source_indent: int | None = None
    for line in text.splitlines():
        header = YAML_SOURCE_FILES.match(line)
        if header:
            source_indent = len(header.group(1))
            continue
        if source_indent is None:
            continue

        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= source_indent:
            source_indent = None
            continue

        item = YAML_LIST_ITEM.match(line)
        if item is None:
            continue
        scalar = _yaml_scalar(item.group(1))
        if scalar is None:
            continue
        target = _resolve_link(source, scalar, repository_relative=True)
        if target is not None:
            links.add(target)
    return links


def _wiki_project_scalar(text: str, pattern: re.Pattern[str]) -> str | None:
    for line in text.splitlines():
        match = pattern.match(line)
        if match is not None:
            return _yaml_scalar(match.group(1))
    return None


def _check_wiki_snapshot(repo_root: Path, toc_relative: Path) -> list[str]:
    issues: list[str] = []
    toc_path = repo_root / toc_relative
    if not toc_path.is_file():
        return [f"wiki TOC is missing: {toc_relative}"]
    text = toc_path.read_text(encoding="utf-8")
    ref = _wiki_project_scalar(text, YAML_REF_COMMIT)
    updated_at = _wiki_project_scalar(text, YAML_UPDATED_AT)
    if ref is None or re.fullmatch(r"[0-9a-fA-F]{40}", ref) is None:
        issues.append(f"{toc_relative} must pin a full 40-character commit hash")
        return issues
    if updated_at is None:
        issues.append(f"{toc_relative} must declare updated_at")
    else:
        try:
            date.fromisoformat(updated_at)
        except ValueError:
            issues.append(f"{toc_relative} updated_at must be an ISO date")

    verify = subprocess.run(
        ["git", "cat-file", "-e", f"{ref}^{{commit}}"],
        cwd=repo_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if verify.returncode != 0:
        issues.append(f"{toc_relative} pins missing commit {ref}")
        return issues
    tree_result = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", ref],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if tree_result.returncode != 0:
        issues.append(
            f"cannot inspect wiki snapshot {ref}: {tree_result.stderr.strip()}"
        )
        return issues
    tree_paths = set(tree_result.stdout.splitlines())
    for source in sorted(_wiki_source_links(toc_relative, text), key=str):
        pattern = str(source)
        if any(character in pattern for character in "*?["):
            matched = any(
                fnmatch.fnmatchcase(candidate, pattern)
                for candidate in tree_paths
            )
        else:
            matched = pattern in tree_paths or any(
                candidate.startswith(f"{pattern.rstrip('/')}/")
                for candidate in tree_paths
            )
        if not matched:
            issues.append(
                f"{toc_relative} source {pattern!r} does not exist at pinned ref {ref}"
            )
    return issues


def _document_links(source: Path, text: str) -> set[Path]:
    if source.suffix.lower() in {".yaml", ".yml"}:
        return _wiki_source_links(source, text)
    return _markdown_links(source, text)


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

    raw_documents = _required_object(manifest, "documents")
    raw_series = _required_object(manifest, "versioned_series")
    raw_surfaces = _required_object(manifest, "reference_surfaces")
    raw_cross_references = _required_object(manifest, "required_cross_references")
    raw_retired = _required_object(manifest, "retired_series")

    document_keys = {str(key) for key in raw_documents}
    if document_keys != REQUIRED_DOCUMENT_KEYS:
        raise _key_set_error(
            "documents",
            document_keys,
            set(REQUIRED_DOCUMENT_KEYS),
        )

    documents = {
        str(key): _relative_path(value, f"documents.{key}")
        for key, value in raw_documents.items()
    }

    series_keys = {str(key) for key in raw_series}
    required_series_keys = set(REQUIRED_VERSIONED_SERIES)
    if series_keys != required_series_keys:
        raise _key_set_error(
            "versioned_series",
            series_keys,
            required_series_keys,
        )
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
        required = REQUIRED_VERSIONED_SERIES[str(name)]
        if prefix != required["prefix"] or document != required["document"]:
            raise ValueError(
                f"versioned_series.{name} must be "
                f"prefix={required['prefix']!r}, document={required['document']!r}"
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
        if len(keys) != len(set(keys)):
            raise ValueError(f"reference_surfaces.{raw_path} contains duplicate documents")
        surfaces[path] = keys
    if set(surfaces) != set(REQUIRED_REFERENCE_SURFACES):
        raise _key_set_error(
            "reference_surfaces",
            {str(path) for path in surfaces},
            {str(path) for path in REQUIRED_REFERENCE_SURFACES},
        )
    for path, expected_keys in REQUIRED_REFERENCE_SURFACES.items():
        actual_keys = set(surfaces[path])
        if actual_keys != expected_keys:
            raise ValueError(
                f"reference_surfaces.{path} must contain exactly "
                f"{sorted(expected_keys)}"
            )

    cross_reference_keys = {str(key) for key in raw_cross_references}
    required_cross_reference_keys = set(REQUIRED_CROSS_REFERENCES)
    if cross_reference_keys != required_cross_reference_keys:
        raise _key_set_error(
            "required_cross_references",
            cross_reference_keys,
            required_cross_reference_keys,
        )
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
        expected_targets = REQUIRED_CROSS_REFERENCES[str(source)]
        if len(targets) != len(set(targets)) or set(targets) != expected_targets:
            raise ValueError(
                f"required_cross_references.{source} must contain exactly "
                f"{sorted(expected_targets)}"
            )
        cross_references[source] = targets

    missing_retired = set(REQUIRED_RETIRED_SERIES) - {
        str(key) for key in raw_retired
    }
    if missing_retired:
        raise ValueError(
            "retired_series is missing required entries: "
            f"{sorted(missing_retired)}"
        )
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
        try:
            _version_tuple(last_version)
        except ValueError:
            raise ValueError(
                f"retired_series.{name}.last_version must be a numeric version"
            ) from None
        if not isinstance(deleted_in, str) or not deleted_in:
            raise ValueError(f"retired_series.{name}.deleted_in must be non-empty")
        if not re.fullmatch(r"[0-9a-fA-F]{7,40}", deleted_in):
            raise ValueError(
                f"retired_series.{name}.deleted_in must be a commit identifier"
            )
        if len(forbidden) != len(set(forbidden)):
            raise ValueError(
                f"retired_series.{name}.forbidden_current_references "
                "must not contain duplicates"
            )
        required_retired = REQUIRED_RETIRED_SERIES.get(str(name))
        if (
            required_retired is not None
            and replacement != required_retired["replacement"]
        ):
            raise ValueError(
                f"retired_series.{name}.replacement must be "
                f"{required_retired['replacement']!r}"
            )
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
        links = _document_links(surface, text)
        for key in keys:
            current = documents[key]
            if current not in links:
                issues.append(
                    f"{surface} does not link to canonical current {key}: {current}"
                )

        relevant_series = [
            (name, config)
            for name, config in series.items()
            if config["document"] in keys
        ]
        for name, config in relevant_series:
            current = documents[config["document"]]
            for _, _, candidate in series_files.get(name, []):
                relative = candidate.relative_to(repo_root)
                if relative != current and relative in links:
                    issues.append(
                        f"{surface} still references superseded {name}: {relative}"
                    )

    for source, targets in cross_references.items():
        source_relative = documents[source]
        source_path = repo_root / source_relative
        if not source_path.is_file():
            continue
        text = source_path.read_text(encoding="utf-8")
        links = _document_links(source_relative, text)
        for target in targets:
            target_relative = documents[target]
            if target_relative not in links:
                issues.append(
                    f"{source_relative} does not link to canonical {target}: "
                    f"{target_relative}"
                )

    current_links: dict[Path, set[Path]] = {}
    for relative in documents.values():
        path = repo_root / relative
        if path.is_file():
            current_links[relative] = _document_links(
                relative,
                path.read_text(encoding="utf-8"),
            )
    for surface in surfaces:
        path = repo_root / surface
        if path.is_file():
            current_links[surface] = _document_links(
                surface,
                path.read_text(encoding="utf-8"),
            )
    for name, config in retired.items():
        replacement = documents[str(config["replacement"])]
        for forbidden in config["forbidden_current_references"]:
            for relative, links in current_links.items():
                if any(forbidden in link.stem for link in links):
                    issues.append(
                        f"{relative} references retired {name} target {forbidden!r}; "
                        f"use {replacement}"
                    )

    wiki_toc = Path("docs/wiki/toc.yaml")
    if wiki_toc in surfaces:
        issues.extend(_check_wiki_snapshot(repo_root, wiki_toc))

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
