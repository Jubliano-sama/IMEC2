#!/usr/bin/env python3
"""Verify that repository entry points name the same authoritative documents."""

from __future__ import annotations

import fnmatch
import hashlib
import json
import posixpath
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit


REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = Path("Documentation/CURRENT.json")
CANONICAL_WIKI_REPO_BASE_URL = "https://github.com/Jubliano-sama/IMEC2/blob"
VERSION_HEADER = re.compile(r"^Version:\s*([0-9]+(?:\.[0-9]+)*)\s*$", re.MULTILINE)
MARKDOWN_LINK = re.compile(
    r"(?<!!)\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^)\n]+?)\s*\)"
)
WIKI_LINK = re.compile(r"\[\[([^\]\n]+)\]\]")
YAML_SOURCE_FILES = re.compile(r"^(\s*)source_files:\s*(?:#.*)?$")
YAML_LIST_ITEM = re.compile(r"^\s*-\s*(.*?)\s*(?:#.*)?$")
YAML_REF_COMMIT = re.compile(r"^\s*ref_commit_hash:\s*(.*?)\s*(?:#.*)?$")
YAML_UPDATED_AT = re.compile(r"^\s*updated_at:\s*(.*?)\s*(?:#.*)?$")
YAML_REPO_BASE_URL = re.compile(r"^\s*repo_base_url:\s*(.*?)\s*(?:#.*)?$")
YAML_PAGE_ID = re.compile(r"^  - id:\s*(.*?)\s*(?:#.*)?$")
YAML_PAGE_FILENAME = re.compile(r"^    filename:\s*(.*?)\s*(?:#.*)?$")
YAML_SECTION_ID = re.compile(r"^      - id:\s*(.*?)\s*(?:#.*)?$")
YAML_SECTION_AUTOGEN = re.compile(
    r"^        autogen:\s*(true|false)\s*(?:#.*)?$",
    re.IGNORECASE,
)
PAGE_ID_MARKER = re.compile(r"<!--\s*PAGE_ID:\s*([A-Za-z0-9_.-]+)\s*-->")
AUTOGEN_BEGIN = re.compile(
    r"^\s*<!--\s*BEGIN:AUTOGEN\s+([A-Za-z0-9_.-]+)\s*-->\s*$"
)
AUTOGEN_END = re.compile(
    r"^\s*<!--\s*END:AUTOGEN\s+([A-Za-z0-9_.-]+)\s*-->\s*$"
)
MARKDOWN_LINK_DETAIL = re.compile(
    r"(?<!!)\[([^\]\n]*)\]\(\s*(<[^>\n]+>|[^)\n]+?)\s*\)"
)
CITATION_LABEL_RANGE = re.compile(r":([0-9]+)(?:-([0-9]+))?$")
CITATION_TARGET = re.compile(
    r"^([0-9a-fA-F]{40})/(.+)#L([0-9]+)(?:-L([0-9]+))?$"
)
SUMMARY_COMMIT = re.compile(
    r"^Commit:\s*`([0-9a-fA-F]{40})`\s*$",
    re.MULTILINE,
)
SUMMARY_WIKI_STATE = re.compile(
    r"^Wiki state SHA-256:\s*`([0-9a-fA-F]{64})`\s*$",
    re.MULTILINE,
)
MERMAID_FENCE = re.compile(r"^\s*```mermaid\s*$", re.MULTILINE)

WIKI_CONTEXT_ARTIFACTS = (
    Path("docs/wiki/_context/context_pack.json"),
    Path("docs/wiki/_context/sync_context.json"),
    Path("docs/wiki/_context/update_context.json"),
)
WIKI_REPORT_ARTIFACTS = (
    Path("docs/wiki/_reports/SUMMARY.md"),
    Path("docs/wiki/_reports/structure_validation.json"),
    Path("docs/wiki/_reports/mermaid_invalid.json"),
)

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


