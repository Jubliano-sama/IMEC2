#!/usr/bin/env python3
"""Run the repository's source, native, and optional exact-role verification."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
FIRMWARE_ROOT = REPO_ROOT / "firmware"
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
        "mesh deployment policy negative suite",
        "firmware/tests/mesh_integration/test_mesh_deployment_policy.py",
    ),
)
MESH_PRESETS = ("mesh_clicker", "mesh_anchor", "mesh_gateway")


def _run(
    label: str,
    command: list[str],
    *,
    env: dict[str, str] | None = None,
) -> None:
    print(f"\n==> {label}", flush=True)
    print(" ".join(command), flush=True)
    subprocess.run(
        command,
        cwd=REPO_ROOT,
        env=env,
        check=True,
    )


def _source_checks() -> None:
    for label, relative in SOURCE_CHECKS:
        _run(label, [str(PYTHON), relative])


def _native_environment(sanitizers: bool) -> dict[str, str]:
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
        str(FIRMWARE_ROOT),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Debug",
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
                "firmware/scripts/run_mesh_stress.py",
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


def _run_exact_roles(build_root: Path, jobs: int) -> None:
    local_west = REPO_ROOT / ".venv/bin/west"
    west = str(local_west) if local_west.is_file() else shutil.which("west")
    if west is None:
        raise RuntimeError(
            "west is missing; create the repository Python environment first"
        )
    for preset in MESH_PRESETS:
        build_dir = build_root / preset.replace("_", "-")
        _run(
            f"build exact {preset} role",
            [
                west,
                "build",
                "--pristine=always",
                "--no-sysbuild",
                "-s",
                "firmware/app",
                "-b",
                "nrf52833dk/nrf52833",
                "--build-dir",
                str(build_dir),
                "--",
                f"-DIMEC_BUILD_PRESET={preset}",
            ],
            env={**os.environ, "CMAKE_BUILD_PARALLEL_LEVEL": str(jobs)},
        )
        _run(
            f"verify static stack policy for {preset}",
            [
                str(PYTHON),
                "firmware/scripts/verify_stack_evidence.py",
                "--build-dir",
                str(build_dir),
            ],
        )

    persistence_build = build_root / "mesh-persistence-native"
    _run(
        "build Zephyr NVS persistence test",
        [
            west,
            "build",
            "--pristine=always",
            "--no-sysbuild",
            "-s",
            "firmware/app/tests/mesh_persistence",
            "-b",
            "native_sim/native/64",
            "--build-dir",
            str(persistence_build),
        ],
        env={**os.environ, "CMAKE_BUILD_PARALLEL_LEVEL": str(jobs)},
    )
    persistence_binary = persistence_build / "zephyr/zephyr.exe"
    if not persistence_binary.is_file():
        raise RuntimeError(
            f"Zephyr persistence test binary is missing: {persistence_binary}"
        )
    _run("run Zephyr NVS persistence test", [str(persistence_binary)])


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
        default=Path("build/verified-roles"),
        help="Build root used with --exact-roles (default: build/verified-roles).",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(os.cpu_count() or 2, 8),
        help="Parallel build/test jobs (default: up to 8 logical CPUs).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    if args.stress_count < 0:
        parser.error("--stress-count must be zero or positive")
    if args.checks_only and args.sanitizers:
        parser.error("--sanitizers has no effect with --checks-only")

    try:
        _source_checks()
        if not args.checks_only:
            if args.native_build_dir is not None:
                build_dir = args.native_build_dir
                if not build_dir.is_absolute():
                    build_dir = REPO_ROOT / build_dir
                build_dir.mkdir(parents=True, exist_ok=True)
                failure_dir = args.stress_failure_dir
                if not failure_dir.is_absolute():
                    failure_dir = REPO_ROOT / failure_dir
                _run_native(
                    build_dir,
                    args.jobs,
                    args.sanitizers,
                    args.stress_count,
                    failure_dir,
                )
            else:
                with tempfile.TemporaryDirectory(prefix="imec2-native-") as temp:
                    failure_dir = args.stress_failure_dir
                    if not failure_dir.is_absolute():
                        failure_dir = REPO_ROOT / failure_dir
                    _run_native(
                        Path(temp),
                        args.jobs,
                        args.sanitizers,
                        args.stress_count,
                        failure_dir,
                    )
        if args.exact_roles:
            build_root = args.zephyr_build_root
            if not build_root.is_absolute():
                build_root = REPO_ROOT / build_root
            build_root.mkdir(parents=True, exist_ok=True)
            _run_exact_roles(build_root, args.jobs)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"\nverification failed: {exc}", file=sys.stderr)
        return 1

    scope = "source checks"
    if not args.checks_only:
        scope += ", native tests"
    if args.exact_roles:
        scope += ", exact production roles"
    print(f"\nverification passed: {scope}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
