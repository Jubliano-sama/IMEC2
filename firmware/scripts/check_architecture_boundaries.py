#!/usr/bin/env python3
"""Prevent known oversized translation units from accumulating more behavior."""

from __future__ import annotations

import io
import json
import posixpath
import re
import subprocess
import sys
import tarfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path("firmware/architecture_boundaries.json")
INCLUDE_DIRECTIVE = re.compile(
    r"^[^\S\r\n]*#[^\S\r\n]*include[^\S\r\n]+([^\r\n]+)",
    re.MULTILINE,
)
QUOTED_INCLUDE = re.compile(r'^"([^"]+)"')
ANGLE_INCLUDE = re.compile(r"^<([^>]+)>")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
CMAKE_TRANSLATION_UNIT = re.compile(
    r'"([^"\r\n]+\.(?:c|cc|cpp|cxx|C|i|ii|m|mm|cu|hip|s|S|asm|'
    r'f|for|f77|f90|f95|f03|f08|F|FOR|F77|F90|F95|F03|F08))"'
    r"|(?<![A-Za-z0-9_./${}-])"
    r"([A-Za-z0-9_./${}-]+\.(?:c|cc|cpp|cxx|C|i|ii|m|mm|cu|hip|"
    r"s|S|asm|f|for|f77|f90|f95|f03|f08|F|FOR|F77|F90|F95|F03|F08))"
    r"(?![A-Za-z0-9_./${}-])"
)
CMAKE_COMMAND_START = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*\(",
)
CMAKE_DYNAMIC_REFERENCE = re.compile(
    r"\$\{[^}]+\}|\$(?:CACHE|ENV)\{[^}]+\}",
)
CMAKE_ARGUMENT = re.compile(r'"(?:\\.|[^"])*"|[^ \t\r\n]+')
IMMUTABLE_BASELINE_COMMIT = "e7f21bd58157351a328457c5b2d4fbea52285c49"
MAX_HEADER_LINES = 1000
MAX_EXISTING_HEADER_CLOSURE_GROWTH = 250
MAX_NEW_HEADER_CLOSURE_LINES = 4000
TRANSLATION_UNIT_SUFFIXES = frozenset(
    {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".C",
        ".i",
        ".ii",
        ".m",
        ".mm",
        ".cu",
        ".hip",
        ".s",
        ".S",
        ".asm",
        ".f",
        ".for",
        ".f77",
        ".f90",
        ".f95",
        ".f03",
        ".f08",
        ".F",
        ".FOR",
        ".F77",
        ".F90",
        ".F95",
        ".F03",
        ".F08",
    }
)
UNSUPPORTED_PRODUCTION_SUFFIXES = TRANSLATION_UNIT_SUFFIXES - {".c"}
NON_PRODUCTION_C_ROOTS = (
    Path("firmware/tests"),
    Path("firmware/app/tests"),
    Path("firmware/battery_usb_test"),
    Path("firmware/led_pulse_test"),
    Path("firmware/power_profile_test"),
    Path("firmware/twr_range_test"),
    Path("firmware/uwb_smoke_test"),
)
APP_CMAKE_VENDOR_SOURCE = "${DWM3000_SDK_DIR}/decadriver/deca_device.c"
APP_CMAKE_VENDOR_INCLUDE = "${DWM3000_SDK_DIR}/decadriver"
APPROVED_DWM_SDK_DEFINITION = (
    'DWM3000_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dwm3000 examples and sdk"'
)
APPROVED_APP_INCLUDE_DIRECTORIES = (
    f'app PRIVATE ../include "{APP_CMAKE_VENDOR_INCLUDE}"'
)
APPROVED_CMAKE_SOURCE_REFERENCES = frozenset(
    {"${DWM3000_SDK_DIR}", "${IMEC_APP_LOCAL_SOURCES}"}
)
CMAKE_SOURCE_COMMANDS = frozenset(
    {
        "add_executable",
        "add_library",
        "set_property",
        "target_sources",
        "zephyr_library_sources",
        "zephyr_library_sources_ifdef",
        "zephyr_library_sources_ifndef",
        "zephyr_library_sources_if_kconfig",
        "zephyr_sources",
        "zephyr_sources_ifdef",
        "zephyr_sources_ifndef",
        "zephyr_sources_if_kconfig",
    }
)
CMAKE_DYNAMIC_EVALUATION_COMMANDS = frozenset({"cmake_language"})
CMAKE_SOURCE_DISCOVERY_FILE_SUBCOMMANDS = frozenset({"GLOB", "GLOB_RECURSE"})
@dataclass(frozen=True)
class _Composition:
    max_lines: int
    includes: tuple[str, ...]


@dataclass(frozen=True)
class _Manifest:
    source_roots: tuple[Path, ...]
    default_c_max_lines: int
    frozen_oversize_sources: Mapping[Path, int]
    approved_include_fragments: Mapping[Path, int]
    composed_translation_units: Mapping[Path, _Composition]


@dataclass(frozen=True)
class _Include:
    form: str
    target: str


@dataclass(frozen=True)
class _CMakeCommand:
    name: str
    body: str
    terminated: bool


def _line_count(path: Path) -> int:
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return sum(1 for _ in stream)


def _text_line_count(text: str) -> int:
    return text.count("\n") + int(bool(text) and not text.endswith("\n"))