@dataclass(frozen=True)
class WikiPage:
    page_id: str
    filename: Path
    source_files: tuple[Path, ...]
    autogen_sections: tuple[str, ...]


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ValueError(f"{path} is missing") from None
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"{path} cannot be read as UTF-8 JSON: {exc}") from exc
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


def _parse_wiki_toc(
    toc_relative: Path,
    text: str,
) -> tuple[list[WikiPage], list[str]]:
    issues: list[str] = []
    lines = text.splitlines()
    page_starts = [
        index
        for index, line in enumerate(lines)
        if YAML_PAGE_ID.match(line) is not None
    ]
    if not page_starts:
        return [], [f"{toc_relative} must declare at least one wiki page"]

    pages: list[WikiPage] = []
    page_ids: set[str] = set()
    filenames: set[Path] = set()
    section_ids: set[str] = set()
    for page_number, start in enumerate(page_starts):
        end = (
            page_starts[page_number + 1]
            if page_number + 1 < len(page_starts)
            else len(lines)
        )
        block = lines[start:end]
        page_match = YAML_PAGE_ID.match(block[0])
        assert page_match is not None
        page_id = _yaml_scalar(page_match.group(1))
        if page_id is None or re.fullmatch(r"[A-Za-z0-9_.-]+", page_id) is None:
            issues.append(
                f"{toc_relative}:{start + 1} has an invalid wiki page id"
            )
            page_id = f"<invalid-page-{start + 1}>"
        elif page_id in page_ids:
            issues.append(f"{toc_relative} repeats wiki page id {page_id!r}")
        page_ids.add(page_id)

        filename_matches = [
            (offset, match)
            for offset, line in enumerate(block)
            if (match := YAML_PAGE_FILENAME.match(line)) is not None
        ]
        filename: Path | None = None
        if len(filename_matches) != 1:
            issues.append(
                f"{toc_relative} page {page_id!r} must declare exactly one filename"
            )
        else:
            raw_filename = _yaml_scalar(filename_matches[0][1].group(1))
            if raw_filename is not None:
                filename = _resolve_link(toc_relative, raw_filename)
            if (
                filename is None
                or filename.suffix.lower() != ".md"
                or filename.parent != toc_relative.parent
            ):
                issues.append(
                    f"{toc_relative} page {page_id!r} filename must be a Markdown "
                    f"file directly under {toc_relative.parent}"
                )
                filename = None
            elif filename in filenames:
                issues.append(
                    f"{toc_relative} repeats wiki filename {filename}"
                )
            else:
                filenames.add(filename)

        source_headers = [
            offset
            for offset, line in enumerate(block)
            if re.fullmatch(r"    source_files:\s*(?:#.*)?", line) is not None
        ]
        source_files: list[Path] = []
        if len(source_headers) != 1:
            issues.append(
                f"{toc_relative} page {page_id!r} must declare exactly one "
                "source_files list"
            )
        else:
            for line in block[source_headers[0] + 1:]:
                indent = len(line) - len(line.lstrip())
                if line.strip() and indent <= 4:
                    break
                item = re.fullmatch(r"      -\s*(.*?)\s*(?:#.*)?", line)
                if item is None:
                    continue
                scalar = _yaml_scalar(item.group(1))
                if scalar is None:
                    issues.append(
                        f"{toc_relative} page {page_id!r} has an invalid source file"
                    )
                    continue
                source = _resolve_link(
                    toc_relative,
                    scalar,
                    repository_relative=True,
                )
                if source is None:
                    issues.append(
                        f"{toc_relative} page {page_id!r} source must stay inside "
                        f"the repository: {scalar!r}"
                    )
                    continue
                source_files.append(source)
            if not source_files:
                issues.append(
                    f"{toc_relative} page {page_id!r} must map at least one source file"
                )
            elif len(source_files) != len(set(source_files)):
                issues.append(
                    f"{toc_relative} page {page_id!r} repeats a source file"
                )

        section_starts = [
            offset
            for offset, line in enumerate(block)
            if YAML_SECTION_ID.match(line) is not None
        ]
        if not section_starts:
            issues.append(
                f"{toc_relative} page {page_id!r} must declare at least one section"
            )
        autogen_sections: list[str] = []
        for section_number, section_start in enumerate(section_starts):
            section_end = (
                section_starts[section_number + 1]
                if section_number + 1 < len(section_starts)
                else len(block)
            )
            section_match = YAML_SECTION_ID.match(block[section_start])
            assert section_match is not None
            section_id = _yaml_scalar(section_match.group(1))
            if (
                section_id is None
                or re.fullmatch(r"[A-Za-z0-9_.-]+", section_id) is None
            ):
                issues.append(
                    f"{toc_relative}:{start + section_start + 1} has an invalid "
                    "wiki section id"
                )
                continue
            if section_id in section_ids:
                issues.append(
                    f"{toc_relative} repeats wiki section id {section_id!r}"
                )
            section_ids.add(section_id)
            autogen_matches = [
                YAML_SECTION_AUTOGEN.match(line)
                for line in block[section_start + 1:section_end]
            ]
            autogen_values = [
                match.group(1).lower()
                for match in autogen_matches
                if match is not None
            ]
            if len(autogen_values) != 1:
                issues.append(
                    f"{toc_relative} section {section_id!r} must declare exactly "
                    "one autogen boolean"
                )
            elif autogen_values[0] == "true":
                autogen_sections.append(section_id)

        if filename is not None:
            pages.append(
                WikiPage(
                    page_id=page_id,
                    filename=filename,
                    source_files=tuple(source_files),
                    autogen_sections=tuple(autogen_sections),
                )
            )
    return pages, issues


