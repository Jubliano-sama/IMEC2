#!/usr/bin/env python3
"""Freeze and validate source inputs consumed by repository verification."""

from __future__ import annotations

import configparser
import ctypes
import errno
import hashlib
import json
import os
import posixpath
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Sequence


DWM_SUBMODULE = Path("dwm3000 examples and sdk")
MANIFEST_REV = "refs/heads/manifest-rev^{commit}"
WEST_LOCK_SCHEMA = 1
PINNED_WEST_CONFIG = {
    "manifest": {
        "path": "manifest",
        "file": "west.yml",
    },
    "zephyr": {
        "base": "zephyr",
    },
}
WEST_MANIFEST_RELATIVE = Path(
    PINNED_WEST_CONFIG["manifest"]["path"]
) / PINNED_WEST_CONFIG["manifest"]["file"]
WEST_METADATA_MAX_BYTES = 1024 * 1024
_SAFE_FILE_READ_CHUNK_BYTES = 64 * 1024
_AMBIENT_BUILD_OVERRIDE_NAMES = frozenset(
    {
        "ARCH_ROOT",
        "APPLICATION_ROOT",
        "AR",
        "AS",
        "BOARD",
        "BOARD_REVISION",
        "BOARD_ROOT",
        "CC",
        "CFLAGS",
        "CPLUS_INCLUDE_PATH",
        "CPATH",
        "CONF_FILE",
        "COMPILER_PATH",
        "CPPFLAGS",
        "CROSS_COMPILE",
        "C_INCLUDE_PATH",
        "CXX",
        "CXXFLAGS",
        "DTC_OVERLAY_FILE",
        "DTS_ROOT",
        "EXTRA_CONF_FILE",
        "EXTRA_CFLAGS",
        "EXTRA_CPPFLAGS",
        "EXTRA_CXXFLAGS",
        "EXTRA_DTC_OVERLAY_FILE",
        "EXTRA_LDFLAGS",
        "EXTRA_ZEPHYR_MODULES",
        "GCC_EXEC_PREFIX",
        "KCONFIG_ALLCONFIG",
        "KCONFIG_CONFIG",
        "KCONFIG_OVERWRITECONFIG",
        "LD",
        "LD_LIBRARY_PATH",
        "LDFLAGS",
        "LIBRARY_PATH",
        "MODULE_EXT_ROOT",
        "NCS_DIR",
        "NCS_ROOT",
        "OBJCOPY",
        "OBJC_INCLUDE_PATH",
        "PKG_CONFIG_LIBDIR",
        "PKG_CONFIG_PATH",
        "PKG_CONFIG_SYSROOT_DIR",
        "SCA_ROOT",
        "SHIELD",
        "SNIPPET",
        "SOC_ROOT",
        "STRIP",
        "TOOLCHAIN_ROOT",
        "ZEPHYR_DIR",
        "Zephyr-sdk_DIR",
        "Zephyr-sdk_ROOT",
        "Zephyr_DIR",
        "Zephyr_ROOT",
    }
)
_AMBIENT_BUILD_OVERRIDE_PREFIXES = (
    "CMAKE_",
    "WEST_",
    "ZEPHYR_",
)


def verification_environment(
    base: dict[str, str] | None = None,
) -> dict[str, str]:
    """Return an environment which cannot write Python or Git read caches."""

    environment = os.environ.copy() if base is None else base.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["PYTHONPYCACHEPREFIX"] = os.devnull
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    return environment


def reject_ambient_build_overrides(
    base: dict[str, str] | None = None,
) -> None:
    """Reject environment variables which can alter CMake or Zephyr inputs."""

    environment = os.environ if base is None else base
    overrides = sorted(
        name
        for name in environment
        if name in _AMBIENT_BUILD_OVERRIDE_NAMES
        or name.startswith(_AMBIENT_BUILD_OVERRIDE_PREFIXES)
        or name.endswith("_TOOLCHAIN_PATH")
        or name.endswith("_SDK_INSTALL_DIR")
    )
    if overrides:
        raise RuntimeError(
            "ambient build configuration overrides are forbidden; remove "
            "these variables and rerun: "
            + ", ".join(overrides)
        )


def pinned_west_environment(
    workspace_root: Path,
    base: dict[str, str] | None = None,
) -> dict[str, str]:
    """Return a fail-closed west/Zephyr environment pinned to one workspace."""

    source = os.environ if base is None else base
    reject_ambient_build_overrides(source)
    environment = source.copy()
    environment.update(
        {
            "ZEPHYR_BASE": os.fspath(workspace_root / "zephyr"),
            "WEST_CONFIG_LOCAL": os.fspath(
                workspace_root / ".west/config"
            ),
            "WEST_CONFIG_GLOBAL": os.devnull,
            "WEST_CONFIG_SYSTEM": os.devnull,
        }
    )
    return verification_environment(environment)