def _normalize_preprocessor_spelling(text: str) -> str:
    """Apply the source transformations which can change include directives."""

    text = text.replace("??=", "#").replace("??/", "\\")
    text = re.sub(r"\\(?:\r\n|\n|\r)", "", text)
    text = BLOCK_COMMENT.sub(
        lambda match: " " + ("\n" * match.group(0).count("\n")),
        text,
    )
    return re.sub(r"^([ \t]*)%:", r"\1#", text, flags=re.MULTILINE)


def _parse_includes(text: str) -> list[_Include]:
    text = _normalize_preprocessor_spelling(text)
    includes: list[_Include] = []
    for raw_token in INCLUDE_DIRECTIVE.findall(text):
        token = raw_token.strip()
        quoted = QUOTED_INCLUDE.match(token)
        if quoted is not None:
            includes.append(_Include("quoted", quoted.group(1)))
            continue
        angled = ANGLE_INCLUDE.match(token)
        if angled is not None:
            includes.append(_Include("angle", angled.group(1)))
            continue
        includes.append(_Include("macro", token))
    return includes


def _includes(path: Path) -> list[_Include]:
    return _parse_includes(path.read_text(encoding="utf-8", errors="replace"))


def _inc_includes(path: Path) -> list[str]:
    return [
        include.target
        for include in _includes(path)
        if include.form == "quoted" and Path(include.target).suffix == ".inc"
    ]


def _is_within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _repository_policy_files(repo_root: Path) -> set[Path]:
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            "firmware",
        ],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode == 0:
        return {
            Path(raw.decode("utf-8", errors="surrogateescape"))
            for raw in result.stdout.split(b"\0")
            if raw
        }
    firmware_root = repo_root / "firmware"
    if not firmware_root.is_dir():
        return set()
    return {
        path.relative_to(repo_root)
        for path in firmware_root.rglob("*")
        if path.is_file() or path.is_symlink()
    }


def _source_texts(
    repo_root: Path,
    policy_files: set[Path],
) -> dict[Path, str]:
    sources: dict[Path, str] = {}
    for relative in policy_files:
        if relative.suffix not in {".c", ".h"}:
            continue
        path = repo_root / relative
        if path.is_file() and not path.is_symlink():
            sources[relative] = path.read_text(
                encoding="utf-8",
                errors="replace",
            )
    return sources


def _resolve_header(
    owner: Path,
    target: str,
    sources: Mapping[Path, str],
    search_roots: tuple[Path, ...],
) -> Path | None:
    target_path = Path(target)
    if target_path.is_absolute() or ".." in target_path.parts:
        return None
    direct = Path(posixpath.normpath((owner.parent / target_path).as_posix()))
    if direct in sources and direct.suffix == ".h":
        return direct
    for root in search_roots:
        candidate = Path(posixpath.normpath((root / target_path).as_posix()))
        if candidate in sources and candidate.suffix == ".h":
            return candidate
    return None


def _header_closure_lines(
    source: Path,
    sources: Mapping[Path, str],
    source_roots: tuple[Path, ...],
) -> int:
    if source not in sources:
        return 0
    search_roots = (Path("firmware/include"), *source_roots)
    pending: list[Path] = []
    for include in _parse_includes(sources[source]):
        if Path(include.target).suffix != ".h":
            continue
        header = _resolve_header(source, include.target, sources, search_roots)
        if header is not None:
            pending.append(header)
    seen: set[Path] = set()
    while pending:
        header = pending.pop()
        if header in seen:
            continue
        seen.add(header)
        for include in _parse_includes(sources[header]):
            if Path(include.target).suffix != ".h":
                continue
            nested = _resolve_header(
                header,
                include.target,
                sources,
                search_roots,
            )
            if nested is not None:
                pending.append(nested)
    return sum(_text_line_count(sources[header]) for header in seen)


def _include_policy_issues(relative: Path, path: Path) -> list[str]:
    issues: list[str] = []
    for include in _includes(path):
        suffix = Path(include.target).suffix
        target_path = Path(include.target)
        if include.form == "quoted" and (
            target_path.is_absolute() or ".." in target_path.parts
        ):
            issues.append(
                f"{relative} includes header outside its approved source roots "
                f"with {include.target!r}; quoted includes may not be absolute "
                "or use parent traversal"
            )
        elif include.form == "macro":
            issues.append(
                f"{relative} uses non-literal include {include.target!r}; "
                "architecture ownership requires literal headers or declared fragments"
            )
        elif include.form == "quoted" and suffix not in {".h", ".inc"}:
            issues.append(
                f"{relative} includes source-like file {include.target!r}; "
                "quoted project includes must be .h or declared .inc files"
            )
        elif (
            include.form == "quoted"
            and suffix == ".inc"
            and relative.suffix != ".c"
        ):
            issues.append(
                f"{relative} includes fragment {include.target!r}; fragments "
                "must be owned directly by one declared C shell"
            )
        elif include.form == "angle" and suffix != ".h":
            issues.append(
                f"{relative} includes source-like angle target {include.target!r}; "
                "angle includes must name headers"
            )
    return issues


def _strip_cmake_comments(text: str) -> str:
    stripped: list[str] = []
    for line in text.splitlines(keepends=True):
        quoted = False
        escaped = False
        end = len(line)
        for index, character in enumerate(line):
            if escaped:
                escaped = False
                continue
            if character == "\\":
                escaped = True
                continue
            if character == '"':
                quoted = not quoted
                continue
            if character == "#" and not quoted:
                end = index
                break
        suffix = "\n" if line.endswith("\n") else ""
        stripped.append(line[:end].rstrip("\r\n") + suffix)
    return "".join(stripped)


