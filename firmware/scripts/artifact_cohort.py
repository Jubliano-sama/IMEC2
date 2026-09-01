#!/usr/bin/env python3
"""Create immutable mesh artifact-cohort and three-role topology manifests.

The existing RTT build string is useful diagnostics, but it is not an artifact
identity: it is fixed at CMake configure time and does not include dirty source
contents.  This module gives build, flash, and capture tools one content-based
contract without requiring a commit from a deliberately dirty hardware-debug
worktree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Iterable, Mapping, Sequence

from intelhex import IntelHex


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "cohorts"
DEFAULT_TOPOLOGY_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "topologies"
DEFAULT_INVENTORY_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "inventories"
SCHEMA = 1
FLASH_SIZE = 512 * 1024
FLASH_SECTOR_SIZE = 4096
DEVICE_IDENTITY_NODE_DOMAIN = 0x494D4543414E4302
LEGACY_FIXED_CLICKER_NODE_ID = 0x1111111111111111
FIXED_GATEWAY_NODE_ID = 0x9999888877776666
# Only inputs capable of changing the firmware bytes belong in source_id.
# Deployment/capture scripts are excluded deliberately: their own hashes live
# in evidence provenance, while adding them here would invalidate an otherwise
# identical firmware cohort after a host-tool-only fix.
SOURCE_PATHS = (
    "firmware/CMakeLists.txt",
    "firmware/app",
    "firmware/cmake",
    "firmware/include",
    "firmware/src",
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_BUILD_ID_RE = re.compile(rb"imec-stack-v1:[A-Za-z0-9_.-]+:[0-9a-f]{64}")
_NINJA_DEFINE_VALUE_RE_TEMPLATE = (
    r"-D{key}=(?:\\\"|\")?([^ \r\n\"\\]+)(?:\\\"|\")?"
)
_DEPENDENCY_CACHE_KEYS = frozenset({
    "DWM3000_SDK_DIR", "NRF_DIR", "NRFXLIB_DIR", "NRFX_DIR", "ZEPHYR_BASE",
})
_TOOLCHAIN_CACHE_KEYS = (
    "CMAKE_C_COMPILER", "CMAKE_AR", "CMAKE_OBJCOPY", "CMAKE_RANLIB",
)
BENCH_TOPOLOGY_PRESETS = frozenset({
    "mesh_gateway", "mesh_anchor", "mesh_anchor_forcedhop",
})
DEPLOYMENT_IDENTITY_PRESETS = frozenset({
    "mesh_clicker", *BENCH_TOPOLOGY_PRESETS,
})
HARDWARE_MAPPED_IDENTITY_PRESETS = frozenset({
    "mesh_clicker", "mesh_anchor", "mesh_anchor_forcedhop",
})
_BOOT_IDENTITY_RE = re.compile(
    r"mesh node identity:\s*"
    r"ficr=0x([0-9a-fA-F]{16})\s+"
    r"node=0x([0-9a-fA-F]{16})\s+"
    r"preset=([A-Za-z0-9_.-]+)"
)
_U64_TEXT_RE = re.compile(r"^0x[0-9a-f]{16}$")
_U16_TEXT_RE = re.compile(r"^0x[0-9a-f]{4}$")


class CohortError(RuntimeError):
    """A cohort or artifact identity could not be established exactly."""


def _repository_venv_executable(
    repo_root: Path,
    name: str,
    *,
    platform_name: str = os.name,
) -> str:
    if platform_name == "nt":
        candidates = (
            repo_root / ".venv" / "Scripts" / f"{name}.exe",
            repo_root / ".venv" / "Scripts" / name,
            repo_root / ".venv" / "bin" / name,
        )
    else:
        candidates = (
            repo_root / ".venv" / "bin" / name,
            repo_root / ".venv" / "Scripts" / f"{name}.exe",
        )
    return str(next((path for path in candidates if path.is_file()), name))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _canonical(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
    ).encode("ascii")


def _fsync_directory(path: Path) -> None:
    if os.name == "nt":
        # CPython cannot open Windows directories for fsync. File contents are
        # flushed before linking; NTFS journals the subsequent metadata update.
        return
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _content_id(value: object) -> str:
    return hashlib.sha256(_canonical(value)).hexdigest()


def _git_discovery_environment() -> dict[str, str]:
    # Git's optional index refresh is observable to Zephyr's commit-header
    # dependencies, so a read-only source snapshot must suppress it.
    environment = os.environ.copy()
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    return environment


def _git(root: Path, *arguments: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["git", "-C", str(root), *arguments],
        capture_output=True,
        check=False,
        env=_git_discovery_environment(),
    )


def _git_head(root: Path) -> str:
    result = _git(root, "rev-parse", "HEAD")
    if result.returncode:
        return "unversioned"
    return result.stdout.decode("ascii", errors="strict").strip()


def _git_firmware_head(root: Path) -> str:
    result = _git(
        root, "log", "-1", "--format=%H", "--", *SOURCE_PATHS,
    )
    if result.returncode:
        return _git_head(root)
    head = result.stdout.decode("ascii", errors="strict").strip()
    return head or _git_head(root)


def _git_paths(root: Path, paths: Sequence[str]) -> list[str]:
    arguments = [
        "ls-files", "-z", "--cached", "--others", "--exclude-standard",
    ]
    if paths:
        arguments.extend(["--", *paths])
    result = _git(root, *arguments)
    if result.returncode:
        return []
    return sorted({
        value.decode("utf-8", errors="surrogateescape")
        for value in result.stdout.split(b"\0") if value
    })


def _file_record(root: Path, relative: str) -> dict[str, object]:
    path = root / relative
    if path.is_symlink():
        target = os.readlink(path)
        return {
            "path": relative,
            "kind": "symlink",
            "size": len(target.encode("utf-8", errors="surrogateescape")),
            "sha256": hashlib.sha256(
                target.encode("utf-8", errors="surrogateescape")
            ).hexdigest(),
        }
    if path.is_file():
        return {
            "path": relative,
            "kind": "file",
            "size": path.stat().st_size,
            "sha256": _sha256(path),
        }
    if path.is_dir():
        # A clean gitlink is completely identified by its commit. Hash only
        # dirty entries on top of that commit; walking every tracked SDK file
        # made each flash invocation needlessly re-hash an entire dependency.
        nested = _dirty_records(path)
        payload = {
            "head": _git_head(path),
            "dirty_entries": nested,
        }
        return {
            "path": relative,
            "kind": "gitlink",
            "size": len(nested),
            "sha256": _content_id(payload),
            "head": payload["head"],
        }
    return {
        "path": relative,
        "kind": "deleted",
        "size": 0,
        "sha256": hashlib.sha256(b"").hexdigest(),
    }


def _dirty_records(
    root: Path,
    paths: Sequence[str] = (),
) -> list[dict[str, object]]:
    arguments = [
        "status", "--porcelain=v1", "-z", "--untracked-files=all",
    ]
    if paths:
        arguments.extend(["--", *paths])
    status = _git(root, *arguments)
    if status.returncode:
        raise CohortError(f"cannot inspect dependency worktree: {root}")
    raw_entries = status.stdout.split(b"\0")
    records: list[dict[str, object]] = []
    index = 0
    while index < len(raw_entries):
        raw = raw_entries[index]
        index += 1
        if not raw:
            continue
        if len(raw) < 4:
            raise CohortError(f"cannot parse dependency worktree status: {root}")
        state = raw[:2]
        relative = raw[3:].decode("utf-8", errors="surrogateescape")
        if b"R" in state or b"C" in state:
            if index >= len(raw_entries) or not raw_entries[index]:
                raise CohortError(f"cannot parse dependency rename status: {root}")
            previous = raw_entries[index].decode(
                "utf-8", errors="surrogateescape",
            )
            index += 1
            records.append(_file_record(root, previous))
        records.append(_file_record(root, relative))
    return sorted(records, key=lambda item: str(item["path"]))


def _parse_cache(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise CohortError(f"build lacks CMakeCache.txt: {build_dir}")
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def _require_clean_build_graph(build_dir: Path, cache: Mapping[str, str]) -> None:
    make = cache.get("CMAKE_MAKE_PROGRAM")
    if not make:
        raise CohortError(f"build cache lacks CMAKE_MAKE_PROGRAM: {build_dir}")
    try:
        result = subprocess.run(
            [make, "-C", str(build_dir), "-n"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CohortError(f"cannot dry-run exact build graph: {build_dir}: {exc}") from exc
    output = "\n".join(
        part.strip() for part in (result.stdout, result.stderr) if part.strip()
    )
    if result.returncode or "ninja: no work to do." not in output:
        raise CohortError(
            f"exact build graph is dirty; rebuild before cohort creation: {build_dir}"
        )


def _single_ninja_define(build_dir: Path, key: str) -> str:
    build_graph = build_dir / "build.ninja"
    if not build_graph.is_file():
        raise CohortError(f"build lacks build.ninja: {build_dir}")
    values = set(re.findall(
        _NINJA_DEFINE_VALUE_RE_TEMPLATE.format(key=re.escape(key)),
        build_graph.read_text(encoding="utf-8"),
    ))
    if len(values) != 1:
        raise CohortError(
            f"build graph does not contain one unambiguous {key}: {build_dir}"
        )
    return values.pop()


def _single_embedded_build_identity(data: bytes, label: str) -> str:
    identities = {
        match.group(0).decode("ascii") for match in _BUILD_ID_RE.finditer(data)
    }
    if len(identities) != 1:
        raise CohortError(
            f"{label} does not contain one unambiguous embedded build identity"
        )
    return identities.pop()


def _require_build_source_binding(
    build_dir: Path,
    source_git_head: str,
    firmware_git_head: str,
    elf: Path,
    hex_path: Path,
) -> str:
    build_git_version = _single_ninja_define(build_dir, "IMEC_GIT_VERSION")
    build_identity = _single_ninja_define(
        build_dir, "IMEC_STACK_DIAG_BUILD_ID",
    )
    expected_git_versions = {source_git_head, firmware_git_head}
    if source_git_head == "unversioned":
        git_version_matches = build_git_version == "unknown"
    else:
        git_version_matches = (
            build_git_version != "unknown" and
            any(
                version.startswith(build_git_version)
                for version in expected_git_versions
            )
        )
    if not git_version_matches:
        raise CohortError(
            "build graph Git version "
            f"{build_git_version!r} matches neither workspace HEAD "
            f"{source_git_head!r} nor firmware-input HEAD {firmware_git_head!r}; "
            "reconfigure and rebuild before cohort creation"
        )

    elf_identity = _single_embedded_build_identity(
        elf.read_bytes(), f"build ELF {elf}",
    )
    try:
        image = IntelHex(str(hex_path))
        hex_bytes = bytes(image.tobinarray(
            start=image.minaddr(), end=image.maxaddr(),
        ))
    except Exception as exc:
        raise CohortError(
            f"artifact HEX cannot be parsed for build identity: {hex_path}: {exc}"
        ) from exc
    hex_identity = _single_embedded_build_identity(
        hex_bytes, f"build HEX {hex_path}",
    )
    if elf_identity != build_identity or hex_identity != build_identity:
        raise CohortError(
            "build graph, ELF, and HEX identities differ; rebuild before "
            f"cohort creation: {build_dir}"
        )
    return build_identity


def _dependency_records(
    repo_root: Path,
    build_dirs: Sequence[Path],
) -> list[dict[str, object]]:
    repositories: dict[Path, set[str]] = {}
    for build_dir in build_dirs:
        for key, value in _parse_cache(build_dir).items():
            if key not in _DEPENDENCY_CACHE_KEYS or not value:
                continue
            candidate = Path(value)
            if not candidate.exists():
                continue
            top = _git(candidate, "rev-parse", "--show-toplevel")
            if top.returncode:
                continue
            path = Path(top.stdout.decode("utf-8").strip()).resolve()
            repositories.setdefault(path, set()).add(key)

    records: list[dict[str, object]] = []
    for path, keys in sorted(repositories.items(), key=lambda item: str(item[0])):
        try:
            location = str(path.relative_to(repo_root.resolve()))
        except ValueError:
            # Absolute checkout paths are execution context, not source
            # content. The cache key plus repository name is stable when an
            # identical workspace is moved or cloned elsewhere.
            location = f"external/{path.name}"
        dirty_entries = _dirty_records(path)
        records.append({
            "location": location,
            "cache_keys": sorted(keys),
            "head": _git_head(path),
            "dirty": bool(dirty_entries),
            "dirty_entries": dirty_entries,
        })
    return sorted(records, key=lambda item: (str(item["location"]), item["cache_keys"]))


def _west_project_records(repo_root: Path) -> list[dict[str, object]]:
    if not (repo_root / ".west" / "config").is_file():
        return []
    executable = _repository_venv_executable(repo_root, "west")
    try:
        result = subprocess.run(
            [
                executable, "list", "-f",
                "{name}\t{path}\t{revision}",
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
            env=_git_discovery_environment(),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CohortError(f"cannot enumerate west projects: {exc}") from exc
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise CohortError(f"cannot enumerate west projects: {detail}")

    records: list[dict[str, object]] = []
    seen: set[tuple[str, str]] = set()
    for line in result.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise CohortError(f"cannot parse west project record: {line!r}")
        name, relative, revision = fields
        key = (name, relative)
        if key in seen:
            raise CohortError(f"west project is repeated: {name} at {relative}")
        seen.add(key)
        project = (repo_root / relative).resolve()
        top = _git(project, "rev-parse", "--show-toplevel")
        if top.returncode:
            raise CohortError(f"west project is not an available Git worktree: {relative}")
        actual_top = Path(
            top.stdout.decode("utf-8", errors="strict").strip()
        ).resolve()
        if actual_top == repo_root.resolve():
            # The west manifest project may be a directory inside the primary
            # repository. Its build-relevant dirty contents are already
            # represented by the explicit source file records above.
            dirty_entries = _dirty_records(actual_top, SOURCE_PATHS)
            dirty_scope = "explicit_source_files"
        elif actual_top == project:
            dirty_entries = _dirty_records(project)
            dirty_scope = "whole_project"
        else:
            raise CohortError(
                f"west project path is not its Git root: {relative}"
            )
        records.append({
            "name": name,
            "path": relative,
            "requested_revision": revision,
            "head": _git_head(actual_top),
            "dirty_scope": dirty_scope,
            "dirty_entries": dirty_entries,
        })
    return sorted(records, key=lambda item: (str(item["name"]), str(item["path"])))


def _toolchain_record(cache: Mapping[str, str]) -> dict[str, object]:
    sdk_text = cache.get("ZEPHYR_SDK_INSTALL_DIR", "")
    sdk_root = Path(sdk_text).resolve() if sdk_text else None
    tools: list[dict[str, object]] = []
    for key in _TOOLCHAIN_CACHE_KEYS:
        value = cache.get(key, "")
        if not value:
            continue
        path = Path(value)
        if not path.is_file():
            raise CohortError(f"toolchain executable is missing: {key}={path}")
        resolved = path.resolve()
        if sdk_root is not None:
            try:
                location = str(resolved.relative_to(sdk_root))
            except ValueError:
                location = f"external/{resolved.name}"
        else:
            location = f"external/{resolved.name}"
        tools.append({
            "cache_key": key,
            "location": location,
            "size": resolved.stat().st_size,
            "sha256": _sha256(resolved),
        })
    if not any(item["cache_key"] == "CMAKE_C_COMPILER" for item in tools):
        raise CohortError("build cache lacks a content-identifiable C compiler")
    sdk_files: list[dict[str, object]] = []
    if sdk_root is not None:
        for relative in (
            "sdk_version", "cmake/Zephyr-sdkConfigVersion.cmake",
        ):
            path = sdk_root / relative
            if path.is_file():
                sdk_files.append({
                    "path": relative,
                    "size": path.stat().st_size,
                    "sha256": _sha256(path),
                })
    record: dict[str, object] = {
        "variant": cache.get("ZEPHYR_TOOLCHAIN_VARIANT"),
        "sdk_name": sdk_root.name if sdk_root is not None else None,
        "sdk_files": sdk_files,
        "tools": tools,
    }
    record["toolchain_id"] = _content_id(record)
    return record


def source_snapshot(repo_root: Path, build_dirs: Sequence[Path]) -> dict[str, object]:
    root = repo_root.resolve()
    paths = _git_paths(root, SOURCE_PATHS)
    if not paths:
        # Unit-test and exported-source fallback. Production repositories are
        # Git worktrees, but refusing all non-Git use would make the helper hard
        # to validate independently.
        paths = sorted(
            str(path.relative_to(root))
            for path in root.rglob("*")
            if path.is_file() and not any(
                part in {"build", "logs", "__pycache__", ".venv", ".git"}
                for part in path.relative_to(root).parts
            )
        )
    files = [_file_record(root, relative) for relative in paths]
    payload: dict[str, object] = {
        "git_head": _git_head(root),
        "files": files,
        "west_projects": _west_project_records(root),
        "dependencies": _dependency_records(root, build_dirs),
    }
    payload["source_id"] = _content_id(payload)
    return payload


def programmed_sector_hashes(hex_path: Path) -> dict[str, str]:
    try:
        image = IntelHex(str(hex_path))
    except Exception as exc:
        raise CohortError(f"artifact HEX cannot be parsed: {hex_path}: {exc}") from exc
    addresses = image.addresses()
    if not addresses:
        raise CohortError(f"artifact HEX contains no bytes: {hex_path}")
    if min(addresses) < 0 or max(addresses) >= FLASH_SIZE:
        raise CohortError(f"artifact HEX leaves application flash: {hex_path}")
    sectors = sorted({address // FLASH_SECTOR_SIZE for address in addresses})
    content = {
        sector: bytearray(b"\xff" * FLASH_SECTOR_SIZE)
        for sector in sectors
    }
    for address in addresses:
        sector = address // FLASH_SECTOR_SIZE
        content[sector][address % FLASH_SECTOR_SIZE] = image[address]
    return {
        f"0x{sector * FLASH_SECTOR_SIZE:08x}": hashlib.sha256(content[sector]).hexdigest()
        for sector in sectors
    }


def _artifact_record(
    build_dir: Path,
    source_id: str,
    source_git_head: str,
    firmware_git_head: str,
) -> dict[str, object]:
    cache = _parse_cache(build_dir)
    _require_clean_build_graph(build_dir, cache)
    toolchain = _toolchain_record(cache)
    preset = cache.get("IMEC_BUILD_PRESET", "")
    if not preset:
        raise CohortError(f"build has no IMEC_BUILD_PRESET: {build_dir}")
    zephyr = build_dir / "zephyr"
    elf, hex_path, config = zephyr / "zephyr.elf", zephyr / "zephyr.hex", zephyr / ".config"
    for path in (elf, hex_path, config):
        if not path.is_file():
            raise CohortError(f"build artifact is missing: {path}")
    build_identity = _require_build_source_binding(
        build_dir, source_git_head, firmware_git_head, elf, hex_path,
    )
    record: dict[str, object] = {
        "preset": preset,
        "source_id": source_id,
        "legacy_build_identity": build_identity,
        "elf_sha256": _sha256(elf),
        "hex_sha256": _sha256(hex_path),
        "config_sha256": _sha256(config),
        "devicetree_sha256": (
            _sha256(zephyr / "zephyr.dts")
            if (zephyr / "zephyr.dts").is_file() else None
        ),
        "programmed_sector_sha256": programmed_sector_hashes(hex_path),
        "board": cache.get("BOARD"),
        "toolchain_id": toolchain["toolchain_id"],
        "toolchain": toolchain,
    }
    record["artifact_id"] = _content_id(record)
    return record


def create_manifest(
    repo_root: Path,
    build_dirs: Sequence[Path],
    output_directory: Path = DEFAULT_OUTPUT_DIRECTORY,
) -> Path:
    if not build_dirs:
        raise CohortError("a cohort requires at least one build directory")
    resolved = [path.resolve() for path in build_dirs]
    source = source_snapshot(repo_root, resolved)
    firmware_git_head = _git_firmware_head(repo_root.resolve())
    artifacts = sorted(
        (
            _artifact_record(
                path,
                str(source["source_id"]),
                str(source["git_head"]),
                firmware_git_head,
            )
            for path in resolved
        ),
        key=lambda item: str(item["preset"]),
    )
    presets = [str(item["preset"]) for item in artifacts]
    if len(set(presets)) != len(presets):
        raise CohortError("a cohort cannot contain two artifacts for one preset")
    payload: dict[str, object] = {
        "schema": SCHEMA,
        "source": source,
        "artifacts": artifacts,
    }
    payload["cohort_id"] = _content_id(payload)
    return _persist_content_addressed(
        payload, output_directory, "cohort", "cohort_id",
    )


def _persist_content_addressed(
    payload: Mapping[str, object],
    output_directory: Path,
    stem: str,
    identity_key: str,
) -> Path:
    identity = payload.get(identity_key)
    if not isinstance(identity, str) or _SHA256_RE.fullmatch(identity) is None:
        raise CohortError(f"{stem} content identity is invalid")
    output_directory.mkdir(parents=True, exist_ok=True)
    destination = output_directory / f"{stem}-{identity}.json"
    encoded = json.dumps(payload, indent=2, sort_keys=True).encode("utf-8") + b"\n"
    temporary = destination.with_name(
        f".{destination.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp"
    )
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    temporary_mode = 0o600 if os.name == "nt" else 0o444
    destination_created = False
    try:
        descriptor = os.open(temporary, flags, temporary_mode)
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        try:
            os.link(temporary, destination)
            destination_created = True
        except FileExistsError:
            if destination.read_bytes() != encoded:
                raise CohortError(
                    "content-addressed cohort path contains different data"
                )
        _fsync_directory(output_directory)
    finally:
        if os.name == "nt" and temporary.exists():
            os.chmod(temporary, 0o600)
        temporary.unlink(missing_ok=True)
    if os.name == "nt" and destination_created:
        os.chmod(destination, 0o444)
    return destination


def load_manifest(path: Path) -> dict[str, object]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CohortError(f"cohort manifest is unreadable: {path}") from exc
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise CohortError("cohort manifest schema is invalid")
    source = data.get("source")
    artifacts = data.get("artifacts")
    if not isinstance(source, dict) or not isinstance(artifacts, list) or not artifacts:
        raise CohortError("cohort manifest contents are invalid")
    source_payload = dict(source)
    source_id = source_payload.pop("source_id", None)
    if not isinstance(source_id, str) or source_id != _content_id(source_payload):
        raise CohortError("cohort source identity is invalid")
    presets: set[str] = set()
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise CohortError("cohort artifact record is invalid")
        preset = artifact.get("preset")
        if not isinstance(preset, str) or not preset or preset in presets:
            raise CohortError("cohort artifact preset is invalid or repeated")
        presets.add(preset)
        if artifact.get("source_id") != source_id:
            raise CohortError("cohort artifact source identity is invalid")
        record = dict(artifact)
        artifact_id = record.pop("artifact_id", None)
        if not isinstance(artifact_id, str) or artifact_id != _content_id(record):
            raise CohortError("cohort artifact identity is invalid")
        for key in ("elf_sha256", "hex_sha256", "config_sha256"):
            if _SHA256_RE.fullmatch(str(artifact.get(key, ""))) is None:
                raise CohortError(f"cohort artifact {key} is invalid")
        devicetree = artifact.get("devicetree_sha256")
        if devicetree is not None and (
            not isinstance(devicetree, str)
            or _SHA256_RE.fullmatch(devicetree) is None
        ):
            raise CohortError("cohort artifact devicetree identity is invalid")
        toolchain = artifact.get("toolchain")
        if not isinstance(toolchain, dict):
            raise CohortError("cohort artifact toolchain record is invalid")
        toolchain_payload = dict(toolchain)
        nested_toolchain_id = toolchain_payload.pop("toolchain_id", None)
        if (
            not isinstance(nested_toolchain_id, str)
            or nested_toolchain_id != _content_id(toolchain_payload)
            or artifact.get("toolchain_id") != nested_toolchain_id
        ):
            raise CohortError("cohort artifact toolchain identity is invalid")
        sectors = artifact.get("programmed_sector_sha256")
        if not isinstance(sectors, dict) or not sectors:
            raise CohortError("cohort artifact programmed-sector identity is invalid")
        for address_text, sector_sha256 in sectors.items():
            try:
                address = int(str(address_text), 16)
            except ValueError as exc:
                raise CohortError(
                    "cohort artifact sector address is invalid"
                ) from exc
            if (
                not isinstance(address_text, str)
                or address < 0
                or address % FLASH_SECTOR_SIZE
                or address + FLASH_SECTOR_SIZE > FLASH_SIZE
                or not isinstance(sector_sha256, str)
                or _SHA256_RE.fullmatch(sector_sha256) is None
            ):
                raise CohortError(
                    "cohort artifact programmed-sector identity is invalid"
                )
    payload = dict(data)
    cohort_id = payload.pop("cohort_id", None)
    if not isinstance(cohort_id, str) or cohort_id != _content_id(payload):
        raise CohortError("cohort content identity is invalid")
    return data


def artifact_for_preset(data: Mapping[str, object], preset: str) -> dict[str, object]:
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list):
        raise CohortError("cohort artifact list is invalid")
    matches = [item for item in artifacts if isinstance(item, dict) and item.get("preset") == preset]
    if len(matches) != 1:
        raise CohortError(f"cohort does not contain exactly one {preset} artifact")
    return matches[0]


def validate_build(path: Path, repo_root: Path, build_dir: Path) -> dict[str, object]:
    data = load_manifest(path)
    source = source_snapshot(repo_root, [build_dir.resolve()])
    recorded_source = data["source"]
    assert isinstance(recorded_source, dict)
    if source.get("source_id") != recorded_source.get("source_id"):
        raise CohortError("workspace source snapshot differs from cohort")
    cache = _parse_cache(build_dir)
    preset = cache.get("IMEC_BUILD_PRESET", "")
    expected = artifact_for_preset(data, preset)
    firmware_git_head = _git_firmware_head(repo_root.resolve())
    current = _artifact_record(
        build_dir.resolve(),
        str(source["source_id"]),
        str(source["git_head"]),
        firmware_git_head,
    )
    if current != expected:
        raise CohortError(f"{preset} build artifacts or configuration differ from cohort")
    return {
        "manifest_path": str(path.resolve()),
        "cohort_id": str(data["cohort_id"]),
        "source_id": str(source["source_id"]),
        "artifact_id": str(current["artifact_id"]),
        "artifact": current,
    }


def _effective_uwb_short_addr(node_id: int) -> int:
    short_addr = node_id & 0xffff
    return short_addr if short_addr != 0 else 1


def _identity_record(preset: str, physical_id: int, node_id: int) -> dict[str, str]:
    if preset not in DEPLOYMENT_IDENTITY_PRESETS:
        raise CohortError(f"{preset!r} has no production identity contract")
    if physical_id in {0, (1 << 64) - 1}:
        raise CohortError(f"target-reported physical identity is invalid for {preset}")
    if node_id in {0, (1 << 64) - 1}:
        raise CohortError(f"target-reported node identity is invalid for {preset}")

    if preset in HARDWARE_MAPPED_IDENTITY_PRESETS:
        if node_id != physical_id ^ DEVICE_IDENTITY_NODE_DOMAIN:
            raise CohortError(
                f"target-reported FICR/node mapping is invalid for {preset}"
            )
        if node_id in {
            LEGACY_FIXED_CLICKER_NODE_ID, FIXED_GATEWAY_NODE_ID,
        }:
            raise CohortError(
                f"target-reported mapped node identity is reserved for {preset}"
            )
        source = "nrf_ficr_mapped"
    else:
        if node_id != FIXED_GATEWAY_NODE_ID:
            raise CohortError("target-reported fixed gateway identity is invalid")
        source = "fixed_gateway"

    return {
        "identity_source": source,
        "physical_id": f"0x{physical_id:016x}",
        "node_id": f"0x{node_id:016x}",
        "effective_uwb_short_addr": (
            f"0x{_effective_uwb_short_addr(node_id):04x}"
        ),
    }


def target_identity_from_transcript(log: str, preset: str) -> dict[str, str]:
    """Extract and validate one stable target identity from a pre-reset RTT log."""
    matches = {
        (int(physical, 16), int(node, 16), reported_preset)
        for physical, node, reported_preset in _BOOT_IDENTITY_RE.findall(log)
    }
    if not matches:
        raise CohortError(
            f"RTT transcript lacks the target identity record for {preset}"
        )
    if len(matches) != 1:
        raise CohortError(
            f"RTT transcript contains conflicting target identity records for {preset}"
        )
    physical_id, node_id, reported_preset = next(iter(matches))
    if reported_preset != preset:
        raise CohortError(
            f"target identity preset {reported_preset!r} differs from {preset!r}"
        )
    return _identity_record(preset, physical_id, node_id)


def _validated_capture_identity(
    capture: Mapping[str, object], capture_path: Path,
) -> dict[str, object]:
    preset = capture.get("preset")
    probe_id = capture.get("probe_id")
    target = capture.get("target")
    if (
        not isinstance(preset, str)
        or preset not in DEPLOYMENT_IDENTITY_PRESETS
        or not isinstance(probe_id, str)
        or not probe_id
        or not isinstance(target, dict)
    ):
        raise CohortError(f"deployment capture identity is invalid: {capture_path}")
    expected_binding = capture_binding_id(capture)
    if capture.get("cohort_capture_id") != expected_binding:
        raise CohortError(
            f"deployment capture binding identity is invalid for {preset}"
        )
    physical_text = target.get("physical_id")
    node_text = target.get("node_id")
    if (
        not isinstance(physical_text, str)
        or _U64_TEXT_RE.fullmatch(physical_text) is None
        or not isinstance(node_text, str)
        or _U64_TEXT_RE.fullmatch(node_text) is None
    ):
        raise CohortError(f"deployment capture lacks a canonical identity for {preset}")
    identity = _identity_record(
        preset, int(physical_text, 16), int(node_text, 16),
    )
    for key, expected in identity.items():
        if target.get(key) != expected:
            raise CohortError(
                f"deployment capture {key} is invalid for {preset}"
            )
    effective = target.get("effective_uwb_short_addr")
    if not isinstance(effective, str) or _U16_TEXT_RE.fullmatch(effective) is None:
        raise CohortError(
            f"deployment capture effective UWB short address is invalid for {preset}"
        )
    return {
        "preset": preset,
        "probe_id": probe_id,
        "capture_id": str(capture.get("capture_id", "")),
        "capture_manifest_path": str(capture_path.resolve()),
        "capture_manifest_sha256": _sha256(capture_path),
        **identity,
    }


def validate_deployment_inventory(
    capture_paths: Sequence[Path],
) -> dict[str, object]:
    """Validate per-device identities, including multiple instances of one role."""
    if not capture_paths:
        raise CohortError("deployment inventory requires at least one capture")
    instances: list[dict[str, object]] = []
    probes: dict[str, dict[str, object]] = {}
    physical_ids: dict[str, dict[str, object]] = {}
    node_ids: dict[str, dict[str, object]] = {}
    short_addresses: dict[str, dict[str, object]] = {}

    for path in capture_paths:
        try:
            capture = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise CohortError(f"deployment capture is unreadable: {path}") from exc
        if not isinstance(capture, dict):
            raise CohortError(f"deployment capture is invalid: {path}")
        instance = _validated_capture_identity(capture, path)
        preset = str(instance["preset"])
        for label, key, seen in (
            ("probe", "probe_id", probes),
            ("physical identity", "physical_id", physical_ids),
            ("node identity", "node_id", node_ids),
            ("effective UWB short address", "effective_uwb_short_addr", short_addresses),
        ):
            value = str(instance[key])
            previous = seen.get(value)
            if previous is not None:
                raise CohortError(
                    f"deployment inventory repeats {label} {value}: "
                    f"{previous['preset']} and {preset}"
                )
            seen[value] = instance
        instances.append(instance)

    gateway_count = sum(
        instance["preset"] == "mesh_gateway" for instance in instances
    )
    if gateway_count != 1:
        raise CohortError(
            f"deployment inventory requires exactly one mesh_gateway, got {gateway_count}"
        )
    result: dict[str, object] = {
        "schema": 1,
        "instances": sorted(
            instances,
            key=lambda item: (
                str(item["preset"]), str(item["physical_id"]),
                str(item["probe_id"]),
            ),
        ),
    }
    result["inventory_id"] = _content_id(result)
    return result


def create_deployment_inventory_manifest(
    capture_paths: Sequence[Path],
    output_directory: Path = DEFAULT_INVENTORY_DIRECTORY,
) -> Path:
    inventory = validate_deployment_inventory(capture_paths)
    return _persist_content_addressed(
        inventory, output_directory, "inventory", "inventory_id",
    )


def validate_topology(
    bindings: Sequence[tuple[str, str, Path]],
    required_presets: Iterable[str],
) -> dict[str, object]:
    """Validate capture/probe bindings for one three-role source cohort."""
    required = set(required_presets)
    if required != BENCH_TOPOLOGY_PRESETS:
        raise CohortError(
            "bench topology requires exactly mesh_gateway, mesh_anchor, and "
            "mesh_anchor_forcedhop"
        )
    inventory = validate_deployment_inventory(
        [capture_path for _preset, _probe_id, capture_path in bindings]
    )
    roles: dict[str, dict[str, object]] = {}
    source_ids: set[str] = set()
    toolchain_ids: set[str] = set()
    probes: set[str] = set()
    for preset, probe_id, capture_path in bindings:
        try:
            capture = json.loads(capture_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise CohortError(f"topology capture is unreadable: {capture_path}") from exc
        if not isinstance(capture, dict):
            raise CohortError(f"topology capture is invalid: {capture_path}")
        if capture.get("preset") != preset or capture.get("probe_id") != probe_id:
            raise CohortError(
                f"topology capture role/probe differs for {preset}"
            )
        expected_mode = (
            "bench_only" if preset == "mesh_anchor_forcedhop"
            else "production_candidate"
        )
        expected_promotion = expected_mode != "bench_only"
        if (
            capture.get("evidence_mode") != expected_mode
            or capture.get("promotion_allowed") is not expected_promotion
        ):
            raise CohortError(
                f"topology capture evidence mode is invalid for {preset}"
            )
        expected_binding = capture_binding_id(capture)
        if capture.get("cohort_capture_id") != expected_binding:
            raise CohortError(
                f"topology capture binding identity is invalid for {preset}"
            )
        binding = capture.get("cohort")
        target = capture.get("target")
        capture_artifact = capture.get("artifact")
        assert isinstance(binding, dict)
        assert isinstance(target, dict)
        assert isinstance(capture_artifact, dict)
        cohort_path = Path(str(binding["manifest_path"]))
        data = load_manifest(cohort_path)
        artifact = artifact_for_preset(data, preset)
        source = data["source"]
        assert isinstance(source, dict)
        for key, actual in (
            ("cohort_id", data["cohort_id"]),
            ("source_id", source["source_id"]),
            ("artifact_id", artifact["artifact_id"]),
        ):
            if binding.get(key) != actual:
                raise CohortError(
                    f"topology capture {key} differs from its cohort for {preset}"
                )
        if capture_artifact.get("artifact_id") != artifact.get("artifact_id"):
            raise CohortError(
                f"topology capture artifact differs from its cohort for {preset}"
            )
        sectors = artifact.get("programmed_sector_sha256")
        if (
            not isinstance(sectors, dict)
            or target.get("code_sector_map_sha256") != _content_id(sectors)
        ):
            raise CohortError(
                f"topology capture target readback differs from its cohort for {preset}"
            )
        if preset in roles:
            raise CohortError(f"topology repeats preset {preset}")
        if probe_id in probes:
            raise CohortError(f"topology repeats probe {probe_id}")
        probes.add(probe_id)
        source_ids.add(str(source["source_id"]))
        toolchain_id = artifact.get("toolchain_id")
        if (
            not isinstance(toolchain_id, str)
            or _SHA256_RE.fullmatch(toolchain_id) is None
        ):
            raise CohortError(
                f"topology cohort lacks a toolchain identity for {preset}"
            )
        toolchain_ids.add(toolchain_id)
        roles[preset] = {
            "probe_id": probe_id,
            "capture_id": str(capture["capture_id"]),
            "capture_manifest_path": str(capture_path.resolve()),
            "capture_manifest_sha256": _sha256(capture_path),
            "target_code_sector_map_sha256": str(
                target["code_sector_map_sha256"]
            ),
            "artifact_id": str(artifact["artifact_id"]),
            "cohort_id": str(data["cohort_id"]),
            "physical_id": str(target["physical_id"]),
            "node_id": str(target["node_id"]),
            "effective_uwb_short_addr": str(
                target["effective_uwb_short_addr"]
            ),
        }
    if set(roles) != required:
        raise CohortError(
            f"topology presets differ: expected {sorted(required)}, got {sorted(roles)}"
        )
    if len(source_ids) != 1:
        raise CohortError("topology artifacts do not share one source snapshot")
    if len(toolchain_ids) != 1:
        raise CohortError("topology artifacts do not share one toolchain identity")
    result: dict[str, object] = {
        "schema": 1,
        "source_id": next(iter(source_ids)),
        "toolchain_id": next(iter(toolchain_ids)),
        "inventory_id": inventory["inventory_id"],
        "roles": dict(sorted(roles.items())),
    }
    result["topology_id"] = _content_id(result)
    return result


def create_topology_manifest(
    bindings: Sequence[tuple[str, str, Path]],
    required_presets: Iterable[str] = BENCH_TOPOLOGY_PRESETS,
    output_directory: Path = DEFAULT_TOPOLOGY_DIRECTORY,
) -> Path:
    topology = validate_topology(bindings, required_presets)
    return _persist_content_addressed(
        topology, output_directory, "topology", "topology_id",
    )


def target_readback_identity(readback: Path, artifact: Mapping[str, object]) -> dict[str, str]:
    content = readback.read_bytes()
    if len(content) != FLASH_SIZE:
        raise CohortError(
            f"target readback has {len(content)} bytes, expected {FLASH_SIZE}"
        )
    sectors = artifact.get("programmed_sector_sha256")
    if not isinstance(sectors, dict) or not sectors:
        raise CohortError("artifact lacks programmed-sector identity")
    for address_text, expected in sectors.items():
        try:
            address = int(str(address_text), 16)
        except ValueError as exc:
            raise CohortError("artifact sector address is invalid") from exc
        if (
            address < 0
            or address % FLASH_SECTOR_SIZE
            or address + FLASH_SECTOR_SIZE > FLASH_SIZE
            or not isinstance(expected, str)
            or _SHA256_RE.fullmatch(expected) is None
        ):
            raise CohortError("artifact sector identity is invalid")
        actual = hashlib.sha256(content[address:address + FLASH_SECTOR_SIZE]).hexdigest()
        if actual != expected:
            raise CohortError(
                f"target code sector {address_text} differs from cohort artifact"
            )
    return {
        "flash_sha256": hashlib.sha256(content).hexdigest(),
        "code_sector_map_sha256": _content_id(sectors),
    }


def capture_binding_id(capture: Mapping[str, object]) -> str:
    artifact = capture.get("artifact")
    target = capture.get("target")
    binding = capture.get("cohort")
    if not all(isinstance(item, dict) for item in (artifact, target, binding)):
        raise CohortError("capture lacks artifact, target, or cohort binding")
    assert isinstance(artifact, dict)
    assert isinstance(target, dict)
    assert isinstance(binding, dict)
    for value in (
        capture.get("capture_id"),
        artifact.get("elf_sha256"), artifact.get("hex_sha256"),
        artifact.get("artifact_id"), target.get("flash_sha256"),
        target.get("pre_capture_flash_sha256"),
        target.get("post_capture_flash_sha256"),
        target.get("code_sector_map_sha256"), binding.get("cohort_id"),
        binding.get("source_id"), binding.get("artifact_id"),
    ):
        if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
            raise CohortError("capture content binding contains an invalid identity")
    for key in (
        "pre_readback_started_at_utc", "pre_readback_completed_at_utc",
        "post_readback_started_at_utc", "post_readback_completed_at_utc",
    ):
        if not isinstance(target.get(key), str) or not target.get(key):
            raise CohortError(f"capture target lacks {key}")
    if not isinstance(binding.get("manifest_path"), str) or not binding.get("manifest_path"):
        raise CohortError("capture cohort manifest path is invalid")
    payload = {
        "capture_id": capture["capture_id"],
        "preset": capture.get("preset"),
        "probe_id": capture.get("probe_id"),
        "evidence_mode": capture.get("evidence_mode"),
        "promotion_allowed": capture.get("promotion_allowed"),
        "artifact": artifact,
        "target": target,
        "cohort": binding,
    }
    return _content_id(payload)


def _topology_binding(value: str) -> tuple[str, str, Path]:
    fields = value.split("=", 2)
    if len(fields) != 3 or not all(fields):
        raise argparse.ArgumentTypeError(
            "topology binding must be PRESET=PROBE_ID=CAPTURE_MANIFEST"
        )
    return fields[0], fields[1], Path(fields[2])


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--build-dir", type=Path, action="append")
    parser.add_argument(
        "--topology-binding", type=_topology_binding, action="append",
        metavar="PRESET=PROBE_ID=CAPTURE_MANIFEST",
    )
    parser.add_argument(
        "--inventory-capture", type=Path, action="append",
        metavar="CAPTURE_MANIFEST",
    )
    args = parser.parse_args(argv)
    selected_modes = sum(bool(value) for value in (
        args.build_dir, args.topology_binding, args.inventory_capture,
    ))
    if selected_modes != 1:
        parser.error(
            "select exactly one mode: --build-dir, --topology-binding, or "
            "--inventory-capture"
        )
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.topology_binding:
            path = create_topology_manifest(
                args.topology_binding,
                BENCH_TOPOLOGY_PRESETS,
                args.output_dir or DEFAULT_TOPOLOGY_DIRECTORY,
            )
        elif args.inventory_capture:
            path = create_deployment_inventory_manifest(
                args.inventory_capture,
                args.output_dir or DEFAULT_INVENTORY_DIRECTORY,
            )
        else:
            path = create_manifest(
                args.repo_root,
                args.build_dir,
                args.output_dir or DEFAULT_OUTPUT_DIRECTORY,
            )
    except (OSError, CohortError) as exc:
        print(f"artifact evidence creation blocked: {exc}", file=sys.stderr)
        return 1
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
