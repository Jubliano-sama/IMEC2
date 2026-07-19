#!/usr/bin/env python3
"""Prove retained failures replay and a fixed liveness seed stays deterministic."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )


def assert_json_lines(path: Path) -> None:
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            json.loads(line)
        except json.JSONDecodeError as error:
            raise AssertionError(
                f"invalid JSONL at {path}:{line_number}: {error}"
            ) from error


def case_directories(failure_dir: Path) -> list[Path]:
    return sorted(
        path
        for path in failure_dir.iterdir()
        if path.is_dir() and (path / "case.json").is_file()
    )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_mesh_stress_replay.py RUNNER MESH_STRESS", file=sys.stderr)
        return 2

    runner = Path(sys.argv[1]).resolve()
    executable = Path(sys.argv[2]).resolve()
    repo_root = runner.parents[2]

    with tempfile.TemporaryDirectory(prefix="mesh-stress-replay-") as temporary:
        failure_dir = Path(temporary) / "failures"
        first = run(
            [
                sys.executable,
                str(runner),
                "--build-dir",
                str(executable.parent),
                "--failure-dir",
                str(failure_dir),
                "--scenario",
                "busy-line",
                "--seed-start",
                "0x5eed",
                "--count",
                "1",
                "--jobs",
                "1",
                "--packets",
                "2",
                "--max-steps",
                "1",
                "--case-timeout-s",
                "5",
            ],
            repo_root,
        )
        if first.returncode != 1:
            print(
                f"failure capture returned {first.returncode}\n{first.stdout}\n{first.stderr}",
                file=sys.stderr,
            )
            return 1

        retained_cases = case_directories(failure_dir)
        if len(retained_cases) != 1:
            print(f"expected one failure artifact, found {len(retained_cases)}", file=sys.stderr)
            return 1
        case_directory = retained_cases[0]
        metadata = json.loads((case_directory / "case.json").read_text(encoding="utf-8"))
        replay_script = case_directory / "replay.sh"
        trace = case_directory / "trace.jsonl"
        snapshot = case_directory / "mesh_stress"
        shared_snapshot = Path(metadata["executable"]["shared_snapshot_path"])
        first_stderr = (case_directory / "first-run.stderr").read_text(
            encoding="utf-8"
        )
        retained_replay_stdout = (case_directory / "replay.stdout").read_text(
            encoding="utf-8"
        )
        retained_replay_stderr = (case_directory / "replay.stderr").read_text(
            encoding="utf-8"
        )
        expected_failure = "status=-1004 last_error=0"

        if (
            metadata["first_returncode"] == 0
            or metadata["replay_returncode"] != metadata["first_returncode"]
            or metadata["executable"]["sha256"] != sha256(snapshot)
            or not shared_snapshot.is_file()
            or shared_snapshot.stat().st_dev != snapshot.stat().st_dev
            or shared_snapshot.stat().st_ino != snapshot.stat().st_ino
            or not replay_script.is_file()
            or not trace.is_file()
            or expected_failure not in first_stderr
            or expected_failure not in retained_replay_stderr
        ):
            print("retained failure metadata is incomplete or inconsistent", file=sys.stderr)
            return 1

        initial_trace_hash = sha256(trace)
        try:
            assert_json_lines(trace)
        except AssertionError as error:
            print(str(error), file=sys.stderr)
            return 1
        trace_summary = json.loads(
            trace.read_text(encoding="utf-8").splitlines()[-1]
        )
        dropped_counters = (
            "connection_event_dropped",
            "rx_window_dropped",
            "transmission_dropped",
            "reception_dropped",
            "trace_dropped",
        )
        if trace_summary.get("type") != "summary" or any(
            counter not in trace_summary for counter in dropped_counters
        ):
            print("trace summary omitted telemetry truncation counters", file=sys.stderr)
            return 1
        for replay_index in range(2):
            replay = run([str(replay_script)], repo_root)
            if replay.returncode != metadata["replay_returncode"]:
                print(
                    f"replay {replay_index} returned {replay.returncode}\n"
                    f"{replay.stdout}\n{replay.stderr}",
                    file=sys.stderr,
                )
                return 1
            if (
                replay.stdout != retained_replay_stdout
                or replay.stderr != retained_replay_stderr
            ):
                print(f"replay {replay_index} changed deterministic output", file=sys.stderr)
                return 1
            if sha256(trace) != initial_trace_hash:
                print(f"replay {replay_index} changed the deterministic trace", file=sys.stderr)
                return 1

        liveness_traces = [Path(temporary) / f"liveness-{index}.jsonl" for index in range(2)]
        liveness_results: list[subprocess.CompletedProcess[str]] = []
        for liveness_trace in liveness_traces:
            liveness_results.append(
                run(
                    [
                        str(executable),
                        "--scenario",
                        "busy-line",
                        "--seed",
                        "0x1089",
                        "--fault-seed",
                        "0xf4174e64",
                        "--packets",
                        "2",
                        "--loss",
                        "300",
                        "--ack-loss",
                        "1200",
                        "--duplicate",
                        "800",
                        "--delay",
                        "2500",
                        "--max-delay-us",
                        "6000",
                        "--max-steps",
                        "800",
                        "--trace",
                        str(liveness_trace),
                    ],
                    repo_root,
                )
            )
        if (
            liveness_results[0].returncode != 0
            or liveness_results[1].returncode != 0
            or liveness_results[1].stdout != liveness_results[0].stdout
            or liveness_results[1].stderr != liveness_results[0].stderr
            or sha256(liveness_traces[0]) != sha256(liveness_traces[1])
        ):
            print(
                "fixed liveness seed did not succeed deterministically\n"
                f"first:\n{liveness_results[0].stdout}\n{liveness_results[0].stderr}\n"
                f"second:\n{liveness_results[1].stdout}\n{liveness_results[1].stderr}",
                file=sys.stderr,
            )
            return 1
        try:
            assert_json_lines(liveness_traces[0])
            assert_json_lines(liveness_traces[1])
        except AssertionError as error:
            print(str(error), file=sys.stderr)
            return 1

        timeout_build = Path(temporary) / "timeout-build"
        timeout_build.mkdir()
        timeout_executable = timeout_build / "mesh_stress"
        timeout_executable.write_text("#!/bin/sh\nsleep 5\n", encoding="utf-8")
        timeout_executable.chmod(0o755)
        timeout_failure_dir = Path(temporary) / "timeout-failures"
        timeout_capture = run(
            [
                sys.executable,
                str(runner),
                "--build-dir",
                str(timeout_build),
                "--failure-dir",
                str(timeout_failure_dir),
                "--scenario",
                "one-hop",
                "--seed-start",
                "0x7000",
                "--count",
                "3",
                "--jobs",
                "1",
                "--case-timeout-s",
                "0.1",
                "--max-failure-artifacts",
                "1",
            ],
            repo_root,
        )
        timeout_cases = case_directories(timeout_failure_dir)
        campaign_manifests = sorted(timeout_failure_dir.glob("*-campaign.json"))
        if (
            timeout_capture.returncode != 1
            or len(timeout_cases) != 1
            or len(campaign_manifests) != 1
            or "retained=1 omitted=2" not in timeout_capture.stderr
        ):
            print(
                "timeout capture or failure artifact cap was inconsistent\n"
                f"{timeout_capture.stdout}\n{timeout_capture.stderr}",
                file=sys.stderr,
            )
            return 1
        timeout_metadata = json.loads(
            (timeout_cases[0] / "case.json").read_text(encoding="utf-8")
        )
        campaign_metadata = json.loads(
            campaign_manifests[0].read_text(encoding="utf-8")
        )
        timeout_replay = run([str(timeout_cases[0] / "replay.sh")], repo_root)
        if (
            timeout_metadata["first_returncode"] != 124
            or not timeout_metadata["first_timed_out"]
            or timeout_metadata["replay_returncode"] != 124
            or not timeout_metadata["replay_timed_out"]
            or timeout_replay.returncode != 124
            or campaign_metadata["failure_count"] != 3
            or campaign_metadata["retained_artifact_count"] != 1
            or campaign_metadata["sanitizer_environment"]
            != timeout_metadata["sanitizer_environment"]
            or any(
                not failure["replay"].startswith("timeout ")
                for failure in campaign_metadata["failures"]
            )
        ):
            print("timeout replay was not bounded or exactly recorded", file=sys.stderr)
            return 1

    print("mesh stress failure artifacts replay exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