def _cmake_commands(text: str) -> list[_CMakeCommand]:
    commands: list[_CMakeCommand] = []
    position = 0
    while True:
        match = CMAKE_COMMAND_START.search(text, position)
        if match is None:
            break
        depth = 1
        index = match.end()
        body_start = index
        quoted = False
        escaped = False
        while index < len(text) and depth:
            character = text[index]
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = not quoted
            elif not quoted and character == "[":
                opening = re.match(r"\[(=*)\[", text[index:])
                if opening is not None:
                    closing = "]" + opening.group(1) + "]"
                    closing_index = text.find(
                        closing,
                        index + len(opening.group(0)),
                    )
                    if closing_index < 0:
                        index = len(text)
                        break
                    index = closing_index + len(closing)
                    continue
            elif not quoted and character == "(":
                depth += 1
            elif not quoted and character == ")":
                depth -= 1
            index += 1
        terminated = depth == 0
        body_end = index - 1 if terminated else len(text)
        commands.append(
            _CMakeCommand(
                name=match.group(1).lower(),
                body=text[body_start:body_end],
                terminated=terminated,
            )
        )
        position = index if terminated else len(text)
    return commands


def _normalized_cmake_body(body: str) -> str:
    return " ".join(body.split())


def _cmake_first_argument(body: str) -> str | None:
    body = body.lstrip()
    if not body:
        return None
    if body.startswith('"'):
        escaped = False
        for index, character in enumerate(body[1:], start=1):
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                return body[1:index]
        return None
    opening = re.match(r"\[(=*)\[", body)
    if opening is not None:
        closing = "]" + opening.group(1) + "]"
        closing_index = body.find(closing, len(opening.group(0)))
        if closing_index < 0:
            return None
        return body[len(opening.group(0)):closing_index]
    match = re.match(r"[^ \t\r\n]+", body)
    return match.group(0) if match is not None else None