def _run_capture(
    command: Sequence[str],
    *,
    cwd: Path,
    input_bytes: bytes | None = None,
    env: dict[str, str] | None = None,
) -> bytes:
    result = subprocess.run(
        list(command),
        cwd=cwd,
        env=verification_environment(env),
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        if not detail:
            detail = result.stdout.decode("utf-8", errors="replace").strip()
        if not detail:
            detail = f"exit {result.returncode}"
        rendered = " ".join(command)
        raise RuntimeError(f"command failed in {cwd}: {rendered}: {detail}")
    return result.stdout


def git_bytes(repo_root: Path, arguments: Sequence[str]) -> bytes:
    """Run a read-only Git query without allowing optional index refreshes."""

    try:
        return _run_capture(["git", *arguments], cwd=repo_root)
    except RuntimeError as exc:
        raise RuntimeError(
            f"cannot inspect Git repository {repo_root}: {exc}"
        ) from exc


def repository_fingerprint(repo_root: Path) -> str:
    """Hash HEAD, the complete tracked diff, and nonignored untracked inputs."""

    digest = hashlib.sha256()
    head = git_bytes(repo_root, ["rev-parse", "--verify", "HEAD"]).strip()
    diff = git_bytes(
        repo_root,
        [
            "diff",
            "--binary",
            "--full-index",
            "--no-ext-diff",
            "--no-textconv",
            "--ignore-submodules=none",
            "HEAD",
            "--",
        ],
    )
    untracked = git_bytes(
        repo_root,
        ["ls-files", "--others", "--exclude-standard", "-z"],
    )
    digest.update(b"head\0")
    digest.update(head)
    digest.update(b"\0diff\0")
    digest.update(diff)
    for raw_path in sorted(path for path in untracked.split(b"\0") if path):
        relative = _safe_relative_path(raw_path)
        path = repo_root / relative
        digest.update(b"\0untracked\0")
        digest.update(raw_path)
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            digest.update(b"\0missing")
            continue
        digest.update(f"\0mode={metadata.st_mode:o}\0".encode())
        if path.is_symlink():
            digest.update(os.fsencode(os.readlink(path)))
        elif path.is_file():
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
        else:
            digest.update(b"non-file")
    return f"{head.decode('ascii', errors='replace')}:{digest.hexdigest()}"


def _safe_relative_path(raw_path: bytes) -> Path:
    relative = Path(os.fsdecode(raw_path))
    if relative.is_absolute() or ".." in relative.parts:
        raise RuntimeError(f"Git returned unsafe path: {relative}")
    return relative


def _status_entries(status: bytes) -> list[str]:
    return [
        entry.decode("utf-8", errors="replace")
        for entry in status.split(b"\0")
        if entry
    ]


def _preview(entries: Sequence[str]) -> str:
    preview = ", ".join(entries[:8])
    if len(entries) > 8:
        preview += f", ... ({len(entries) - 8} more)"
    return preview


def require_clean_build_inputs(
    repo_root: Path,
    build_inputs: Sequence[str],
) -> None:
    """Reject tracked or nonignored-untracked changes in exact build inputs."""

    tracked = git_bytes(
        repo_root,
        [
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=no",
            "--",
            *build_inputs,
        ],
    )
    untracked = git_bytes(
        repo_root,
        [
            "ls-files",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            *build_inputs,
        ],
    )
    entries = _status_entries(tracked + untracked)
    if entries:
        raise RuntimeError(
            "exact-role and compatibility artifacts require tracked-clean app "
            "sources with no untracked build inputs so their embedded Git "
            f"identity is truthful; outstanding paths: {_preview(entries)}"
        )


def _require_clean_local_submodule(repo_root: Path, relative: Path) -> None:
    submodule = repo_root / relative
    if not submodule.is_dir():
        raise RuntimeError(
            f"required local DWM submodule checkout is missing: {submodule}"
        )
    try:
        git_bytes(submodule, ["rev-parse", "--verify", "HEAD"])
        status = git_bytes(
            submodule,
            [
                "status",
                "--porcelain=v1",
                "-z",
                "--untracked-files=all",
                "--ignore-submodules=none",
            ],
        )
    except RuntimeError as exc:
        raise RuntimeError(
            f"required local DWM submodule is not a usable Git checkout: {submodule}"
        ) from exc
    entries = _status_entries(status)
    if entries:
        raise RuntimeError(
            "the local DWM submodule must be clean before it can seed an "
            f"immutable snapshot; outstanding paths: {_preview(entries)}"
        )


def _copy_untracked_files(source: Path, destination: Path) -> None:
    untracked = git_bytes(
        source,
        ["ls-files", "--others", "--exclude-standard", "-z"],
    )
    for raw_path in sorted(path for path in untracked.split(b"\0") if path):
        relative = _safe_relative_path(raw_path)
        source_path = source / relative
        destination_path = destination / relative
        try:
            metadata = source_path.lstat()
        except FileNotFoundError as exc:
            raise RuntimeError(
                f"untracked input disappeared while freezing it: {relative}"
            ) from exc
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        if source_path.is_symlink():
            destination_path.symlink_to(os.readlink(source_path))
        elif source_path.is_file():
            shutil.copy2(source_path, destination_path, follow_symlinks=False)
        else:
            raise RuntimeError(
                "verification cannot freeze non-file untracked input "
                f"{relative} (mode {metadata.st_mode:o})"
            )


def _require_self_contained_symlinks(snapshot: Path) -> None:
    for directory, names, files in os.walk(snapshot, followlinks=False):
        names[:] = [name for name in names if name != ".git"]
        base = Path(directory)
        for name in [*names, *files]:
            link = base / name
            if not link.is_symlink():
                continue
            relative = link.relative_to(snapshot)
            target = Path(os.readlink(link))
            normalized = Path(
                posixpath.normpath((relative.parent / target).as_posix())
            )
            if target.is_absolute() or normalized.is_absolute() or ".." in normalized.parts:
                raise RuntimeError(
                    f"source symlink escapes immutable snapshot: "
                    f"{relative} -> {target}"
                )


def _snapshot_gitlink(snapshot: Path, relative: Path) -> str:
    output = git_bytes(
        snapshot,
        ["ls-files", "--stage", "-z", "--", os.fspath(relative)],
    )
    entries = [entry for entry in output.split(b"\0") if entry]
    if len(entries) != 1:
        raise RuntimeError(
            f"snapshot does not contain exactly one DWM gitlink: {relative}"
        )
    metadata, separator, _path = entries[0].partition(b"\t")
    if not separator:
        raise RuntimeError(f"cannot parse DWM gitlink metadata for {relative}")
    fields = metadata.split()
    if len(fields) != 3 or fields[0] != b"160000":
        raise RuntimeError(f"snapshot DWM path is not a Git submodule: {relative}")
    return fields[1].decode("ascii")


def _initialize_local_submodule(
    snapshot: Path,
    source_repo: Path,
    relative: Path,
) -> None:
    desired_sha = _snapshot_gitlink(snapshot, relative)
    git_bytes(source_repo, ["cat-file", "-e", f"{desired_sha}^{{commit}}"])
    submodule_name = git_bytes(
        snapshot,
        [
            "config",
            "-z",
            "-f",
            ".gitmodules",
            "--get-regexp",
            r"^submodule\..*\.path$",
        ],
    )
    matching_names: list[str] = []
    for record in (item for item in submodule_name.split(b"\0") if item):
        raw_key, separator, raw_value = record.partition(b"\n")
        key = raw_key.decode("utf-8", errors="replace")
        value = raw_value.decode("utf-8", errors="replace")
        if separator and value == os.fspath(relative):
            matching_names.append(key.removesuffix(".path").removeprefix("submodule."))
    if len(matching_names) != 1:
        raise RuntimeError(
            f"cannot identify DWM submodule metadata for {relative}"
        )
    name = matching_names[0]
    _run_capture(
        [
            "git",
            "config",
            f"submodule.{name}.url",
            os.fspath(source_repo.resolve()),
        ],
        cwd=snapshot,
    )
    _run_capture(
        [
            "git",
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "update",
            "--init",
            "--no-fetch",
            "--checkout",
            "--",
            os.fspath(relative),
        ],
        cwd=snapshot,
    )
    actual_sha = git_bytes(
        snapshot / relative,
        ["rev-parse", "--verify", "HEAD"],
    ).decode("ascii").strip()
    if actual_sha != desired_sha:
        raise RuntimeError(
            f"snapshot DWM submodule is {actual_sha}, expected {desired_sha}"
        )


@dataclass(frozen=True)
class SourceSnapshot:
    root: Path
    fingerprint: str
    write_guard: LinuxInotifyWriteGuard


@contextmanager
def frozen_source_snapshot(
    repo_root: Path,
    *,
    expected_fingerprint: str | None = None,
    submodule_path: Path = DWM_SUBMODULE,
) -> Iterator[SourceSnapshot]:
    """Yield a local, full-history snapshot of one exact invocation state."""

    repo_root = repo_root.resolve()
    _require_clean_local_submodule(repo_root, submodule_path)
    captured = expected_fingerprint or repository_fingerprint(repo_root)
    head = git_bytes(repo_root, ["rev-parse", "--verify", "HEAD"]).decode(
        "ascii"
    ).strip()
    tracked_diff = git_bytes(
        repo_root,
        [
            "diff",
            "--binary",
            "--full-index",
            "--no-ext-diff",
            "--no-textconv",
            "--ignore-submodules=none",
            "HEAD",
            "--",
        ],
    )

    with tempfile.TemporaryDirectory(prefix="imec2-source-snapshot-") as temporary:
        snapshot = Path(temporary) / "repository"
        _run_capture(
            [
                "git",
                "clone",
                "--quiet",
                "--local",
                "--no-hardlinks",
                "--no-checkout",
                os.fspath(repo_root),
                os.fspath(snapshot),
            ],
            cwd=Path(temporary),
        )
        _run_capture(
            ["git", "checkout", "--quiet", "--detach", head],
            cwd=snapshot,
        )
        if tracked_diff:
            _run_capture(
                [
                    "git",
                    "apply",
                    "--binary",
                    "--index",
                    "--whitespace=nowarn",
                    "-",
                ],
                cwd=snapshot,
                input_bytes=tracked_diff,
            )
        _copy_untracked_files(repo_root, snapshot)
        _initialize_local_submodule(
            snapshot,
            repo_root / submodule_path,
            submodule_path,
        )
        _require_self_contained_symlinks(snapshot)

        snapshot_fingerprint = repository_fingerprint(snapshot)
        current_fingerprint = repository_fingerprint(repo_root)
        if snapshot_fingerprint != captured or current_fingerprint != captured:
            raise RuntimeError(
                "repository changed while its immutable verification snapshot "
                "was being captured; discard the snapshot and retry"
            )
        guard = LinuxInotifyWriteGuard(
            [snapshot],
            subject="immutable source snapshot",
            exclude_git=False,
        )
        try:
            guard.assert_stable("after guard installation")
            yield SourceSnapshot(snapshot, captured, guard)
        finally:
            try:
                guard.assert_stable("snapshot completion")
            finally:
                guard.close()


@dataclass(frozen=True)
class WestWorkspace:
    executable: str
    root: Path


@dataclass(frozen=True)
class WestProject:
    name: str
    path: Path
    revision: str
    expected_sha: str


class VerificationMatrixFailure(RuntimeError):
    """Preserve a matrix failure together with every cleanup failure."""

    def __init__(
        self,
        primary_error: BaseException | None,
        cleanup_errors: Sequence[Exception],
    ):
        self.primary_error = primary_error
        self.cleanup_errors = tuple(cleanup_errors)
        cleanup_detail = "; ".join(
            f"{type(error).__name__}: {error}"
            for error in self.cleanup_errors
        )
        if primary_error is None:
            message = (
                "west dependency cleanup validation failed: "
                + cleanup_detail
            )
        else:
            message = (
                "exact build matrix failed with "
                f"{type(primary_error).__name__}: {primary_error}; "
                "west dependency cleanup also failed: "
                + cleanup_detail
            )
        super().__init__(message)


def _load_west_lock(lock_path: Path) -> dict[str, tuple[Path, str, str]]:
    try:
        value = json.loads(lock_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise RuntimeError(f"west project lock is missing: {lock_path}") from None
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"west project lock is invalid JSON: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema") != WEST_LOCK_SCHEMA:
        raise RuntimeError(
            f"west project lock must use schema {WEST_LOCK_SCHEMA}: {lock_path}"
        )
    records = value.get("projects")
    if not isinstance(records, list) or not records:
        raise RuntimeError("west project lock must contain a non-empty projects list")
    locked: dict[str, tuple[Path, str, str]] = {}
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise RuntimeError(f"west project lock record {index} must be an object")
        name = record.get("name")
        raw_path = record.get("path")
        revision = record.get("revision")
        sha = record.get("sha")
        if not all(
            isinstance(field, str) and field
            for field in (name, raw_path, revision, sha)
        ):
            raise RuntimeError(
                f"west project lock record {index} has missing string fields"
            )
        relative = Path(raw_path)
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(
                f"west project lock record {name!r} has unsafe path {raw_path!r}"
            )
        if len(sha) != 40 or any(character not in "0123456789abcdef" for character in sha):
            raise RuntimeError(
                f"west project lock record {name!r} has invalid SHA {sha!r}"
            )
        if name in locked:
            raise RuntimeError(f"west project lock repeats project {name!r}")
        locked[name] = (relative, revision, sha)
    return locked


def _git_common_checkout(repo_root: Path) -> Path | None:
    try:
        raw = git_bytes(repo_root, ["rev-parse", "--git-common-dir"])
    except RuntimeError:
        return None
    common = Path(os.fsdecode(raw.strip()))
    if not common.is_absolute():
        common = repo_root / common
    common = common.resolve()
    if common.name != ".git":
        return None
    return common.parent


def validate_west_workspace_config(
    workspace_root: Path,
    *,
    phase: str,
) -> None:
    """Require the local west configuration used by the build."""

    config_path = workspace_root / ".west/config"
    try:
        payload = _read_regular_file_bytes(
            config_path,
            subject="west workspace configuration",
            phase=phase,
        ).decode("utf-8")
    except (RuntimeError, UnicodeError) as exc:
        raise RuntimeError(
            f"west workspace configuration is unavailable {phase}: "
            f"{config_path}: {exc}"
        ) from exc

    parser = configparser.ConfigParser(interpolation=None, strict=True)
    try:
        parser.read_string(payload)
    except configparser.Error as exc:
        raise RuntimeError(
            f"west workspace configuration is invalid {phase}: {exc}"
        ) from exc
    actual = {
        section: dict(parser.items(section, raw=True))
        for section in parser.sections()
    }
    if parser.defaults() or actual != PINNED_WEST_CONFIG:
        raise RuntimeError(
            "west workspace configuration differs from the pinned local "
            f"configuration {phase}: {config_path}; expected "
            "[manifest] path=manifest, file=west.yml and [zephyr] base=zephyr"
        )


def _read_regular_file_bytes(
    path: Path,
    *,
    subject: str,
    phase: str,
    max_bytes: int = WEST_METADATA_MAX_BYTES,
) -> bytes:
    """Read bounded regular-file bytes without following a raced replacement."""

    if max_bytes <= 0:
        raise RuntimeError(
            f"{subject} has an invalid non-positive read limit {phase}: "
            f"{max_bytes}"
        )
    try:
        before = path.lstat()
    except OSError as exc:
        raise RuntimeError(
            f"{subject} is unavailable {phase}: {path}: {exc}"
        ) from exc
    if not stat.S_ISREG(before.st_mode):
        raise RuntimeError(
            f"{subject} must be a regular non-symlink file {phase}: "
            f"{path} (mode {before.st_mode:o})"
        )
    if before.st_size > max_bytes:
        raise RuntimeError(
            f"{subject} exceeds the {max_bytes}-byte verification limit "
            f"{phase}: {path} ({before.st_size} bytes)"
        )

    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise RuntimeError(
            f"{subject} cannot be opened safely {phase}: {path}: {exc}"
        ) from exc
    try:
        try:
            opened = os.fstat(descriptor)
            if not stat.S_ISREG(opened.st_mode):
                raise RuntimeError(
                    f"{subject} became a non-regular file while it was opened "
                    f"{phase}: {path}"
                )
            if opened.st_size > max_bytes:
                raise RuntimeError(
                    f"{subject} exceeds the {max_bytes}-byte verification "
                    f"limit after open {phase}: {path} "
                    f"({opened.st_size} bytes)"
                )
            before_identity = (
                before.st_dev,
                before.st_ino,
                before.st_mode,
                before.st_size,
                before.st_mtime_ns,
                before.st_ctime_ns,
            )
            opened_identity = (
                opened.st_dev,
                opened.st_ino,
                opened.st_mode,
                opened.st_size,
                opened.st_mtime_ns,
                opened.st_ctime_ns,
            )
            if opened_identity != before_identity:
                raise RuntimeError(
                    f"{subject} changed identity while it was opened "
                    f"{phase}: {path}"
                )
            payload = bytearray()
            while True:
                request_bytes = min(
                    _SAFE_FILE_READ_CHUNK_BYTES,
                    max_bytes + 1 - len(payload),
                )
                chunk = os.read(descriptor, request_bytes)
                if not isinstance(chunk, bytes):
                    raise RuntimeError(
                        f"{subject} returned a non-bytes read {phase}: {path}"
                    )
                if len(chunk) > request_bytes:
                    raise RuntimeError(
                        f"{subject} returned more bytes than requested "
                        f"{phase}: {path}"
                    )
                if not chunk:
                    break
                payload.extend(chunk)
                if len(payload) > max_bytes:
                    raise RuntimeError(
                        f"{subject} exceeded the {max_bytes}-byte "
                        f"verification limit while reading {phase}: {path}"
                    )
            after_opened = os.fstat(descriptor)
        except OSError as exc:
            raise RuntimeError(
                f"{subject} cannot be read safely {phase}: {path}: {exc}"
            ) from exc
    finally:
        os.close(descriptor)

    try:
        after = path.lstat()
    except OSError as exc:
        raise RuntimeError(
            f"{subject} disappeared while it was read {phase}: {path}: {exc}"
        ) from exc
    identity_after_opened = (
        after_opened.st_dev,
        after_opened.st_ino,
        after_opened.st_mode,
        after_opened.st_size,
        after_opened.st_mtime_ns,
        after_opened.st_ctime_ns,
    )
    identity_after_path = (
        after.st_dev,
        after.st_ino,
        after.st_mode,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if (
        identity_after_opened != before_identity
        or identity_after_path != before_identity
    ):
        raise RuntimeError(
            f"{subject} changed while it was read {phase}: {path}"
        )
    return bytes(payload)


def validate_west_manifest_identity(
    workspace_root: Path,
    frozen_manifest_path: Path,
    *,
    phase: str,
) -> None:
    """Bind the live west manifest to the immutable source snapshot."""

    frozen_payload = _read_regular_file_bytes(
        frozen_manifest_path,
        subject="frozen source manifest",
        phase=phase,
    )
    live_manifest = workspace_root / WEST_MANIFEST_RELATIVE
    live_directory = live_manifest.parent
    try:
        directory_metadata = live_directory.lstat()
    except OSError as exc:
        raise RuntimeError(
            f"live west manifest directory is unavailable {phase}: "
            f"{live_directory}: {exc}"
        ) from exc
    if not stat.S_ISDIR(directory_metadata.st_mode):
        raise RuntimeError(
            f"live west manifest directory must be a real directory {phase}: "
            f"{live_directory} (mode {directory_metadata.st_mode:o})"
        )

    live_payload = _read_regular_file_bytes(
        live_manifest,
        subject="live west manifest",
        phase=phase,
    )
    if live_payload != frozen_payload:
        live_hash = hashlib.sha256(live_payload).hexdigest()
        frozen_hash = hashlib.sha256(frozen_payload).hexdigest()
        raise RuntimeError(
            "live west manifest differs from the immutable source snapshot "
            f"{phase}: {live_manifest} sha256={live_hash}; "
            f"{frozen_manifest_path} sha256={frozen_hash}"
        )


def discover_west_workspace(repo_root: Path) -> WestWorkspace:
    """Find the initialized workspace shared by a checkout or linked worktree."""

    candidates: list[Path] = [repo_root.resolve()]
    common_checkout = _git_common_checkout(repo_root)
    if common_checkout is not None and common_checkout not in candidates:
        candidates.append(common_checkout)
    reject_ambient_build_overrides()

    executable_candidates: list[str] = []
    for candidate in candidates:
        local_west = candidate / ".venv/bin/west"
        if local_west.is_file():
            executable_candidates.append(os.fspath(local_west))
    installed_west = shutil.which("west")
    if installed_west:
        executable_candidates.append(installed_west)
    executable_candidates = list(dict.fromkeys(executable_candidates))
    if not executable_candidates:
        raise RuntimeError(
            "west is missing; create the repository Python environment first"
        )

    failures: list[str] = []
    for candidate in candidates:
        for executable in executable_candidates:
            config_path = candidate / ".west/config"
            if not config_path.is_file():
                failures.append(f"{candidate} has no local .west/config")
                continue
            try:
                validate_west_workspace_config(
                    candidate,
                    phase="before workspace discovery",
                )
                config_guard = LinuxInotifyWriteGuard(
                    [candidate / ".west"],
                    subject=".west configuration",
                )
            except RuntimeError as exc:
                failures.append(f"{candidate}: {exc}")
                continue
            try:
                config_guard.assert_stable(
                    "after west discovery guard installation"
                )
                validate_west_workspace_config(
                    candidate,
                    phase="after west discovery guard installation",
                )
                config_guard.assert_stable("before west topdir")
                result = subprocess.run(
                    [executable, "topdir"],
                    cwd=candidate,
                    env=pinned_west_environment(candidate),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                )
                config_guard.assert_stable("after west topdir")
                validate_west_workspace_config(
                    candidate,
                    phase="after workspace discovery",
                )
            finally:
                config_guard.close()
            if result.returncode != 0:
                detail = result.stderr.strip() or f"exit {result.returncode}"
                failures.append(f"{executable} from {candidate}: {detail}")
                continue
            workspace_root = Path(result.stdout.strip()).resolve()
            if workspace_root != candidate.resolve():
                failures.append(
                    f"{executable} returned unexpected topdir {workspace_root} "
                    f"for pinned workspace {candidate.resolve()}"
                )
                continue
            checkout_matches = False
            try:
                repo_root.resolve().relative_to(workspace_root)
                checkout_matches = True
            except ValueError:
                checkout_matches = common_checkout == workspace_root
            if not checkout_matches:
                failures.append(
                    f"{workspace_root} does not own checkout {repo_root.resolve()}"
                )
                continue
            return WestWorkspace(executable, workspace_root)

    detail = "; ".join(failures[:4])
    raise RuntimeError(
        "Zephyr matrix builds require an initialized west workspace for this "
        f"checkout; discovery failed: {detail}"
    )


def active_west_projects(
    workspace: WestWorkspace,
    lock_path: Path,
    frozen_manifest_path: Path,
) -> tuple[WestProject, ...]:
    """Resolve active projects against the repository-owned immutable lock."""

    locked = _load_west_lock(lock_path)
    validate_west_workspace_config(
        workspace.root,
        phase="before active project resolution",
    )
    config_guard = LinuxInotifyWriteGuard(
        [
            workspace.root / ".west",
            workspace.root / WEST_MANIFEST_RELATIVE.parent,
        ],
        subject=".west configuration or live west manifest",
    )
    try:
        config_guard.assert_stable("after west metadata guard installation")
        validate_west_workspace_config(
            workspace.root,
            phase="after west metadata guard installation",
        )
        validate_west_manifest_identity(
            workspace.root,
            frozen_manifest_path,
            phase="after west metadata guard installation",
        )
        config_guard.assert_stable("before west list")
        output = _run_capture(
            [
                workspace.executable,
                "list",
                "-f",
                "{name}\t{path}\t{revision}\t{cloned}",
            ],
            cwd=workspace.root,
            env=pinned_west_environment(workspace.root),
        ).decode("utf-8", errors="replace")
        config_guard.assert_stable("after west list")
        validate_west_workspace_config(
            workspace.root,
            phase="after active project resolution",
        )
        validate_west_manifest_identity(
            workspace.root,
            frozen_manifest_path,
            phase="after active project resolution",
        )
        config_guard.assert_stable("after active project validation")
    finally:
        config_guard.close()
    projects: list[WestProject] = []
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) != 4:
            raise RuntimeError(f"cannot parse west project record: {line!r}")
        name, relative_text, revision, cloned = fields
        if name == "manifest":
            continue
        if cloned != "cloned":
            raise RuntimeError(
                f"active west project {name!r} is not cloned ({relative_text})"
            )
        relative = Path(relative_text)
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(
                f"west project {name!r} has unsafe path {relative_text!r}"
            )
        locked_record = locked.pop(name, None)
        if locked_record is None:
            raise RuntimeError(
                f"active west project {name!r} is missing from {lock_path}"
            )
        locked_path, locked_revision, expected_sha = locked_record
        if relative != locked_path or revision != locked_revision:
            raise RuntimeError(
                f"active west project {name!r} differs from lock: "
                f"path={relative}, revision={revision!r}; expected "
                f"path={locked_path}, revision={locked_revision!r}"
            )
        path = (workspace.root / relative).resolve()
        projects.append(WestProject(name, path, revision, expected_sha))
    if locked:
        raise RuntimeError(
            "west project lock contains inactive or missing projects: "
            + ", ".join(sorted(locked))
        )
    if not projects:
        raise RuntimeError("west returned no active non-manifest projects")
    return tuple(projects)


def _west_symlink_problems(
    project: WestProject,
    guarded_roots: Sequence[Path],
) -> list[str]:
    try:
        output = git_bytes(
            project.path,
            ["ls-files", "--stage", "-z"],
        )
    except RuntimeError as exc:
        return [f"{project.name}: cannot inspect tracked symlinks: {exc}"]
    problems: list[str] = []
    for record in (entry for entry in output.split(b"\0") if entry):
        metadata, separator, raw_path = record.partition(b"\t")
        if not separator or not metadata.startswith(b"120000 "):
            continue
        relative = _safe_relative_path(raw_path)
        link = project.path / relative
        try:
            target = link.resolve(strict=False)
        except (OSError, RuntimeError) as exc:
            problems.append(
                f"{project.name}: cannot resolve symlink {relative}: {exc}"
            )
            continue
        if not any(
            target == root or root in target.parents
            for root in guarded_roots
        ):
            problems.append(
                f"{project.name}: tracked symlink {relative} resolves outside "
                f"guarded west projects to {target}"
            )
    return problems


def _ignored_west_path_is_inert(relative: Path) -> bool:
    if "__pycache__" in relative.parts and relative.suffix in {".pyc", ".pyo"}:
        return True
    return (
        len(relative.parts) >= 2
        and relative.parts[0] == ".cache"
        and relative.parts[1] == "ToolchainCapabilityDatabase"
    )


def _ignored_west_path_problems(project: WestProject) -> list[str]:
    try:
        output = git_bytes(
            project.path,
            [
                "ls-files",
                "--others",
                "--ignored",
                "--exclude-standard",
                "-z",
            ],
        )
    except RuntimeError as exc:
        return [f"{project.name}: cannot inspect ignored paths: {exc}"]
    ignored = [
        _safe_relative_path(raw_path)
        for raw_path in output.split(b"\0")
        if raw_path
    ]
    material = [
        os.fspath(relative)
        for relative in ignored
        if not _ignored_west_path_is_inert(relative)
    ]
    if not material:
        return []
    return [
        f"{project.name}: ignored paths can affect unpinned build inputs "
        f"{_preview(material)}"
    ]


def _west_index_flag_problems(project: WestProject) -> list[str]:
    try:
        output = git_bytes(project.path, ["ls-files", "-v", "-z"])
    except RuntimeError as exc:
        return [f"{project.name}: cannot inspect index flags: {exc}"]
    flagged: list[str] = []
    for record in (entry for entry in output.split(b"\0") if entry):
        if len(record) < 3 or record[1:2] != b" ":
            flagged.append("<malformed git ls-files record>")
            continue
        tag = os.fsdecode(record[:1])
        relative = _safe_relative_path(record[2:])
        flags: list[str] = []
        if tag.upper() == "S":
            flags.append("skip-worktree")
        if tag.islower():
            flags.append("assume-unchanged")
        if flags:
            flagged.append(
                f"{relative} ({', '.join(flags)})"
            )
    if not flagged:
        return []
    return [
        f"{project.name}: Git index flags can conceal dependency changes "
        f"{_preview(flagged)}"
    ]


def validate_west_projects(
    projects: Sequence[WestProject],
    *,
    phase: str,
) -> None:
    """Require manifest-pinned HEADs and clean project worktrees."""

    problems: list[str] = []
    guarded_roots = tuple(project.path.resolve() for project in projects)
    for project in projects:
        try:
            resolved = git_bytes(
                project.path,
                ["rev-parse", "--verify", MANIFEST_REV],
            ).decode("ascii").strip()
            head = git_bytes(
                project.path,
                ["rev-parse", "--verify", "HEAD"],
            ).decode("ascii").strip()
            status = git_bytes(
                project.path,
                [
                    "status",
                    "--porcelain=v1",
                    "-z",
                    "--untracked-files=all",
                    "--ignore-submodules=none",
                ],
            )
        except RuntimeError as exc:
            problems.append(f"{project.name}: {exc}")
            continue
        if resolved != project.expected_sha:
            problems.append(
                f"{project.name}: manifest-rev moved from "
                f"{project.expected_sha} to {resolved}"
            )
        if head != project.expected_sha:
            problems.append(
                f"{project.name}: HEAD {head} != manifest SHA "
                f"{project.expected_sha} ({project.revision})"
            )
        entries = _status_entries(status)
        if entries:
            problems.append(f"{project.name}: dirty paths {_preview(entries)}")
        problems.extend(_west_index_flag_problems(project))
        problems.extend(_west_symlink_problems(project, guarded_roots))
        problems.extend(_ignored_west_path_problems(project))
    if problems:
        raise RuntimeError(
            f"west dependency validation failed {phase}: " + "; ".join(problems)
        )


class LinuxInotifyWriteGuard:
    """Record writes anywhere beneath dependency worktrees, including restores."""

    _IN_MODIFY = 0x00000002
    _IN_ATTRIB = 0x00000004
    _IN_CLOSE_WRITE = 0x00000008
    _IN_MOVED_FROM = 0x00000040
    _IN_MOVED_TO = 0x00000080
    _IN_CREATE = 0x00000100
    _IN_DELETE = 0x00000200
    _IN_DELETE_SELF = 0x00000400
    _IN_MOVE_SELF = 0x00000800
    _IN_Q_OVERFLOW = 0x00004000
    _WATCH_MASK = (
        _IN_MODIFY
        | _IN_ATTRIB
        | _IN_CLOSE_WRITE
        | _IN_MOVED_FROM
        | _IN_MOVED_TO
        | _IN_CREATE
        | _IN_DELETE
        | _IN_DELETE_SELF
        | _IN_MOVE_SELF
    )
    _EVENT = struct.Struct("iIII")

    def __init__(
        self,
        roots: Sequence[Path],
        *,
        subject: str = "west dependency worktree",
        exclude_git: bool = True,
    ):
        if not sys.platform.startswith("linux"):
            raise RuntimeError(
                "exact Zephyr verification requires Linux inotify dependency guards"
            )
        libc = ctypes.CDLL(None, use_errno=True)
        self._init = libc.inotify_init1
        self._init.argtypes = [ctypes.c_int]
        self._init.restype = ctypes.c_int
        self._add_watch = libc.inotify_add_watch
        self._add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        self._add_watch.restype = ctypes.c_int
        self._fd = self._init(os.O_NONBLOCK | os.O_CLOEXEC)
        if self._fd < 0:
            error = ctypes.get_errno()
            raise RuntimeError(
                f"cannot initialize inotify dependency guard: {os.strerror(error)}"
            )
        self._subject = subject
        self._exclude_git = exclude_git
        self._paths: dict[int, Path] = {}
        try:
            for root in sorted({path.resolve() for path in roots}):
                self._watch_tree(root)
        except BaseException:
            self.close()
            raise

    def _watch_tree(self, root: Path) -> None:
        if not root.is_dir():
            raise RuntimeError(f"dependency worktree is missing: {root}")
        for directory, names, _files in os.walk(root, followlinks=False):
            names[:] = [
                name
                for name in names
                if (not self._exclude_git or name != ".git")
                and not (Path(directory) / name).is_symlink()
            ]
            self._watch(Path(directory))

    def _watch(self, path: Path) -> None:
        descriptor = self._add_watch(
            self._fd,
            os.fsencode(path),
            self._WATCH_MASK,
        )
        if descriptor < 0:
            error = ctypes.get_errno()
            if error == errno.ENOSPC:
                detail = "inotify watch limit exhausted"
            else:
                detail = os.strerror(error)
            raise RuntimeError(
                f"cannot guard dependency directory {path}: {detail}"
            )
        self._paths[descriptor] = path

    def _events(self) -> list[str]:
        events: list[str] = []
        while True:
            try:
                payload = os.read(self._fd, 1024 * 1024)
            except BlockingIOError:
                break
            if not payload:
                break
            offset = 0
            while offset < len(payload):
                descriptor, mask, _cookie, name_length = self._EVENT.unpack_from(
                    payload,
                    offset,
                )
                offset += self._EVENT.size
                raw_name = payload[offset:offset + name_length].rstrip(b"\0")
                offset += name_length
                if mask & self._IN_Q_OVERFLOW:
                    events.append("inotify event queue overflowed")
                    continue
                base = self._paths.get(descriptor, Path("<unknown>"))
                path = base / os.fsdecode(raw_name) if raw_name else base
                events.append(os.fspath(path))
        return events

    def assert_stable(self, phase: str) -> None:
        events = self._events()
        if events:
            raise RuntimeError(
                f"{self._subject} changed during verification "
                f"({phase}); first event: {events[0]}"
            )

    def close(self) -> None:
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1


@contextmanager
def frozen_west_dependencies(
    projects: Sequence[WestProject],
    *,
    workspace: WestWorkspace,
    lock_path: Path,
    frozen_manifest_path: Path,
) -> Iterator[LinuxInotifyWriteGuard]:
    """Freeze active projects and west metadata for one exact build matrix."""

    validate_west_workspace_config(
        workspace.root,
        phase="before matrix",
    )
    validate_west_manifest_identity(
        workspace.root,
        frozen_manifest_path,
        phase="before matrix",
    )
    validate_west_projects(projects, phase="before matrix")
    guard = LinuxInotifyWriteGuard(
        [
            *(project.path for project in projects),
            workspace.root / ".west",
            workspace.root / WEST_MANIFEST_RELATIVE.parent,
        ],
        subject=(
            "west dependency worktree, .west configuration, or live west "
            "manifest"
        ),
    )
    primary_error: BaseException | None = None
    try:
        try:
            guard.assert_stable("after guard installation")
            validate_west_workspace_config(
                workspace.root,
                phase="after guard installation",
            )
            validate_west_manifest_identity(
                workspace.root,
                frozen_manifest_path,
                phase="after guard installation",
            )
            validate_west_projects(projects, phase="after guard installation")
            resolved_projects = active_west_projects(
                workspace,
                lock_path,
                frozen_manifest_path,
            )
            if tuple(resolved_projects) != tuple(projects):
                raise RuntimeError(
                    "active west project set changed before the exact build "
                    "matrix"
                )
            guard.assert_stable("after project revalidation")
            yield guard
        except BaseException as exc:
            primary_error = exc
    finally:
        cleanup_errors: list[Exception] = []
        try:
            try:
                guard.assert_stable("matrix completion")
            except Exception as exc:
                cleanup_errors.append(exc)
            try:
                validate_west_projects(projects, phase="after matrix")
            except Exception as exc:
                cleanup_errors.append(exc)
            try:
                validate_west_workspace_config(
                    workspace.root,
                    phase="after matrix",
                )
            except Exception as exc:
                cleanup_errors.append(exc)
            try:
                validate_west_manifest_identity(
                    workspace.root,
                    frozen_manifest_path,
                    phase="after matrix",
                )
            except Exception as exc:
                cleanup_errors.append(exc)
            try:
                resolved_projects = active_west_projects(
                    workspace,
                    lock_path,
                    frozen_manifest_path,
                )
                if tuple(resolved_projects) != tuple(projects):
                    raise RuntimeError(
                        "active west project set changed across the exact "
                        "build matrix"
                    )
            except Exception as exc:
                cleanup_errors.append(exc)
            try:
                guard.assert_stable("after matrix project resolution")
            except Exception as exc:
                cleanup_errors.append(exc)
        finally:
            try:
                guard.close()
            except Exception as exc:
                cleanup_errors.append(exc)
    if primary_error is not None:
        if cleanup_errors:
            combined = VerificationMatrixFailure(
                primary_error,
                cleanup_errors,
            )
            if isinstance(primary_error, Exception):
                raise combined from primary_error
            raise primary_error from combined
        raise primary_error
    if cleanup_errors:
        combined = VerificationMatrixFailure(None, cleanup_errors)
        raise combined from cleanup_errors[0]


def require_build_root_outside_projects(
    build_root: Path,
    projects: Sequence[WestProject],
) -> None:
    """Keep generated build and cache files outside dependency worktrees."""

    resolved = build_root.resolve()
    for project in projects:
        try:
            resolved.relative_to(project.path.resolve())
        except ValueError:
            continue
        raise RuntimeError(
            f"Zephyr build root {resolved} is inside west dependency "
            f"{project.name} ({project.path})"
        )