def _wiki_state_sha256(
    repo_root: Path,
    toc_relative: Path,
    pages: list[WikiPage] | tuple[WikiPage, ...],
) -> str:
    """Hash the current TOC and generated pages, excluding derived evidence."""

    digest = hashlib.sha256()
    paths = [toc_relative, *(page.filename for page in pages)]
    for relative in paths:
        data = (repo_root / relative).read_bytes()
        encoded_path = relative.as_posix().encode("utf-8")
        digest.update(len(encoded_path).to_bytes(8, "big"))
        digest.update(encoded_path)
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def _tracked_paths(repo_root: Path, paths: set[Path]) -> tuple[set[Path], str | None]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--", *(str(path) for path in sorted(paths))],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        return set(), result.stderr.decode("utf-8", errors="replace").strip()
    return {
        Path(value.decode("utf-8", errors="surrogateescape"))
        for value in result.stdout.split(b"\0")
        if value
    }, None


def _check_tracked_wiki_pages(
    repo_root: Path,
    toc_relative: Path,
    pages: list[WikiPage],
) -> list[str]:
    issues: list[str] = []
    required = {toc_relative, *(page.filename for page in pages)}
    tracked, tracked_error = _tracked_paths(repo_root, required)
    if tracked_error is not None:
        issues.append(f"cannot inspect tracked wiki pages: {tracked_error}")
    else:
        for relative in sorted(required - tracked):
            issues.append(f"wiki page must be tracked: {relative}")

    result = subprocess.run(
        ["git", "ls-files", "-z", "--", str(toc_relative.parent)],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        issues.append(
            "cannot inspect tracked top-level wiki pages: "
            f"{result.stderr.decode('utf-8', errors='replace').strip()}"
        )
        return issues

    tracked_top_level: set[Path] = set()
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        path = Path(raw.decode("utf-8", errors="surrogateescape"))
        if path.parent == toc_relative.parent and path.suffix.lower() == ".md":
            tracked_top_level.add(path)
    expected_top_level = {page.filename for page in pages}
    for relative in sorted(tracked_top_level - expected_top_level):
        issues.append(
            f"tracked top-level wiki page is absent from {toc_relative}: {relative}"
        )
    return issues


def _path_matches_source(path: Path, source: Path) -> bool:
    candidate = path.as_posix()
    pattern = source.as_posix()
    if any(character in pattern for character in "*?["):
        return fnmatch.fnmatchcase(candidate, pattern)
    return candidate == pattern or candidate.startswith(f"{pattern.rstrip('/')}/")


def _check_wiki_source_drift(
    repo_root: Path,
    ref: str,
    sources: set[Path],
) -> list[str]:
    issues: list[str] = []
    changed = subprocess.run(
        ["git", "diff", "--no-renames", "--name-only", "-z", ref, "--"],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if changed.returncode != 0:
        return [
            "cannot compare mapped wiki sources with pinned ref "
            f"{ref}: {changed.stderr.decode('utf-8', errors='replace').strip()}"
        ]
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if untracked.returncode != 0:
        return [
            "cannot inspect untracked mapped wiki sources: "
            f"{untracked.stderr.decode('utf-8', errors='replace').strip()}"
        ]

    drifted: set[Path] = set()
    for raw in (*changed.stdout.split(b"\0"), *untracked.stdout.split(b"\0")):
        if not raw:
            continue
        path = Path(raw.decode("utf-8", errors="surrogateescape"))
        if any(_path_matches_source(path, source) for source in sources):
            drifted.add(path)
    for path in sorted(drifted):
        issues.append(
            f"mapped wiki source changed since pinned ref {ref}: {path}"
        )
    return issues


def _parse_autogen_sections(
    page: Path,
    text: str,
) -> tuple[dict[str, str], list[str]]:
    issues: list[str] = []
    sections: dict[str, str] = {}
    active_id: str | None = None
    active_start = 0
    content: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        begin = AUTOGEN_BEGIN.match(line)
        end = AUTOGEN_END.match(line)
        if "BEGIN:AUTOGEN" in line and begin is None:
            issues.append(f"{page}:{line_number} has a malformed AUTOGEN begin marker")
        if "END:AUTOGEN" in line and end is None:
            issues.append(f"{page}:{line_number} has a malformed AUTOGEN end marker")
        if begin is not None:
            if active_id is not None:
                issues.append(
                    f"{page}:{line_number} nests AUTOGEN section {begin.group(1)!r} "
                    f"inside {active_id!r}"
                )
            else:
                active_id = begin.group(1)
                active_start = line_number
                content = []
            continue
        if end is not None:
            section_id = end.group(1)
            if active_id is None:
                issues.append(
                    f"{page}:{line_number} closes unopened AUTOGEN section "
                    f"{section_id!r}"
                )
            elif section_id != active_id:
                issues.append(
                    f"{page}:{line_number} closes AUTOGEN section {section_id!r}, "
                    f"expected {active_id!r}"
                )
                active_id = None
                content = []
            else:
                if section_id in sections:
                    issues.append(
                        f"{page} repeats AUTOGEN section {section_id!r}"
                    )
                else:
                    sections[section_id] = "\n".join(content)
                active_id = None
                content = []
            continue
        if active_id is not None:
            content.append(line)
    if active_id is not None:
        issues.append(
            f"{page}:{active_start} leaves AUTOGEN section {active_id!r} open"
        )
    return sections, issues


def _git_file_line_count(
    repo_root: Path,
    ref: str,
    source: Path,
    cache: dict[Path, int | None],
) -> int | None:
    if source in cache:
        return cache[source]
    result = subprocess.run(
        ["git", "show", f"{ref}:{source.as_posix()}"],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        cache[source] = None
    else:
        cache[source] = len(result.stdout.splitlines())
    return cache[source]


def _check_autogen_citations(
    repo_root: Path,
    page: Path,
    section_id: str,
    content: str,
    *,
    repo_base_url: str,
    ref: str,
    line_counts: dict[Path, int | None],
) -> tuple[list[str], set[Path]]:
    issues: list[str] = []
    cited_sources: set[Path] = set()
    citation_count = 0
    base = repo_base_url.rstrip("/") + "/"
    for link in MARKDOWN_LINK_DETAIL.finditer(content):
        label = link.group(1).strip()
        target = _strip_markdown_title(link.group(2))
        if target.startswith("<") and target.endswith(">"):
            target = target[1:-1].strip()
        parsed = urlsplit(target)
        label_range = CITATION_LABEL_RANGE.search(label)
        has_line_fragment = re.fullmatch(
            r"L[0-9]+(?:-L[0-9]+)?",
            parsed.fragment,
        ) is not None
        is_blob_url = parsed.scheme in {"http", "https"} and "/blob/" in parsed.path
        if not target.startswith(base):
            if is_blob_url or has_line_fragment or label_range is not None:
                issues.append(
                    f"{page} AUTOGEN section {section_id!r} has an unpinned "
                    f"or foreign citation: {target}"
                )
            continue

        citation_count += 1
        remainder = target[len(base):]
        citation = CITATION_TARGET.fullmatch(remainder)
        if citation is None:
            issues.append(
                f"{page} AUTOGEN section {section_id!r} citation must use a full "
                f"commit and #Lstart[-Lend] range: {target}"
            )
            continue
        citation_ref, raw_source, raw_start, raw_end = citation.groups()
        if citation_ref.lower() != ref.lower():
            issues.append(
                f"{page} AUTOGEN section {section_id!r} citation pins "
                f"{citation_ref}, expected {ref}"
            )
        decoded_source = unquote(raw_source)
        source = _resolve_link(
            page,
            decoded_source,
            repository_relative=True,
        )
        if source is None or any(
            character in decoded_source for character in "*?["
        ):
            issues.append(
                f"{page} AUTOGEN section {section_id!r} citation has an invalid "
                f"repository path: {decoded_source!r}"
            )
            continue
        cited_sources.add(source)
        start_line = int(raw_start)
        end_line = int(raw_end or raw_start)
        if start_line < 1 or end_line < start_line:
            issues.append(
                f"{page} AUTOGEN section {section_id!r} citation has an invalid "
                f"line range {start_line}-{end_line}: {source}"
            )
            continue
        if label_range is not None:
            label_start = int(label_range.group(1))
            label_end = int(label_range.group(2) or label_range.group(1))
            if (label_start, label_end) != (start_line, end_line):
                issues.append(
                    f"{page} AUTOGEN section {section_id!r} citation label range "
                    f"{label_start}-{label_end} does not match target range "
                    f"{start_line}-{end_line}"
                )
        line_count = _git_file_line_count(
            repo_root,
            ref,
            source,
            line_counts,
        )
        if line_count is None:
            issues.append(
                f"{page} AUTOGEN section {section_id!r} citation source "
                f"{source} is missing at pinned ref {ref}"
            )
        elif end_line > line_count:
            issues.append(
                f"{page} AUTOGEN section {section_id!r} citation range "
                f"{start_line}-{end_line} exceeds {source} at pinned ref {ref} "
                f"({line_count} lines)"
            )
    if citation_count == 0:
        issues.append(
            f"{page} AUTOGEN section {section_id!r} has no pinned source citation"
        )
    return issues, cited_sources


def _load_wiki_artifact(
    repo_root: Path,
    relative: Path,
) -> tuple[dict[str, Any] | None, list[str]]:
    try:
        return _load_json(repo_root / relative), []
    except ValueError as exc:
        return None, [str(exc)]


def _check_artifact_binding(
    relative: Path,
    artifact: dict[str, Any],
    ref: str,
    wiki_state: str,
) -> list[str]:
    metadata = artifact.get("metadata")
    if not isinstance(metadata, dict):
        return [f"{relative} must contain a metadata object"]
    issues: list[str] = []
    if metadata.get("ref_commit_hash") != ref:
        issues.append(
            f"{relative} metadata.ref_commit_hash must equal wiki source pin {ref}"
        )
    if metadata.get("wiki_state_sha256") != wiki_state:
        issues.append(
            f"{relative} metadata.wiki_state_sha256 must equal current wiki state "
            f"{wiki_state}"
        )
    return issues


def _expect_artifact_value(
    issues: list[str],
    relative: Path,
    field: str,
    actual: object,
    expected: object,
) -> None:
    if actual != expected:
        issues.append(
            f"{relative} {field} is {actual!r}, expected {expected!r} "
            "for the current wiki"
        )


def _check_wiki_context_artifacts(
    repo_root: Path,
    *,
    ref: str,
    updated_at: str | None,
    pages: list[WikiPage],
    wiki_state: str,
) -> list[str]:
    issues: list[str] = []
    expected_page_ids = {page.page_id for page in pages}
    page_count = len(pages)
    context_pack_path, sync_path, update_path = WIKI_CONTEXT_ARTIFACTS
    context_pack, artifact_issues = _load_wiki_artifact(
        repo_root,
        context_pack_path,
    )
    issues.extend(artifact_issues)
    sync, artifact_issues = _load_wiki_artifact(repo_root, sync_path)
    issues.extend(artifact_issues)
    update, artifact_issues = _load_wiki_artifact(repo_root, update_path)
    issues.extend(artifact_issues)

    if context_pack is not None:
        issues.extend(
            _check_artifact_binding(
                context_pack_path,
                context_pack,
                ref,
                wiki_state,
            )
        )
    if sync is not None:
        issues.extend(
            _check_artifact_binding(sync_path, sync, ref, wiki_state)
        )
        toc_project = sync.get("toc_project")
        if not isinstance(toc_project, dict):
            issues.append(f"{sync_path} toc_project must be an object")
        else:
            _expect_artifact_value(
                issues,
                sync_path,
                "toc_project.ref_commit_hash",
                toc_project.get("ref_commit_hash"),
                ref,
            )
            _expect_artifact_value(
                issues,
                sync_path,
                "toc_project.updated_at",
                toc_project.get("updated_at"),
                updated_at,
            )
        metadata = sync.get("metadata")
        if isinstance(metadata, dict):
            for field, expected in (
                ("total_toc_pages", page_count),
                ("total_existing_docs", page_count),
                ("total_new_pages", 0),
                ("total_pages_to_update", 0),
                ("total_unchanged_pages", page_count),
                ("total_new_sections", 0),
                ("total_deleted_sections", 0),
                ("total_empty_sections_detected", 0),
            ):
                _expect_artifact_value(
                    issues,
                    sync_path,
                    f"metadata.{field}",
                    metadata.get(field),
                    expected,
                )
        _expect_artifact_value(
            issues,
            sync_path,
            "new_pages",
            sync.get("new_pages"),
            [],
        )
        _expect_artifact_value(
            issues,
            sync_path,
            "pages_to_update",
            sync.get("pages_to_update"),
            {},
        )
        unchanged_pages = sync.get("unchanged_pages")
        if (
            not isinstance(unchanged_pages, list)
            or len(unchanged_pages) != page_count
            or set(unchanged_pages) != expected_page_ids
        ):
            issues.append(
                f"{sync_path} unchanged_pages must name every current TOC page"
            )
    if update is not None:
        issues.extend(
            _check_artifact_binding(update_path, update, ref, wiki_state)
        )
        _expect_artifact_value(
            issues,
            update_path,
            "target_commit",
            update.get("target_commit"),
            ref,
        )
        _expect_artifact_value(
            issues,
            update_path,
            "toc_updated_at",
            update.get("toc_updated_at"),
            updated_at,
        )
    return issues


def _check_wiki_report_artifacts(
    repo_root: Path,
    *,
    ref: str,
    pages: list[WikiPage],
    wiki_state: str,
    mermaid_count: int,
) -> list[str]:
    issues: list[str] = []
    page_count = len(pages)
    section_count = sum(len(page.autogen_sections) for page in pages)
    summary_path, structure_path, mermaid_path = WIKI_REPORT_ARTIFACTS
    summary_text = (repo_root / summary_path).read_text(encoding="utf-8")
    summary_commit = SUMMARY_COMMIT.search(summary_text)
    if summary_commit is None or summary_commit.group(1) != ref:
        issues.append(f"{summary_path} Commit must equal wiki source pin {ref}")
    summary_state = SUMMARY_WIKI_STATE.search(summary_text)
    if summary_state is None or summary_state.group(1) != wiki_state:
        issues.append(
            f"{summary_path} Wiki state SHA-256 must equal current wiki state "
            f"{wiki_state}"
        )
    if "**Overall Status**: ✅ Complete" not in summary_text:
        issues.append(f"{summary_path} must report a complete generation")

    structure, artifact_issues = _load_wiki_artifact(repo_root, structure_path)
    issues.extend(artifact_issues)
    if structure is not None:
        issues.extend(
            _check_artifact_binding(structure_path, structure, ref, wiki_state)
        )
        structure_summary = structure.get("summary")
        if not isinstance(structure_summary, dict):
            issues.append(f"{structure_path} summary must be an object")
        else:
            for field, expected in (
                ("pages_validated", page_count),
                ("pages_missing", 0),
                ("sections_validated", section_count),
                ("sections_missing", 0),
                ("total_errors", 0),
                ("is_valid", True),
            ):
                _expect_artifact_value(
                    issues,
                    structure_path,
                    f"summary.{field}",
                    structure_summary.get(field),
                    expected,
                )
        _expect_artifact_value(
            issues,
            structure_path,
            "errors",
            structure.get("errors"),
            [],
        )

    mermaid, artifact_issues = _load_wiki_artifact(repo_root, mermaid_path)
    issues.extend(artifact_issues)
    if mermaid is not None:
        issues.extend(
            _check_artifact_binding(mermaid_path, mermaid, ref, wiki_state)
        )
        for field, expected in (
            ("invalid_blocks", []),
            ("total_invalid", 0),
            ("total_scanned", mermaid_count),
            ("files_affected", 0),
        ):
            _expect_artifact_value(
                issues,
                mermaid_path,
                field,
                mermaid.get(field),
                expected,
            )
    return issues


def _check_wiki_artifacts(
    repo_root: Path,
    *,
    ref: str,
    updated_at: str | None,
    pages: list[WikiPage],
    wiki_state: str,
    mermaid_count: int,
) -> list[str]:
    issues: list[str] = []
    required = set(WIKI_CONTEXT_ARTIFACTS + WIKI_REPORT_ARTIFACTS)
    tracked, tracked_error = _tracked_paths(repo_root, required)
    if tracked_error is not None:
        return [f"cannot inspect tracked wiki evidence: {tracked_error}"]
    for relative in sorted(required):
        if relative not in tracked:
            issues.append(f"wiki evidence artifact must be tracked: {relative}")
        if not (repo_root / relative).is_file():
            issues.append(f"wiki evidence artifact is missing: {relative}")
    if any(not (repo_root / relative).is_file() for relative in required):
        return issues
    issues.extend(
        _check_wiki_context_artifacts(
            repo_root,
            ref=ref,
            updated_at=updated_at,
            pages=pages,
            wiki_state=wiki_state,
        )
    )
    issues.extend(
        _check_wiki_report_artifacts(
            repo_root,
            ref=ref,
            pages=pages,
            wiki_state=wiki_state,
            mermaid_count=mermaid_count,
        )
    )
    return issues


def _check_wiki_snapshot(repo_root: Path, toc_relative: Path) -> list[str]:
    issues: list[str] = []
    toc_path = repo_root / toc_relative
    if not toc_path.is_file():
        return [f"wiki TOC is missing: {toc_relative}"]
    text = toc_path.read_text(encoding="utf-8")
    ref = _wiki_project_scalar(text, YAML_REF_COMMIT)
    updated_at = _wiki_project_scalar(text, YAML_UPDATED_AT)
    repo_base_url = _wiki_project_scalar(text, YAML_REPO_BASE_URL)
    if ref is None or re.fullmatch(r"[0-9a-fA-F]{40}", ref) is None:
        issues.append(f"{toc_relative} must pin a full 40-character commit hash")
        return issues
    ref = ref.lower()
    if updated_at is None:
        issues.append(f"{toc_relative} must declare updated_at")
    else:
        try:
            date.fromisoformat(updated_at)
        except ValueError:
            issues.append(f"{toc_relative} updated_at must be an ISO date")
    if repo_base_url != CANONICAL_WIKI_REPO_BASE_URL:
        issues.append(
            f"{toc_relative} project.repo_base_url must be canonical "
            f"{CANONICAL_WIKI_REPO_BASE_URL}"
        )

    pages, toc_issues = _parse_wiki_toc(toc_relative, text)
    issues.extend(toc_issues)

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
    ancestry = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ref, "HEAD"],
        cwd=repo_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ancestry.returncode == 1:
        issues.append(
            f"{toc_relative} pinned commit {ref} is not an ancestor of HEAD"
        )
    elif ancestry.returncode != 0:
        issues.append(
            f"cannot verify wiki pin ancestry for {ref}: "
            f"{ancestry.stderr.strip()}"
        )

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
    mapped_sources = _wiki_source_links(toc_relative, text)
    for source in sorted(mapped_sources, key=str):
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

    issues.extend(_check_tracked_wiki_pages(repo_root, toc_relative, pages))

    line_counts: dict[Path, int | None] = {}
    cited_sources: set[Path] = set()
    mermaid_count = 0
    for page in pages:
        page_path = repo_root / page.filename
        if not page_path.is_file():
            issues.append(f"wiki page is missing: {page.filename}")
            continue
        page_text = page_path.read_text(encoding="utf-8")
        page_id_matches = PAGE_ID_MARKER.findall(page_text)
        if page_id_matches != [page.page_id]:
            issues.append(
                f"{page.filename} must declare exactly PAGE_ID {page.page_id!r}"
            )
        sections, section_issues = _parse_autogen_sections(
            page.filename,
            page_text,
        )
        issues.extend(section_issues)
        expected_sections = set(page.autogen_sections)
        actual_sections = set(sections)
        for section_id in sorted(expected_sections - actual_sections):
            issues.append(
                f"{page.filename} is missing AUTOGEN section {section_id!r}"
            )
        for section_id in sorted(actual_sections - expected_sections):
            issues.append(
                f"{page.filename} has unexpected AUTOGEN section {section_id!r}"
            )
        if repo_base_url is not None:
            for section_id in sorted(expected_sections & actual_sections):
                citation_issues, section_sources = _check_autogen_citations(
                    repo_root,
                    page.filename,
                    section_id,
                    sections[section_id],
                    repo_base_url=repo_base_url,
                    ref=ref,
                    line_counts=line_counts,
                )
                issues.extend(citation_issues)
                cited_sources.update(section_sources)
        mermaid_count += len(MERMAID_FENCE.findall(page_text))

    issues.extend(
        _check_wiki_source_drift(
            repo_root,
            ref,
            mapped_sources | cited_sources,
        )
    )
    if pages and all((repo_root / page.filename).is_file() for page in pages):
        wiki_state = _wiki_state_sha256(repo_root, toc_relative, pages)
        issues.extend(
            _check_wiki_artifacts(
                repo_root,
                ref=ref,
                updated_at=updated_at,
                pages=pages,
                wiki_state=wiki_state,
                mermaid_count=mermaid_count,
            )
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