def _cmake_dynamic_source_issues(
    cmake_relative: Path,
    text: str,
) -> list[str]:
    issues: list[str] = []
    dwm_sdk_definitions = 0
    dwm_sdk_references = 0
    local_source_list_definitions = 0
    local_source_list_references = 0
    for command in _cmake_commands(text):
        if not command.terminated:
            issues.append(
                f"{cmake_relative} has unterminated CMake command "
                f"{command.name}; production source ownership cannot be audited"
            )
            continue

        body = _normalized_cmake_body(command.body)
        arguments = CMAKE_ARGUMENT.findall(body)
        if command.name == "aux_source_directory":
            issues.append(
                f"{cmake_relative} uses aux_source_directory; production "
                "source ownership must remain literal and explicit"
            )
        if command.name in CMAKE_DYNAMIC_EVALUATION_COMMANDS:
            issues.append(
                f"{cmake_relative} uses {command.name}; dynamically evaluated "
                "production source declarations are not permitted"
            )
        if command.name == "file":
            subcommand = body.split(maxsplit=1)[0].strip('"').upper() if body else ""
            if subcommand in CMAKE_SOURCE_DISCOVERY_FILE_SUBCOMMANDS:
                issues.append(
                    f"{cmake_relative} uses file({subcommand}); production "
                    "source ownership must remain literal and explicit"
                )
        if command.name == "set_source_files_properties" or (
            command.name == "set_property"
            and re.search(r"\bSOURCE\b", body, re.IGNORECASE)
            and re.search(r"\bLANGUAGE\b", body, re.IGNORECASE)
        ):
            issues.append(
                f"{cmake_relative} overrides a source language; production "
                "translation units must use their audited file suffix"
            )
        if "include_director" in command.name:
            if (
                cmake_relative == Path("firmware/app/CMakeLists.txt")
                and command.name == "target_include_directories"
                and body == APPROVED_APP_INCLUDE_DIRECTORIES
            ):
                pass
            else:
                issues.append(
                    f"{cmake_relative} changes production include directories "
                    f"through {command.name}; only the pinned app and DWM3000 "
                    "include roots are permitted"
                )
        if (
            command.name in {"set_property", "set_target_properties"}
            and re.search(
                r"\b(?:INTERFACE_)?INCLUDE_DIRECTORIES\b",
                body,
                re.IGNORECASE,
            )
        ):
            issues.append(
                f"{cmake_relative} changes production include directories "
                f"through {command.name}; include roots must use the pinned "
                "target_include_directories command"
            )

        references = CMAKE_DYNAMIC_REFERENCE.findall(command.body)
        if command.name in CMAKE_SOURCE_COMMANDS:
            unsupported = sorted(
                {
                    reference
                    for reference in references
                    if reference not in APPROVED_CMAKE_SOURCE_REFERENCES
                }
            )
            if unsupported:
                issues.append(
                    f"{cmake_relative} has variable-driven sources "
                    f"{unsupported} in {command.name}; source paths must be literal"
                )
            if "$<" in command.body:
                issues.append(
                    f"{cmake_relative} has generator-expression-driven sources "
                    f"in {command.name}; source paths must be literal"
                )
            if ";" in command.body:
                issues.append(
                    f"{cmake_relative} has a semicolon source list in "
                    f"{command.name}; source paths must be separate literals"
                )
        if command.name in CMAKE_SOURCE_COMMANDS - {"set_property", "target_sources"}:
            issues.append(
                f"{cmake_relative} uses alternate source command "
                f"{command.name}; production sources must use target_sources"
            )
        if (
            command.name in {"set_property", "set_target_properties"}
            and re.search(r"\bSOURCES\b", body, re.IGNORECASE)
        ):
            issues.append(
                f"{cmake_relative} changes production sources through "
                f"{command.name}; production sources must use target_sources"
            )

        defines_dwm_sdk = (
            command.name == "set"
            and bool(arguments)
            and arguments[0] == "DWM3000_SDK_DIR"
        )
        dwm_arguments = [
            argument.strip('"')
            for argument in arguments
            if "${DWM3000_SDK_DIR}" in argument
        ]
        if defines_dwm_sdk:
            dwm_sdk_definitions += 1
            if (
                cmake_relative != Path("firmware/app/CMakeLists.txt")
                or body != APPROVED_DWM_SDK_DEFINITION
            ):
                issues.append(
                    f"{cmake_relative} changes DWM3000_SDK_DIR; it must be "
                    "defined once as the pinned repository submodule path"
                )
        elif dwm_arguments:
            dwm_sdk_references += len(dwm_arguments)
            allowed = (
                command.name == "target_sources"
                and all(
                    argument == APP_CMAKE_VENDOR_SOURCE
                    for argument in dwm_arguments
                )
            ) or (
                command.name == "target_include_directories"
                and all(
                    argument == APP_CMAKE_VENDOR_INCLUDE
                    for argument in dwm_arguments
                )
            )
            if not allowed:
                issues.append(
                    f"{cmake_relative} uses DWM3000_SDK_DIR outside its pinned "
                    "vendor source/include paths"
                )
        elif "DWM3000_SDK_DIR" in command.body:
            issues.append(
                f"{cmake_relative} mutates DWM3000_SDK_DIR through "
                f"{command.name}; only one literal set() is permitted"
            )

        if (
            command.name == "set"
            and arguments
            and CMAKE_DYNAMIC_REFERENCE.fullmatch(arguments[0])
        ):
            issues.append(
                f"{cmake_relative} uses an indirect CMake variable assignment; "
                "production source variables must be named literally"
            )

        defines_local_sources = (
            command.name == "set"
            and bool(arguments)
            and arguments[0] == "IMEC_APP_LOCAL_SOURCES"
        )
        references_local_sources = (
            command.name == "target_sources"
            and "${IMEC_APP_LOCAL_SOURCES}" in command.body
        )
        if defines_local_sources:
            local_source_list_definitions += 1
            values = [argument.strip('"') for argument in arguments[1:]]
            if (
                references
                or "$<" in command.body
                or ";" in command.body
                or not values
                or any(Path(value).suffix != ".c" for value in values)
            ):
                issues.append(
                    f"{cmake_relative} defines IMEC_APP_LOCAL_SOURCES "
                    "dynamically or with non-C entries; its entries must be "
                    "separate literal .c paths"
                )
        elif references_local_sources:
            local_source_list_references += 1
        elif "IMEC_APP_LOCAL_SOURCES" in command.body:
            issues.append(
                f"{cmake_relative} mutates IMEC_APP_LOCAL_SOURCES through "
                f"{command.name}; only one literal set() is permitted"
            )

    if local_source_list_references and local_source_list_definitions != 1:
        issues.append(
            f"{cmake_relative} references IMEC_APP_LOCAL_SOURCES but has "
            f"{local_source_list_definitions} literal definitions; exactly one is required"
        )
    if dwm_sdk_references and dwm_sdk_definitions != 1:
        issues.append(
            f"{cmake_relative} has {dwm_sdk_definitions} DWM3000_SDK_DIR "
            "definitions for pinned vendor inputs; exactly one is required"
        )
    return issues


def _normalized_repo_path(base: Path, raw_path: str) -> Path:
    return Path(posixpath.normpath((base / Path(raw_path)).as_posix()))


