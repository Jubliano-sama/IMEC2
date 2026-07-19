#!/usr/bin/env python3
"""Sweep deterministic mesh simulator seeds and retain exact failure replays."""

from __future__ import annotations

import argparse
import concurrent.futures
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


UINT32_MASK = (1 << 32) - 1
DEFAULT_FAULT_SEED_XOR = 0xF4175EED
DEFAULT_MAX_FAILURE_ARTIFACTS = 25


def uint32(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0 or parsed > UINT32_MASK:
        raise argparse.ArgumentTypeError("seed must be in the range 0..0xffffffff")
    return parsed


def nonnegative(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def positive(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be at least one")
    return parsed


def positive_seconds(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0.0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return parsed


def fault_rate(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0 or parsed > 10_000:
        raise argparse.ArgumentTypeError("fault rates are parts per 10000")
    return parsed


@dataclass(frozen=True)
class StressCase:
    seed: int
    fault_seed: int


@dataclass(frozen=True)
class StressResult:
    case: StressCase
    command: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool = False


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description=(
            "Run deterministic mesh_stress cases in parallel. Failed cases are "
            "rerun once with a JSONL trace and an exact replay command."
        )
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=repo_root / "firmware" / "build",
        help="CMake build directory containing mesh_stress",
    )
    parser.add_argument(
        "--failure-dir",
        type=Path,
        help="failure artifact directory (default: logs/mesh-stress-failures)",
    )
    parser.add_argument(
        "--scenario",
        choices=("one-hop", "busy-line", "queue-full"),
        default="busy-line",
    )
    parser.add_argument("--seed-start", type=uint32, default=1)
    parser.add_argument("--count", type=nonnegative, default=100)
    parser.add_argument(
        "--fault-seed-start",
        type=uint32,
        help=(
            "first fault RNG seed; later cases increment it. By default each "
            "fault seed is protocol_seed XOR 0xf4175eed"
        ),
    )
    parser.add_argument("--jobs", type=nonnegative, default=1)
    parser.add_argument("--packets", type=int, choices=range(1, 5), default=2)
    parser.add_argument("--loss", type=fault_rate, default=0)
    parser.add_argument("--ack-loss", type=fault_rate, default=0)
    parser.add_argument("--duplicate", type=fault_rate, default=0)
    parser.add_argument("--delay", type=fault_rate, default=0)
    parser.add_argument("--max-delay-us", type=nonnegative, default=4000)
    parser.add_argument(
        "--reset-step",
        type=nonnegative,
        help="reset the middle node at this busy-line simulator step",
    )
    parser.add_argument("--max-steps", type=positive, default=300)
    parser.add_argument(
        "--case-timeout-s",
        type=positive_seconds,
        default=30.0,
        help="wall-clock timeout for both the first run and traced replay",
    )
    parser.add_argument(
        "--max-failure-artifacts",
        type=positive,
        default=DEFAULT_MAX_FAILURE_ARTIFACTS,
        help=(
            "maximum per-case replay directories to retain; every failing "
            "seed remains listed in the compact campaign manifest"
        ),
    )
    return parser.parse_args()


def make_cases(args: argparse.Namespace) -> list[StressCase]:
    cases = []
    for index in range(args.count):
        seed = (args.seed_start + index) & UINT32_MASK
        if args.fault_seed_start is None:
            fault_seed = seed ^ DEFAULT_FAULT_SEED_XOR
        else:
            fault_seed = (args.fault_seed_start + index) & UINT32_MASK
        cases.append(StressCase(seed=seed, fault_seed=fault_seed))
    return cases


def command_for(
    executable: Path,
    args: argparse.Namespace,
    case: StressCase,
    trace_path: Path | None = None,
) -> tuple[str, ...]:
    command = [
        str(executable),
        "--scenario",
        args.scenario,
        "--seed",
        f"0x{case.seed:08x}",
        "--fault-seed",
        f"0x{case.fault_seed:08x}",
        "--packets",
        str(args.packets),
        "--loss",
        str(args.loss),
        "--ack-loss",
        str(args.ack_loss),
        "--duplicate",
        str(args.duplicate),
        "--delay",
        str(args.delay),
        "--max-delay-us",
        str(args.max_delay_us),
        "--max-steps",
        str(args.max_steps),
    ]
    if args.reset_step is not None:
        command.extend(("--reset-step", str(args.reset_step)))
    if trace_path is None:
        command.append("--quiet")
    else:
        command.extend(("--trace", str(trace_path)))
    return tuple(command)


def bounded_command(
    command: tuple[str, ...], case_timeout_s: float
) -> tuple[str, ...]:
    return (
        "timeout",
        "--signal=TERM",
        "--kill-after=1s",
        f"{case_timeout_s:.17g}s",
        *command,
    )


def run_case(
    repo_root: Path,
    executable: Path,
    args: argparse.Namespace,
    case: StressCase,
) -> StressResult:
    command = command_for(executable, args, case)
    try:
        completed = subprocess.run(
            command,
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.case_timeout_s,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        return StressResult(
            case=case,
            command=command,
            returncode=124,
            stdout=_timeout_text(exc.stdout),
            stderr=_timeout_text(exc.stderr),
            timed_out=True,
        )
    return StressResult(
        case=case,
        command=command,
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
    )


def _timeout_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    return value.decode(errors="replace") if isinstance(value, bytes) else value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _snapshot_executable(executable: Path, snapshot_dir: Path) -> tuple[Path, str]:
    source_hash_before = _sha256(executable)
    snapshot = snapshot_dir / "mesh_stress"

    shutil.copy2(executable, snapshot)
    snapshot_hash = _sha256(snapshot)
    source_hash_after = _sha256(executable)
    if source_hash_before != snapshot_hash or source_hash_after != snapshot_hash:
        raise RuntimeError("mesh_stress changed while creating campaign snapshot")
    if not os.access(snapshot, os.X_OK):
        raise RuntimeError("campaign mesh_stress snapshot is not executable")
    return snapshot, snapshot_hash


def _persist_shared_executable(
    failure_dir: Path,
    snapshot: Path,
    executable_sha256: str,
) -> Path:
    failure_dir.mkdir(parents=True, exist_ok=True)
    shared = failure_dir / f".mesh_stress-{executable_sha256}"
    if shared.exists():
        if not shared.is_file() or _sha256(shared) != executable_sha256:
            raise RuntimeError("existing shared mesh_stress snapshot hash mismatch")
        return shared

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".mesh_stress-copy-", dir=failure_dir
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        shutil.copy2(snapshot, temporary)
        if _sha256(temporary) != executable_sha256:
            raise RuntimeError("persistent mesh_stress snapshot hash mismatch")
        os.replace(temporary, shared)
    finally:
        temporary.unlink(missing_ok=True)
    return shared


def _link_replay_executable(shared: Path, replay_executable: Path) -> None:
    try:
        os.link(shared, replay_executable)
    except OSError:
        shutil.copy2(shared, replay_executable)


def _sanitizer_environment() -> dict[str, str | None]:
    return {
        name: os.environ.get(name)
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS")
    }


def _source_identity(repo_root: Path) -> dict[str, object]:
    head = subprocess.run(
        ("git", "rev-parse", "HEAD"),
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    status = subprocess.run(
        ("git", "status", "--porcelain", "--untracked-files=no"),
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return {
        "git_head": head.stdout.strip() if head.returncode == 0 else None,
        "tracked_worktree_dirty": status.returncode != 0 or bool(status.stdout),
    }


def _config_fingerprint(
    args: argparse.Namespace,
    result: StressResult,
    executable_sha256: str,
) -> str:
    material = {
        "scenario": args.scenario,
        "seed": result.case.seed,
        "fault_seed": result.case.fault_seed,
        "packets": args.packets,
        "loss": args.loss,
        "ack_loss": args.ack_loss,
        "duplicate": args.duplicate,
        "delay": args.delay,
        "max_delay_us": args.max_delay_us,
        "reset_step": args.reset_step,
        "max_steps": args.max_steps,
        "case_timeout_s": args.case_timeout_s,
        "executable_sha256": executable_sha256,
        "sanitizer_environment": _sanitizer_environment(),
    }
    encoded = json.dumps(material, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()[:16]


def save_failure(
    repo_root: Path,
    original_executable: Path,
    shared_executable: Path,
    executable_sha256: str,
    failure_dir: Path,
    args: argparse.Namespace,
    result: StressResult,
    timestamp: str,
) -> tuple[Path, str, int]:
    fingerprint = _config_fingerprint(args, result, executable_sha256)
    case_dir = failure_dir / (
        f"{timestamp}-{args.scenario}-seed-{result.case.seed:08x}-"
        f"fault-{result.case.fault_seed:08x}-cfg-{fingerprint}"
    )
    case_dir.mkdir(parents=True, exist_ok=False)
    replay_executable = case_dir / "mesh_stress"
    _link_replay_executable(shared_executable, replay_executable)
    if _sha256(replay_executable) != executable_sha256:
        raise RuntimeError("linked mesh_stress snapshot hash mismatch")
    trace_path = case_dir / "trace.jsonl"
    replay_command = command_for(replay_executable, args, result.case, trace_path)
    try:
        completed_replay = subprocess.run(
            replay_command,
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.case_timeout_s,
            check=False,
        )
        replay = StressResult(
            case=result.case,
            command=replay_command,
            returncode=completed_replay.returncode,
            stdout=completed_replay.stdout,
            stderr=completed_replay.stderr,
        )
    except subprocess.TimeoutExpired as exc:
        replay = StressResult(
            case=result.case,
            command=replay_command,
            returncode=124,
            stdout=_timeout_text(exc.stdout),
            stderr=_timeout_text(exc.stderr),
            timed_out=True,
        )

    (case_dir / "first-run.stdout").write_text(result.stdout, encoding="utf-8")
    (case_dir / "first-run.stderr").write_text(result.stderr, encoding="utf-8")
    (case_dir / "replay.stdout").write_text(replay.stdout, encoding="utf-8")
    (case_dir / "replay.stderr").write_text(replay.stderr, encoding="utf-8")
    bounded_replay_command = bounded_command(replay_command, args.case_timeout_s)
    replay_text = shlex.join(bounded_replay_command)
    replay_script = case_dir / "replay.sh"
    sanitizer_environment = _sanitizer_environment()
    environment_lines = [
        "unset ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS",
        *(
            f"export {name}={shlex.quote(value)}"
            for name, value in sanitizer_environment.items()
            if value is not None
        ),
    ]
    replay_script.write_text(
        "#!/bin/sh\nset -eu\n"
        f"expected_sha256={shlex.quote(executable_sha256)}\n"
        f"actual_sha256=$(sha256sum {shlex.quote(str(replay_executable))} | "
        "awk '{print $1}')\n"
        "if [ \"$actual_sha256\" != \"$expected_sha256\" ]; then\n"
        "  echo 'mesh_stress snapshot hash mismatch' >&2\n"
        "  exit 125\n"
        "fi\n"
        + "\n".join(environment_lines)
        + "\n"
        + replay_text
        + "\n",
        encoding="utf-8",
    )
    replay_script.chmod(0o755)
    metadata = {
        "scenario": args.scenario,
        "seed": f"0x{result.case.seed:08x}",
        "fault_seed": f"0x{result.case.fault_seed:08x}",
        "first_returncode": result.returncode,
        "first_timed_out": result.timed_out,
        "replay_returncode": replay.returncode,
        "replay_timed_out": replay.timed_out,
        "replay": replay_text,
        "replay_script": str(replay_script),
        "config_fingerprint": fingerprint,
        "recorded_at_utc": timestamp,
        "executable": {
            "original_path": str(original_executable),
            "shared_snapshot_path": str(shared_executable),
            "snapshot_path": str(replay_executable),
            "sha256": executable_sha256,
        },
        "runner_sha256": _sha256(Path(__file__)),
        "source": _source_identity(repo_root),
        "sanitizer_environment": sanitizer_environment,
        "faults_per_10000": {
            "loss": args.loss,
            "ack_loss": args.ack_loss,
            "duplicate": args.duplicate,
            "delay": args.delay,
        },
        "max_delay_us": args.max_delay_us,
        "reset_step": args.reset_step,
        "max_steps": args.max_steps,
        "packets": args.packets,
        "case_timeout_s": args.case_timeout_s,
    }
    (case_dir / "case.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return case_dir, str(replay_script), replay.returncode


def save_campaign_manifest(
    failure_dir: Path,
    shared_executable: Path,
    executable_sha256: str,
    args: argparse.Namespace,
    failures: list[StressResult],
    retained_case_directories: dict[StressCase, Path],
    timestamp: str,
) -> Path:
    manifest_path = failure_dir / f"{timestamp}-{args.scenario}-campaign.json"
    metadata = {
        "scenario": args.scenario,
        "recorded_at_utc": timestamp,
        "case_timeout_s": args.case_timeout_s,
        "failure_count": len(failures),
        "retained_artifact_count": len(retained_case_directories),
        "max_failure_artifacts": args.max_failure_artifacts,
        "runner_sha256": _sha256(Path(__file__)),
        "source": _source_identity(Path(__file__).resolve().parents[2]),
        "sanitizer_environment": _sanitizer_environment(),
        "shared_executable": {
            "path": str(shared_executable),
            "sha256": executable_sha256,
        },
        "failures": [
            {
                "seed": f"0x{result.case.seed:08x}",
                "fault_seed": f"0x{result.case.fault_seed:08x}",
                "returncode": result.returncode,
                "timed_out": result.timed_out,
                "artifact_directory": (
                    str(retained_case_directories[result.case])
                    if result.case in retained_case_directories
                    else None
                ),
                "replay": shlex.join(
                    bounded_command(
                        command_for(shared_executable, args, result.case),
                        args.case_timeout_s,
                    )
                ),
            }
            for result in failures
        ],
    }
    manifest_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest_path


def main() -> int:
    args = parse_args()
    if args.count == 0:
        print("error: --count must be at least one", file=sys.stderr)
        return 2
    if args.count > UINT32_MASK + 1:
        print("error: --count would repeat 32-bit seeds", file=sys.stderr)
        return 2
    if args.reset_step is not None and args.scenario != "busy-line":
        print("error: --reset-step is valid only for busy-line", file=sys.stderr)
        return 2
    if args.jobs == 0:
        args.jobs = max(1, os.cpu_count() or 1)

    repo_root = Path(__file__).resolve().parents[2]
    build_dir = args.build_dir.expanduser().resolve()
    executable = build_dir / "mesh_stress"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        print(f"error: executable not found: {executable}", file=sys.stderr)
        print("build the mesh_stress target before starting a sweep", file=sys.stderr)
        return 2
    failure_dir = (
        args.failure_dir.expanduser().resolve()
        if args.failure_dir is not None
        else repo_root / "logs" / "mesh-stress-failures"
    )

    cases = make_cases(args)
    try:
        with tempfile.TemporaryDirectory(
            prefix="mesh-stress-campaign-"
        ) as snapshot_directory_name:
            campaign_executable, executable_sha256 = _snapshot_executable(
                executable, Path(snapshot_directory_name)
            )
            print(
                f"mesh stress: scenario={args.scenario} cases={len(cases)} "
                f"jobs={args.jobs} seed_start=0x{cases[0].seed:08x} "
                f"executable_sha256={executable_sha256}"
            )
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=args.jobs
            ) as executor:
                futures = [
                    executor.submit(
                        run_case, repo_root, campaign_executable, args, case
                    )
                    for case in cases
                ]
                results = [future.result() for future in futures]

            failures = [result for result in results if result.returncode != 0]
            if not failures:
                print(f"PASS: {len(results)} deterministic cases")
                return 0

            print(
                f"FAIL: {len(failures)} of {len(results)} cases",
                file=sys.stderr,
            )
            timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
            shared_executable = _persist_shared_executable(
                failure_dir, campaign_executable, executable_sha256
            )
            retained_case_directories: dict[StressCase, Path] = {}
            for result in failures[: args.max_failure_artifacts]:
                case_dir, replay, replay_returncode = save_failure(
                    repo_root,
                    executable,
                    shared_executable,
                    executable_sha256,
                    failure_dir,
                    args,
                    result,
                    timestamp,
                )
                retained_case_directories[result.case] = case_dir
                print(
                    f"seed=0x{result.case.seed:08x} "
                    f"fault_seed=0x{result.case.fault_seed:08x} "
                    f"replay_rc={replay_returncode}",
                    file=sys.stderr,
                )
                print(f"artifacts: {case_dir}", file=sys.stderr)
                print(f"replay: {replay}", file=sys.stderr)

            campaign_manifest = save_campaign_manifest(
                failure_dir,
                shared_executable,
                executable_sha256,
                args,
                failures,
                retained_case_directories,
                timestamp,
            )
            omitted = len(failures) - len(retained_case_directories)
            if omitted > 0:
                print(
                    "failure artifact cap reached: "
                    f"retained={len(retained_case_directories)} "
                    f"omitted={omitted}",
                    file=sys.stderr,
                )
            print(f"campaign: {campaign_manifest}", file=sys.stderr)
            return 1
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
