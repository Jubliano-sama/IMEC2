#!/usr/bin/env python3
"""Run the repository's source, native, and optional exact-role verification."""

from __future__ import annotations

import argparse
import fcntl
import os
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

from verification_inputs import (
    LinuxInotifyWriteGuard,
    WestWorkspace,
    active_west_projects,
    discover_west_workspace,
    frozen_source_snapshot,
    frozen_west_dependencies,
    pinned_west_environment,
    reject_ambient_build_overrides,
    repository_fingerprint,
    require_build_root_outside_projects,
    require_clean_build_inputs,
    verification_environment,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
PYTHON = Path(sys.executable)
SOURCE_CHECKS = (
    ("repository truth", "firmware/scripts/check_repository_truth.py"),
    ("architecture boundaries", "firmware/scripts/check_architecture_boundaries.py"),
    ("agent guidance", "firmware/scripts/check_agent_guidance.py"),
    ("mesh deployment policy", "firmware/scripts/check_mesh_deployment_policy.py"),
    (
        "repository truth self-test",
        "firmware/tests/mesh_integration/test_repository_truth.py",
    ),
    (
        "architecture boundary self-test",
        "firmware/tests/mesh_integration/test_architecture_boundaries.py",
    ),
    (
        "agent preflight self-test",
        "firmware/tests/mesh_integration/test_agent_preflight.py",
    ),
    (
        "verification entrypoint self-test",
        "firmware/tests/mesh_integration/test_verify_changes.py",
    ),
    (
        "verification input self-test",
        "firmware/tests/mesh_integration/test_verification_inputs.py",
    ),
    (
        "mesh deployment policy negative suite",
        "firmware/tests/mesh_integration/test_mesh_deployment_policy.py",
    ),
)
MESH_PRESETS = ("mesh_clicker", "mesh_anchor", "mesh_gateway")
COMPATIBILITY_PRESETS = (
    "mesh_transmitter",
    "mesh_transmitter_forcedhop",
    "ml_clicker",
    "ml_anchor_1",
    "ml_anchor_8",
)
LEGACY_ROLES = ("clicker", "anchor", "gateway")
WEST_PROJECT_LOCK = Path("firmware/west_projects.lock.json")
EXACT_BUILD_INPUTS = (
    "firmware/app",
    "firmware/include",
    "firmware/scripts",
    "firmware/src",
    str(WEST_PROJECT_LOCK),
    "dwm3000 examples and sdk",
)
_EXPECTED_REPOSITORY_FINGERPRINT: str | None = None
_VERIFICATION_ROOT = REPO_ROOT
_EXPECTED_SNAPSHOT_FINGERPRINT: str | None = None
_ACTIVE_SOURCE_GUARD: LinuxInotifyWriteGuard | None = None
_ACTIVE_DEPENDENCY_GUARD: LinuxInotifyWriteGuard | None = None


def _repository_fingerprint(repo_root: Path | None = None) -> str:
    return repository_fingerprint(REPO_ROOT if repo_root is None else repo_root)


def _assert_repository_stable(label: str) -> None:
    if _EXPECTED_REPOSITORY_FINGERPRINT is None:
        return
    actual = _repository_fingerprint()
    if actual != _EXPECTED_REPOSITORY_FINGERPRINT:
        raise RuntimeError(
            f"repository changed during verification ({label}); discard this mixed-revision "
            "result and rerun from a stable checkout"
        )


def _assert_snapshot_stable(label: str) -> None:
    if _EXPECTED_SNAPSHOT_FINGERPRINT is None:
        return
    actual = repository_fingerprint(_VERIFICATION_ROOT)
    if actual != _EXPECTED_SNAPSHOT_FINGERPRINT:
        raise RuntimeError(
            f"immutable source snapshot changed during verification ({label}); "
            "discard this result and rerun"
        )


def _require_clean_repository() -> None:
    require_clean_build_inputs(REPO_ROOT, EXACT_BUILD_INPUTS)


def _run(
    label: str,
    command: list[str],
    *,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> None:
    if cwd is None:
        cwd = _VERIFICATION_ROOT
    environment = verification_environment(env)
    _assert_repository_stable(f"before {label}")
    _assert_snapshot_stable(f"before {label}")
    if _ACTIVE_SOURCE_GUARD is not None:
        _ACTIVE_SOURCE_GUARD.assert_stable(f"before {label}")
    if _ACTIVE_DEPENDENCY_GUARD is not None:
        _ACTIVE_DEPENDENCY_GUARD.assert_stable(f"before {label}")
    print(f"\n==> {label}", flush=True)
    print(" ".join(command), flush=True)
    try:
        subprocess.run(
            command,
            cwd=cwd,
            env=environment,
            check=True,
        )
    finally:
        if _ACTIVE_DEPENDENCY_GUARD is not None:
            _ACTIVE_DEPENDENCY_GUARD.assert_stable(f"after {label}")
        if _ACTIVE_SOURCE_GUARD is not None:
            _ACTIVE_SOURCE_GUARD.assert_stable(f"after {label}")
        _assert_snapshot_stable(f"after {label}")
        _assert_repository_stable(f"after {label}")


def _source_checks() -> None:
    for label, relative in SOURCE_CHECKS:
        _run(label, [str(PYTHON), str(_VERIFICATION_ROOT / relative)])


def _west_executable() -> str:
    return discover_west_workspace(REPO_ROOT).executable


@contextmanager
def _locked_zephyr_build_root(requested: Path | None) -> Iterator[Path]:
    if requested is None:
        with tempfile.TemporaryDirectory(prefix="imec2-zephyr-") as temporary:
            build_root = Path(temporary)
            print(f"Zephyr verification build root: {build_root}", flush=True)
            yield build_root
        return

    build_root = requested if requested.is_absolute() else REPO_ROOT / requested
    build_root.mkdir(parents=True, exist_ok=True)
    lock_path = build_root / ".verify.lock"
    lock_flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        lock_flags |= os.O_NOFOLLOW
    lock_fd = os.open(lock_path, lock_flags, 0o600)
    with os.fdopen(lock_fd, "r+", encoding="utf-8") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError(
                f"Zephyr verification build root is already in use: {build_root}"
            ) from None
        print(f"Zephyr verification build root: {build_root}", flush=True)
        yield build_root


def _native_environment(sanitizers: bool) -> dict[str, str]:
    reject_ambient_build_overrides()
    env = os.environ.copy()
    if sanitizers:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    return env


def _run_native(
    build_dir: Path,
    jobs: int,
    sanitizers: bool,
    stress_count: int,
    stress_failure_dir: Path,
) -> None:
    configure = [
        "cmake",
        "-S",
        str(_VERIFICATION_ROOT / "firmware"),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DPython3_EXECUTABLE={PYTHON}",
    ]
    if sanitizers:
        configure.append("-DIMEC_ENABLE_SANITIZERS=ON")
    env = _native_environment(sanitizers)
    _run("configure native tests", configure, env=env)
    _run(
        "build native tests",
        ["cmake", "--build", str(build_dir), "--parallel", str(jobs)],
        env=env,
    )
    _run(
        "run complete native suite",
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "--parallel",
            str(jobs),
        ],
        env=env,
    )
    if stress_count:
        _run(
            f"run {stress_count}-seed deterministic mesh stress merge gate",
            [
                str(PYTHON),
                str(_VERIFICATION_ROOT / "firmware/scripts/run_mesh_stress.py"),
                "--build-dir",
                str(build_dir),
                "--failure-dir",
                str(stress_failure_dir),
                "--scenario",
                "busy-line",
                "--seed-start",
                "1",
                "--count",
                str(stress_count),
                "--jobs",
                str(jobs),
                "--packets",
                "2",
                "--loss",
                "50",
                "--ack-loss",
                "300",
                "--duplicate",
                "300",
                "--delay",
                "500",
                "--max-delay-us",
                "4000",
            ],
            env=env,
        )


def _zephyr_environment(
    jobs: int,
    workspace: WestWorkspace,
) -> dict[str, str]:
    environment = pinned_west_environment(workspace.root)
    environment.update(
        {
            "CCACHE_DISABLE": "1",
            "CMAKE_BUILD_PARALLEL_LEVEL": str(jobs),
        }
    )
    return environment


def _run_exact_roles(
    build_root: Path,
    jobs: int,
    workspace: WestWorkspace,
) -> None:
    environment = _zephyr_environment(jobs, workspace)
    user_cache = build_root / "user-cache"
    for preset in MESH_PRESETS:
        build_dir = build_root / preset.replace("_", "-")
        _run(
            f"build exact {preset} role",
            [
                workspace.executable,
                "build",
                "--pristine=always",
                "--no-sysbuild",
                "-s",
                str(_VERIFICATION_ROOT / "firmware/app"),
                "-b",
                "nrf52833dk/nrf52833",
                "--build-dir",
                str(build_dir),
                "--",
                f"-DUSER_CACHE_DIR={user_cache}",
                f"-DIMEC_BUILD_PRESET={preset}",
            ],
            env=environment,
            cwd=workspace.root,
        )
        _run(
            f"verify static stack policy for {preset}",
            [
                str(PYTHON),
                str(_VERIFICATION_ROOT / "firmware/scripts/verify_stack_evidence.py"),
                "--build-dir",
                str(build_dir),
            ],
        )

    persistence_build = build_root / "mesh-persistence-native"
    _run(
        "build Zephyr NVS persistence test",
        [
            workspace.executable,
            "build",
            "--pristine=always",
            "--no-sysbuild",
            "-s",
            str(_VERIFICATION_ROOT / "firmware/app/tests/mesh_persistence"),
            "-b",
            "native_sim/native/64",
            "--build-dir",
            str(persistence_build),
            "--",
            f"-DUSER_CACHE_DIR={user_cache}",
        ],
        env=environment,
        cwd=workspace.root,
    )
    persistence_binary = persistence_build / "zephyr/zephyr.exe"
    if not persistence_binary.is_file():
        raise RuntimeError(
            f"Zephyr persistence test binary is missing: {persistence_binary}"
        )
    _run(
        "run Zephyr NVS persistence test",
        [str(persistence_binary)],
        cwd=persistence_build,
    )


def _run_compatibility_builds(
    build_root: Path,
    jobs: int,
    workspace: WestWorkspace,
) -> None:
    environment = _zephyr_environment(jobs, workspace)
    user_cache = build_root.parent / "user-cache"
    for preset in COMPATIBILITY_PRESETS:
        _run(
            f"build compatibility preset {preset}",
            [
                workspace.executable,
                "build",
                "--pristine=always",
                "--no-sysbuild",
                "-s",
                str(_VERIFICATION_ROOT / "firmware/app"),
                "-b",
                "nrf52833dk/nrf52833",
                "--build-dir",
                str(build_root / preset.replace("_", "-")),
                "--",
                f"-DUSER_CACHE_DIR={user_cache}",
                f"-DIMEC_BUILD_PRESET={preset}",
            ],
            env=environment,
            cwd=workspace.root,
        )
    for role in LEGACY_ROLES:
        _run(
            f"build legacy role {role}",
            [
                workspace.executable,
                "build",
                "--pristine=always",
                "--no-sysbuild",
                "-s",
                str(_VERIFICATION_ROOT / "firmware/app"),
                "-b",
                "nrf52833dk/nrf52833",
                "--build-dir",
                str(build_root / f"legacy-{role}"),
                "--",
                f"-DUSER_CACHE_DIR={user_cache}",
                f"-DFIRMWARE_ROLE={role}",
            ],
            env=environment,
            cwd=workspace.root,
        )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run source-of-truth and policy checks, a fresh native CTest suite, "
            "and optional exact production-role builds."
        )
    )
    parser.add_argument(
        "--checks-only",
        action="store_true",
        help="Run executable repository checks without compiling native tests.",
    )
    parser.add_argument(
        "--sanitizers",
        action="store_true",
        help="Enable AddressSanitizer and UndefinedBehaviorSanitizer.",
    )
    parser.add_argument(
        "--native-build-dir",
        type=Path,
        help="Keep/reuse this native build directory instead of a fresh temporary one.",
    )
    parser.add_argument(
        "--exact-roles",
        action="store_true",
        help="Also build mesh_clicker, mesh_anchor, and mesh_gateway and run static stack gates.",
    )
    parser.add_argument(
        "--compatibility-builds",
        action="store_true",
        help=(
            "Also compile legacy roles, both mesh traffic sources, ml_clicker, "
            "and the first/last deterministic ML anchor slots."
        ),
    )
    parser.add_argument(
        "--stress-count",
        type=int,
        default=500,
        help="Deterministic busy-line merge-gate cases after CTest (default: 500; zero skips).",
    )
    parser.add_argument(
        "--stress-failure-dir",
        type=Path,
        default=Path("logs/mesh-stress-failures/verify-changes"),
        help="Replay artifact directory for failed stress cases.",
    )
    parser.add_argument(
        "--zephyr-build-root",
        type=Path,
        help=(
            "Retained exclusive build root for Zephyr matrices; when omitted, "
            "use a fresh temporary directory and remove it after the run."
        ),
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(os.cpu_count() or 2, 8),
        help="Parallel build/test jobs (default: up to 8 logical CPUs).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    global _ACTIVE_DEPENDENCY_GUARD
    global _ACTIVE_SOURCE_GUARD
    global _EXPECTED_REPOSITORY_FINGERPRINT
    global _EXPECTED_SNAPSHOT_FINGERPRINT
    global _VERIFICATION_ROOT

    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    if args.stress_count < 0:
        parser.error("--stress-count must be zero or positive")
    if args.checks_only and args.sanitizers:
        parser.error("--sanitizers has no effect with --checks-only")

    try:
        _EXPECTED_REPOSITORY_FINGERPRINT = _repository_fingerprint()
        print(
            "Repository verification fingerprint: "
            f"{_EXPECTED_REPOSITORY_FINGERPRINT}",
            flush=True,
        )
        if args.exact_roles or args.compatibility_builds:
            _require_clean_repository()
        with frozen_source_snapshot(
            REPO_ROOT,
            expected_fingerprint=_EXPECTED_REPOSITORY_FINGERPRINT,
        ) as snapshot:
            previous_root = _VERIFICATION_ROOT
            _VERIFICATION_ROOT = snapshot.root
            _EXPECTED_SNAPSHOT_FINGERPRINT = snapshot.fingerprint
            _ACTIVE_SOURCE_GUARD = snapshot.write_guard
            try:
                print(
                    f"Immutable source snapshot: {snapshot.root}",
                    flush=True,
                )
                _source_checks()
                if not args.checks_only:
                    failure_dir = args.stress_failure_dir
                    if not failure_dir.is_absolute():
                        failure_dir = REPO_ROOT / failure_dir
                    if args.native_build_dir is not None:
                        build_dir = args.native_build_dir
                        if not build_dir.is_absolute():
                            build_dir = REPO_ROOT / build_dir
                        build_dir.mkdir(parents=True, exist_ok=True)
                        _run_native(
                            build_dir,
                            args.jobs,
                            args.sanitizers,
                            args.stress_count,
                            failure_dir,
                        )
                    else:
                        with tempfile.TemporaryDirectory(
                            prefix="imec2-native-"
                        ) as temporary:
                            _run_native(
                                Path(temporary),
                                args.jobs,
                                args.sanitizers,
                                args.stress_count,
                                failure_dir,
                            )
                if args.exact_roles or args.compatibility_builds:
                    workspace = discover_west_workspace(REPO_ROOT)
                    projects = active_west_projects(
                        workspace,
                        _VERIFICATION_ROOT / WEST_PROJECT_LOCK,
                    )
                    if args.zephyr_build_root is not None:
                        requested_root = args.zephyr_build_root
                        if not requested_root.is_absolute():
                            requested_root = REPO_ROOT / requested_root
                        require_build_root_outside_projects(
                            requested_root,
                            projects,
                        )
                    with frozen_west_dependencies(
                        projects,
                        workspace_root=workspace.root,
                    ) as dependency_guard:
                        _ACTIVE_DEPENDENCY_GUARD = dependency_guard
                        try:
                            with _locked_zephyr_build_root(
                                args.zephyr_build_root
                            ) as build_root:
                                require_build_root_outside_projects(
                                    build_root,
                                    projects,
                                )
                                if args.exact_roles:
                                    _run_exact_roles(
                                        build_root,
                                        args.jobs,
                                        workspace,
                                    )
                                if args.compatibility_builds:
                                    _run_compatibility_builds(
                                        build_root / "compatibility",
                                        args.jobs,
                                        workspace,
                                    )
                        finally:
                            _ACTIVE_DEPENDENCY_GUARD = None
                _assert_snapshot_stable("verification completion")
                _assert_repository_stable("verification completion")
            finally:
                _ACTIVE_SOURCE_GUARD = None
                _EXPECTED_SNAPSHOT_FINGERPRINT = None
                _VERIFICATION_ROOT = previous_root
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"\nverification failed: {exc}", file=sys.stderr)
        return 1
    finally:
        _ACTIVE_DEPENDENCY_GUARD = None
        _ACTIVE_SOURCE_GUARD = None
        _EXPECTED_REPOSITORY_FINGERPRINT = None
        _EXPECTED_SNAPSHOT_FINGERPRINT = None
        _VERIFICATION_ROOT = REPO_ROOT

    scope = "source checks"
    if not args.checks_only:
        scope += ", native tests"
    if args.exact_roles:
        scope += ", exact production roles"
    if args.compatibility_builds:
        scope += ", compatibility builds"
    print(f"\nverification passed: {scope}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