def _app_cmake_source_issues(
    repo_root: Path,
    source_roots: tuple[Path, ...],
    policy_files: set[Path],
) -> list[str]:
    main_relative = Path("firmware/app/CMakeLists.txt")
    if not (repo_root / main_relative).is_file():
        return [f"{main_relative} is missing"]

    issues: list[str] = []
    app_source_dir = Path("firmware/app")
    pending = [(main_relative, app_source_dir)]
    visited: set[tuple[Path, Path]] = set()
    while pending:
        cmake_relative, source_dir = pending.pop()
        context = (cmake_relative, source_dir)
        if context in visited:
            continue
        visited.add(context)
        cmake_path = repo_root / cmake_relative
        if not cmake_path.is_file():
            issues.append(
                f"production CMake include is missing: {cmake_relative}"
            )
            continue
        if cmake_path.is_symlink() or cmake_relative not in policy_files:
            issues.append(
                f"production CMake input {cmake_relative} is generated, ignored, "
                "or symlinked; build ownership requires a repository policy file"
            )
            continue
        text = _strip_cmake_comments(
            cmake_path.read_text(encoding="utf-8", errors="replace")
        )
        issues.extend(_cmake_dynamic_source_issues(cmake_relative, text))

        for source_command in _cmake_commands(text):
            if source_command.name != "target_sources":
                continue
            source_arguments = CMAKE_ARGUMENT.findall(
                _normalized_cmake_body(source_command.body)
            )
            if not source_arguments:
                issues.append(
                    f"{cmake_relative} has unreadable target_sources arguments; "
                    "production source ownership cannot be audited"
                )
                continue
            for raw_argument in source_arguments[1:]:
                raw_source = raw_argument.strip('"')
                if raw_source in {"INTERFACE", "PRIVATE", "PUBLIC"}:
                    continue
                if raw_source in {
                    APP_CMAKE_VENDOR_SOURCE,
                    "${IMEC_APP_LOCAL_SOURCES}",
                }:
                    continue
                if (
                    CMAKE_DYNAMIC_REFERENCE.search(raw_source)
                    or "$<" in raw_source
                    or ";" in raw_source
                ):
                    continue
                if Path(raw_source).suffix != ".c":
                    issues.append(
                        f"{cmake_relative} adds unsupported production source "
                        f"entry {raw_source!r}; target_sources permits only "
                        "literal .c policy files"
                    )
                    continue
                normalized = _normalized_repo_path(source_dir, raw_source)
                normalized_path = repo_root / normalized
                if not any(
                    _is_within(normalized, root) for root in source_roots
                ):
                    issues.append(
                        f"{cmake_relative} sources {raw_source!r} outside "
                        f"declared production roots {list(source_roots)}"
                    )
                elif (
                    normalized not in policy_files
                    or not normalized_path.is_file()
                    or normalized_path.is_symlink()
                ):
                    issues.append(
                        f"{cmake_relative} sources generated or out-of-policy "
                        f"translation unit {raw_source!r}; production inputs "
                        "must exist as repository policy files"
                    )

        for dependency_command in _cmake_commands(text):
            command = dependency_command.name
            if command not in {"include", "add_subdirectory"}:
                continue
            raw_dependency = _cmake_first_argument(dependency_command.body)
            if raw_dependency is None:
                issues.append(
                    f"{cmake_relative} has unreadable {command} target; "
                    "production build ownership must remain explicit"
                )
                continue
            if (
                CMAKE_DYNAMIC_REFERENCE.search(raw_dependency)
                or "$<" in raw_dependency
                or ";" in raw_dependency
            ):
                issues.append(
                    f"{cmake_relative} has unresolved {command} target "
                    f"{raw_dependency!r}; production build ownership must remain explicit"
                )
                continue
            dependency = _normalized_repo_path(
                source_dir,
                raw_dependency,
            )
            dependency_source_dir = source_dir
            if command == "include" and not dependency.suffix:
                dependency = dependency.with_suffix(".cmake")
            elif command == "add_subdirectory":
                dependency_source_dir = dependency
                dependency = dependency_source_dir / "CMakeLists.txt"
            if (
                dependency.is_absolute()
                or ".." in dependency.parts
                or not _is_within(dependency, Path("firmware/app"))
            ):
                issues.append(
                    f"{cmake_relative} reaches CMake input {raw_dependency!r} "
                    "outside firmware/app"
                )
                continue
            pending.append((dependency, dependency_source_dir))

        for match in CMAKE_TRANSLATION_UNIT.finditer(text):
            raw_source = match.group(1) or match.group(2)
            if raw_source == APP_CMAKE_VENDOR_SOURCE:
                continue
            if "${" in raw_source or "$<" in raw_source or ";" in raw_source:
                issues.append(
                    f"{cmake_relative} has an unresolved translation-unit token "
                    f"{raw_source!r}; production source ownership must remain literal"
                )
                continue
            normalized = _normalized_repo_path(
                source_dir,
                raw_source,
            )
            if Path(raw_source).suffix in UNSUPPORTED_PRODUCTION_SUFFIXES:
                issues.append(
                    f"{cmake_relative} adds unsupported production translation "
                    f"unit {raw_source!r}; architecture policy permits only .c"
                )
            if (
                normalized.is_absolute()
                or ".." in normalized.parts
                or not any(_is_within(normalized, root) for root in source_roots)
            ):
                issues.append(
                    f"{cmake_relative} sources {raw_source!r} outside "
                    f"declared production roots {list(source_roots)}"
                )
                continue
            normalized_path = repo_root / normalized
            if (
                normalized not in policy_files
                or not normalized_path.is_file()
                or normalized_path.is_symlink()
            ):
                issues.append(
                    f"{cmake_relative} sources generated or out-of-policy "
                    f"translation unit {raw_source!r}; production inputs must "
                    "exist as repository policy files"
                )
    return issues


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


