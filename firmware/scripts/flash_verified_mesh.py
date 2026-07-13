#!/usr/bin/env python3
"""The only deployment flasher for production-candidate mesh presets."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

import verify_stack_evidence as verifier

REPO_ROOT = Path(__file__).resolve().parents[2]
FLASH_FREQUENCY_HZ = 4_000_000
WEST_EXECUTABLE = REPO_ROOT / ".venv" / "bin" / "west"
CAPTURE_LEDGER = REPO_ROOT / "logs" / "stack-evidence" / "verified-capture-ledger.jsonl"


@contextmanager
def _ledger_lock(path: Path | None = None):
    path = path or CAPTURE_LEDGER
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+", encoding="utf-8") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def _consumed_capture_ids(path: Path | None = None) -> set[str]:
    path = path or CAPTURE_LEDGER
    if not path.is_file():
        return set()
    values: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            item = json.loads(line)
            capture_id = item.get("capture_id") if isinstance(item, dict) else None
            if isinstance(capture_id, str):
                values.add(capture_id)
        except ValueError:
            continue
    return values


def _record_consumed_capture(capture_id: str, preset: str, probe_id: str, path: Path | None = None) -> None:
    path = path or CAPTURE_LEDGER
    path.parent.mkdir(parents=True, exist_ok=True)
    record = json.dumps({"capture_id": capture_id, "preset": preset, "probe_id": probe_id}, sort_keys=True)
    with path.open("a", encoding="utf-8") as ledger:
        ledger.write(record + "\n")


def verify_flash(build_dir: Path, manifest: Path, probe_id: str) -> tuple[verifier.BuildEvidence | None, str | None, list[str]]:
    try:
        policies, frame_limit = verifier.load_policy()
    except verifier.EvidenceError as exc:
        return None, None, [str(exc)]
    build = verifier.verify_build(build_dir, policies, frame_limit)
    issues = list(build.issues)
    policy = policies.get(build.preset)
    if build.preset not in verifier.DEPLOYABLE_PRESETS or policy is None or not policy.deployable:
        issues.append(f"{build.preset or '<missing>'} is not an allowed deployment preset")
    results, hardware_issues = verifier.verify_hardware(
        [manifest], [build], policies, True, {build.preset}, _consumed_capture_ids()
    )
    issues.extend(hardware_issues)
    capture_id: str | None = None
    for result in results:
        issues.extend(result.issues)
        if result.probe_id and result.probe_id != probe_id:
            issues.append(f"trusted capture probe {result.probe_id} does not match selected probe {probe_id}")
        capture_id = result.capture_id
    if not results:
        issues.append("trusted capture is missing for the exact deployment preset")
    return build, capture_id, issues


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--hardware-manifest", required=True, type=Path)
    parser.add_argument("--probe-id", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        with _ledger_lock():
            build, capture_id, issues = verify_flash(args.build_dir, args.hardware_manifest, args.probe_id)
            if issues or build is None or capture_id is None:
                print("verified deployment flash blocked:", file=sys.stderr)
                for issue in issues:
                    print(f"  {issue}", file=sys.stderr)
                return 1
            if not WEST_EXECUTABLE.is_file():
                print(f"verified deployment flash blocked: repository west is missing: {WEST_EXECUTABLE}", file=sys.stderr)
                return 1
            command = [
                str(WEST_EXECUTABLE), "flash", "--runner", "pyocd", "--build-dir", str(args.build_dir),
                "--", "--dev-id", args.probe_id, "--frequency", str(FLASH_FREQUENCY_HZ),
            ]
            result = subprocess.run(command, check=False)
            if result.returncode:
                print(f"verified deployment flash failed with exit status {result.returncode}", file=sys.stderr)
                return result.returncode
            _record_consumed_capture(capture_id, build.preset, args.probe_id)
    except OSError as exc:
        print(f"verified deployment flash failed to start: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