def _require_immutable_baseline_ancestor(repo_root: Path) -> None:
    result = subprocess.run(
        [
            "git",
            "merge-base",
            "--is-ancestor",
            IMMUTABLE_BASELINE_COMMIT,
            "HEAD",
        ],
        cwd=repo_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        return
    if result.returncode == 1:
        raise ValueError(
            f"immutable architecture baseline {IMMUTABLE_BASELINE_COMMIT} is "
            "present but is not an ancestor of HEAD; published history must "
            "retain the exact policy commit. Do not squash, rebase, prune, or "
            "reconstruct it"
        )
    detail = result.stderr.strip()
    raise ValueError(
        f"cannot prove immutable architecture baseline "
        f"{IMMUTABLE_BASELINE_COMMIT} is an ancestor of HEAD. Do not squash, "
        "rebase, prune, or reconstruct this policy object; an intentional "
        "rebaseline requires two separately reviewed, preserved commits: "
        f"{detail}"
    )


def _load_immutable_baseline(repo_root: Path) -> dict[str, Any]:
    _require_immutable_baseline_ancestor(repo_root)
    reference = f"{IMMUTABLE_BASELINE_COMMIT}:{DEFAULT_MANIFEST.as_posix()}"
    result = subprocess.run(
        ["git", "show", reference],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip()
        raise ValueError(
            f"cannot load immutable architecture baseline {reference}; "
            "the checkout must contain the pinned history. Do not squash, "
            "rebase, prune, or reconstruct this policy object; an intentional "
            "rebaseline requires two separately reviewed, preserved commits: "
            f"{detail}"
        )
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ValueError(
            f"immutable architecture baseline {reference} is invalid JSON: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise ValueError(
            f"immutable architecture baseline {reference} must be a JSON object"
        )
    return value


def _load_immutable_sources(repo_root: Path) -> dict[Path, str]:
    _require_immutable_baseline_ancestor(repo_root)
    result = subprocess.run(
        [
            "git",
            "archive",
            "--format=tar",
            IMMUTABLE_BASELINE_COMMIT,
            "firmware",
        ],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(
            f"cannot load source tree from immutable "
            f"{IMMUTABLE_BASELINE_COMMIT} baseline; do not squash, rebase, "
            "prune, or reconstruct this policy object. An intentional "
            "rebaseline requires two separately reviewed, preserved commits: "
            f"{detail}"
        )
    sources: dict[Path, str] = {}
    try:
        with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
            for member in archive.getmembers():
                relative = Path(member.name)
                if not member.isfile() or relative.suffix not in {".c", ".h"}:
                    continue
                stream = archive.extractfile(member)
                if stream is None:
                    raise ValueError(
                        f"cannot read {relative} from immutable source archive"
                    )
                sources[relative] = stream.read().decode(
                    "utf-8",
                    errors="replace",
                )
    except tarfile.TarError as exc:
        raise ValueError(
            f"immutable {IMMUTABLE_BASELINE_COMMIT} source archive is invalid: {exc}"
        ) from exc
    return sources


def _parse_manifest(value: object, label: str) -> _Manifest:
    if not isinstance(value, dict) or value.get("schema") != 1:
        raise ValueError(f"{label} must contain a schema-1 object")

    raw_roots = value.get("source_roots")
    raw_frozen = value.get("frozen_oversize_sources")
    raw_fragments = value.get("approved_include_fragments")
    raw_compositions = value.get("composed_translation_units")
    if not isinstance(raw_roots, list) or not raw_roots:
        raise ValueError(f"{label}.source_roots must be a non-empty list")
    if not isinstance(raw_frozen, dict):
        raise ValueError(f"{label}.frozen_oversize_sources must be an object")
    if not isinstance(raw_fragments, dict):
        raise ValueError(f"{label}.approved_include_fragments must be an object")
    if not isinstance(raw_compositions, dict):
        raise ValueError(f"{label}.composed_translation_units must be an object")

    source_roots = tuple(
        _relative_path(value, f"{label}.source_roots[{index}]")
        for index, value in enumerate(raw_roots)
    )
    frozen = {
        _relative_path(path, f"{label}.frozen_oversize_sources.{path}"):
        _positive_int(limit, f"{label}.frozen_oversize_sources.{path}")
        for path, limit in raw_frozen.items()
    }
    fragments = {
        _relative_path(path, f"{label}.approved_include_fragments.{path}"):
        _positive_int(limit, f"{label}.approved_include_fragments.{path}")
        for path, limit in raw_fragments.items()
    }
    compositions: dict[Path, _Composition] = {}
    for raw_source, raw_config in raw_compositions.items():
        source = _relative_path(
            raw_source,
            f"{label}.composed_translation_units.{raw_source}",
        )
        if not isinstance(raw_config, dict):
            raise ValueError(
                f"{label}.composed_translation_units.{raw_source} "
                "must be an object"
            )
        limit = _positive_int(
            raw_config.get("max_composed_lines"),
            (
                f"{label}.composed_translation_units."
                f"{raw_source}.max_composed_lines"
            ),
        )
        raw_includes = raw_config.get("includes")
        if not isinstance(raw_includes, list):
            raise ValueError(
                f"{label}.composed_translation_units."
                f"{raw_source}.includes must be a list"
            )
        includes = tuple(
            _relative_path(
                include,
                f"{label}.composed_translation_units.{raw_source}.includes",
            ).as_posix()
            for include in raw_includes
        )
        compositions[source] = _Composition(limit, includes)

    return _Manifest(
        source_roots=source_roots,
        default_c_max_lines=_positive_int(
            value.get("default_c_max_lines"),
            f"{label}.default_c_max_lines",
        ),
        frozen_oversize_sources=frozen,
        approved_include_fragments=fragments,
        composed_translation_units=compositions,
    )


def _is_ordered_subset(values: tuple[str, ...], baseline: tuple[str, ...]) -> bool:
    baseline_iter = iter(baseline)
    return all(any(candidate == value for candidate in baseline_iter) for value in values)


def _baseline_policy_issues(
    repo_root: Path,
    current: _Manifest,
    baseline: _Manifest,
) -> list[str]:
    issues: list[str] = []
    baseline_label = f"immutable {IMMUTABLE_BASELINE_COMMIT} baseline"

    if current.default_c_max_lines > baseline.default_c_max_lines:
        issues.append(
            "default_c_max_lines raised from the "
            f"{baseline_label} ceiling of {baseline.default_c_max_lines} "
            f"to {current.default_c_max_lines}"
        )

    if len(set(current.source_roots)) != len(current.source_roots):
        issues.append("source_roots repeats an entry; approved roots must be unique")

    approved_roots = set(baseline.source_roots)
    for root in baseline.source_roots:
        if root not in current.source_roots:
            issues.append(
                f"source_roots no longer cover baseline root {root}; "
                "the checked source scope may not be weakened"
            )
    for root in current.source_roots:
        if root not in approved_roots:
            issues.append(
                f"source_roots adds unapproved root {root}; production source "
                f"roots are frozen by the {baseline_label}"
            )

    for path, limit in current.frozen_oversize_sources.items():
        baseline_limit = baseline.frozen_oversize_sources.get(path)
        if baseline_limit is None:
            issues.append(
                f"{path} is a new frozen source exception; reduce it below "
                "the default ceiling instead"
            )
        elif limit > baseline_limit:
            issues.append(
                f"{path} frozen ceiling raised from {baseline_limit} to {limit}"
            )

    for path, limit in current.approved_include_fragments.items():
        baseline_limit = baseline.approved_include_fragments.get(path)
        if baseline_limit is None:
            issues.append(
                f"{path} is a new approved include fragment; add a focused "
                ".c/.h module instead"
            )
        elif limit > baseline_limit:
            issues.append(
                f"{path} include-fragment ceiling raised from "
                f"{baseline_limit} to {limit}"
            )

    for source, composition in current.composed_translation_units.items():
        baseline_composition = baseline.composed_translation_units.get(source)
        if baseline_composition is None:
            issues.append(
                f"{source} is a new composed translation unit; add focused "
                ".c/.h modules instead"
            )
            continue
        if composition.max_lines > baseline_composition.max_lines:
            issues.append(
                f"{source} composed ceiling raised from "
                f"{baseline_composition.max_lines} to {composition.max_lines}"
            )
        if not _is_ordered_subset(
            composition.includes,
            baseline_composition.includes,
        ):
            issues.append(
                f"{source} include composition weakens the {baseline_label}: "
                f"expected an ordered subset of "
                f"{list(baseline_composition.includes)}, found "
                f"{list(composition.includes)}"
            )

    for source in (
        baseline.composed_translation_units.keys()
        - current.composed_translation_units.keys()
    ):
        source_path = repo_root / source
        if not source_path.is_file():
            continue
        actual = _inc_includes(source_path)
        if actual:
            issues.append(
                f"{source} removed its composed ceiling while still including "
                f"fragments {actual}"
            )

    return issues


def _header_closure_issues(
    current: _Manifest,
    baseline: _Manifest,
    current_sources: Mapping[Path, str],
    immutable_sources: Mapping[Path, str],
) -> list[str]:
    issues: list[str] = []
    for relative in sorted(current_sources):
        if relative.suffix != ".c" or not any(
            _is_within(relative, root) for root in current.source_roots
        ):
            continue
        current_closure = _header_closure_lines(
            relative,
            current_sources,
            current.source_roots,
        )
        if relative not in immutable_sources:
            if current_closure > MAX_NEW_HEADER_CLOSURE_LINES:
                issues.append(
                    f"{relative} is a new source with {current_closure} lines "
                    f"of local-header closure; limit is "
                    f"{MAX_NEW_HEADER_CLOSURE_LINES}"
                )
            continue
        baseline_closure = _header_closure_lines(
            relative,
            immutable_sources,
            baseline.source_roots,
        )
        growth = max(0, current_closure - baseline_closure)
        if growth > MAX_EXISTING_HEADER_CLOSURE_GROWTH:
            issues.append(
                f"{relative} local-header closure grew by {growth} lines "
                f"from the immutable baseline; limit is "
                f"{MAX_EXISTING_HEADER_CLOSURE_GROWTH}"
            )
    return issues


def _global_source_form_issues(
    repo_root: Path,
    current: _Manifest,
    policy_files: set[Path],
) -> list[str]:
    issues: list[str] = []
    for relative in sorted(policy_files):
        path = repo_root / relative
        if path.is_symlink() and (
            any(_is_within(relative, root) for root in current.source_roots)
            or _is_within(relative, Path("firmware/include"))
        ):
            issues.append(
                f"{relative} is a symlinked source input; architecture-owned "
                "source must be stored inside the repository"
            )
            continue
        if not path.is_file():
            continue
        if relative.suffix == ".h":
            count = _line_count(path)
            if count > MAX_HEADER_LINES:
                issues.append(
                    f"{relative} has {count} lines; header ceiling is "
                    f"{MAX_HEADER_LINES}, so implementation must move to a .c module"
                )
            issues.extend(_include_policy_issues(relative, path))
        elif relative.suffix == ".inc" and not any(
            _is_within(relative, root) for root in current.source_roots
        ):
            issues.append(
                f"{relative} is an include fragment outside declared source roots"
            )
        elif relative.suffix == ".c" and not any(
            _is_within(relative, root) for root in current.source_roots
        ) and not any(
            _is_within(relative, root) for root in NON_PRODUCTION_C_ROOTS
        ):
            issues.append(
                f"{relative} is a C source outside declared production roots "
                "and known test/sample roots"
            )
    return issues


def _source_inventory(
    repo_root: Path,
    current: _Manifest,
) -> tuple[list[str], set[Path], set[Path], dict[Path, list[Path]]]:
    issues: list[str] = []
    seen_c_files: set[Path] = set()
    seen_fragments: set[Path] = set()
    fragment_owners: dict[Path, list[Path]] = {}
    for source_root in current.source_roots:
        root = repo_root / source_root
        if not root.is_dir():
            issues.append(f"source root is missing: {source_root}")
            continue
        for suffix in sorted(UNSUPPORTED_PRODUCTION_SUFFIXES):
            for path in root.rglob(f"*{suffix}"):
                relative = path.relative_to(repo_root)
                issues.append(
                    f"{relative} is an unsupported production translation unit; "
                    "architecture policy permits only .c"
                )
        for path in root.rglob("*.c"):
            relative = path.relative_to(repo_root)
            seen_c_files.add(relative)
            issues.extend(_include_policy_issues(relative, path))
            includes = _inc_includes(path)
            if includes and relative not in current.composed_translation_units:
                issues.append(
                    f"{relative} includes fragments {includes} without a declared "
                    "composed translation-unit ceiling"
                )
            for include in includes:
                include_path = path.parent / include
                try:
                    fragment = include_path.relative_to(repo_root)
                except ValueError:
                    issues.append(
                        f"{relative} includes fragment outside the repository: "
                        f"{include}"
                    )
                    continue
                fragment_owners.setdefault(fragment, []).append(relative)
            count = _line_count(path)
            limit = current.frozen_oversize_sources.get(
                relative,
                current.default_c_max_lines,
            )
            if count > limit:
                if relative in current.frozen_oversize_sources:
                    issues.append(
                        f"{relative} grew from its frozen {limit}-line ceiling to {count}"
                    )
                else:
                    issues.append(
                        f"{relative} has {count} lines; new source ceiling is "
                        f"{current.default_c_max_lines}"
                    )
        for path in root.rglob("*.inc"):
            relative = path.relative_to(repo_root)
            seen_fragments.add(relative)
            issues.extend(_include_policy_issues(relative, path))
            nested_includes = _inc_includes(path)
            if nested_includes:
                issues.append(
                    f"{relative} includes nested fragments {nested_includes}; "
                    "include fragments may be owned only by declared C shells"
                )
            if relative not in current.approved_include_fragments:
                issues.append(
                    f"{relative} is a new include fragment; add a focused .c/.h module instead"
                )
                continue
            count = _line_count(path)
            if count > current.approved_include_fragments[relative]:
                issues.append(
                    f"{relative} grew from its frozen "
                    f"{current.approved_include_fragments[relative]}-line "
                    f"ceiling to {count}"
                )
    return issues, seen_c_files, seen_fragments, fragment_owners


def _declared_inventory_issues(
    current: _Manifest,
    seen_c_files: set[Path],
    seen_fragments: set[Path],
    fragment_owners: Mapping[Path, list[Path]],
) -> list[str]:
    issues: list[str] = []
    for relative in current.frozen_oversize_sources:
        if relative not in seen_c_files:
            issues.append(f"frozen source is missing or outside source_roots: {relative}")
    for relative in current.approved_include_fragments:
        if relative not in seen_fragments:
            issues.append(f"approved include fragment is missing: {relative}")
            continue
        owners = fragment_owners.get(relative, [])
        if len(owners) != 1:
            issues.append(
                f"approved include fragment {relative} must have exactly one "
                f"declared C owner, found {owners}"
            )
    return issues


def _composition_issues(repo_root: Path, current: _Manifest) -> list[str]:
    issues: list[str] = []
    for source, composition in current.composed_translation_units.items():
        expected = list(composition.includes)
        source_path = repo_root / source
        if not source_path.is_file():
            issues.append(f"composed translation unit is missing: {source}")
            continue
        actual = _inc_includes(source_path)
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
        if not missing and composed_lines > composition.max_lines:
            issues.append(
                f"{source} composed translation unit grew from its "
                f"{composition.max_lines}-line ceiling to {composed_lines}"
            )
    return issues


def check_repository(
    repo_root: Path = REPO_ROOT,
    manifest_path: Path = DEFAULT_MANIFEST,
    *,
    baseline_manifest: Mapping[str, Any] | None = None,
    baseline_sources: Mapping[Path, str] | None = None,
) -> list[str]:
    try:
        current = _parse_manifest(
            _load_manifest(repo_root, manifest_path),
            manifest_path.as_posix(),
        )
        baseline_value: object = (
            _load_immutable_baseline(repo_root)
            if baseline_manifest is None
            else baseline_manifest
        )
        baseline = _parse_manifest(baseline_value, "immutable baseline")
        immutable_sources = (
            _load_immutable_sources(repo_root)
            if baseline_sources is None
            else dict(baseline_sources)
        )
    except ValueError as exc:
        return [str(exc)]

    issues = _baseline_policy_issues(repo_root, current, baseline)
    policy_files = _repository_policy_files(repo_root)
    current_sources = _source_texts(repo_root, policy_files)
    issues.extend(
        _header_closure_issues(
            current,
            baseline,
            current_sources,
            immutable_sources,
        )
    )
    issues.extend(_global_source_form_issues(repo_root, current, policy_files))
    if (repo_root / "firmware/app/CMakeLists.txt").is_file():
        issues.extend(
            _app_cmake_source_issues(
                repo_root,
                current.source_roots,
                policy_files,
            )
        )
    inventory = _source_inventory(repo_root, current)
    inventory_issues, seen_c_files, seen_fragments, fragment_owners = inventory
    issues.extend(inventory_issues)
    issues.extend(
        _declared_inventory_issues(
            current,
            seen_c_files,
            seen_fragments,
            fragment_owners,
        )
    )
    issues.extend(_composition_issues(repo_root, current))

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
