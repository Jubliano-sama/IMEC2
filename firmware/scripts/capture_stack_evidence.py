#!/usr/bin/env python3
"""Create trusted-local stack evidence through the fixed pyOCD RTT workflow.

This tool is a qualification capture tool, not a deployment flasher. It never
programs hardware. Final programming of deployable mesh images remains solely
the responsibility of ``flash_verified_mesh.py``. The forced-hop anchor can be
captured only with ``--bench-only`` and that evidence is explicitly marked as
non-promotable.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import artifact_cohort as cohort
import verify_stack_evidence as verifier


REPO_ROOT = Path(__file__).resolve().parents[2]
COHORT_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "cohorts"


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _utc_text(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _probe_is_visible(probe_id: str) -> None:
    result = subprocess.run(["pyocd", "list", "--json"], capture_output=True, text=True, check=False)
    if result.returncode == 0:
        try:
            devices = json.loads(result.stdout)
        except ValueError as exc:
            raise verifier.EvidenceError(
                "pyOCD probe enumeration did not produce JSON"
            ) from exc
        visible = isinstance(devices, list) and any(
            isinstance(device, dict) and probe_id in {
                str(device.get("unique_id", "")),
                str(device.get("uid", "")),
                str(device.get("id", "")),
            }
            for device in devices
        )
    else:
        plain = subprocess.run(["pyocd", "list"], capture_output=True,
                               text=True, check=False)
        if plain.returncode:
            raise verifier.EvidenceError(
                f"cannot enumerate pyOCD probes: {plain.stderr.strip()}"
            )
        visible = any(probe_id in line.split()
                      for line in plain.stdout.splitlines())
    if not visible:
        raise verifier.EvidenceError(f"requested probe {probe_id} is not attached")


def _run_rtt(probe_id: str, transcript: Path, duration_seconds: int) -> tuple[datetime, datetime]:
    rtt_command = [
        "pyocd", "rtt", "-t", "nrf52833", "-M", "pre-reset",
        "-a", "0x20000410", "-s", "0x100",
        "-u", probe_id, "--up-channel-id", "0",
    ]
    command = [
        "script", "-q", "-f", "-c",
        shlex.join(["timeout", "--foreground", "--signal=INT", str(duration_seconds), *rtt_command]),
        str(transcript),
    ]
    started = _utc_now()
    result = subprocess.run(command, check=False)
    ended = _utc_now()
    if result.returncode not in {0, 124, 130}:
        raise verifier.EvidenceError(f"pyOCD RTT capture failed with exit status {result.returncode}")
    if not transcript.is_file() or transcript.stat().st_size == 0:
        raise verifier.EvidenceError("pyOCD RTT capture produced no transcript")
    return started, ended


def _read_target_flash(probe_id: str, destination: Path) -> None:
    destination.unlink(missing_ok=True)
    command = [
        "pyocd", "commander", "--no-config", "-t", "nrf52833",
        "-u", probe_id, "-f", "4000000", "-M", "halt",
        "-c", f"savemem 0x0 0x{cohort.FLASH_SIZE:x} {shlex.quote(str(destination))}",
        "-c", "reset",
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise verifier.EvidenceError(
            f"target flash readback failed with exit status {result.returncode}: {detail}"
        )
    if not destination.is_file() or destination.stat().st_size != cohort.FLASH_SIZE:
        actual = destination.stat().st_size if destination.is_file() else 0
        raise verifier.EvidenceError(
            f"target flash readback has {actual} bytes, expected {cohort.FLASH_SIZE}"
        )


def _resolve_cohort(build_dir: Path, manifest: Path | None) -> dict[str, object]:
    selected = manifest
    if selected is None:
        selected = cohort.create_manifest(REPO_ROOT, [build_dir], COHORT_DIRECTORY)
    return cohort.validate_build(selected, REPO_ROOT, build_dir)


def capture(
    build_dir: Path,
    probe_id: str,
    output_dir: Path,
    duration_seconds: int,
    cohort_manifest: Path | None = None,
    bench_only: bool = False,
) -> Path:
    policies, frame_limit = verifier.load_policy()
    build = verifier.verify_build(
        build_dir,
        policies,
        frame_limit,
        allow_watchdog_bypass=True,
    )
    if build.issues:
        raise verifier.EvidenceError("exact build is not eligible for capture: " + "; ".join(build.issues))
    policy = policies.get(build.preset)
    deployable = (
        policy is not None and policy.deployable
        and build.preset in verifier.DEPLOYABLE_PRESETS
    )
    bench_preset = build.preset in verifier.WATCHDOG_BYPASS_BENCH_PRESETS
    if bench_only and (not bench_preset or policy is None):
        raise verifier.EvidenceError(
            f"{build.preset!r} is not an allowed bench-only capture target"
        )
    if not bench_only and not deployable:
        suffix = "; pass --bench-only" if bench_preset else ""
        raise verifier.EvidenceError(
            f"{build.preset!r} is not a deployable capture target{suffix}"
        )
    assert policy is not None
    cohort_binding = _resolve_cohort(build_dir, cohort_manifest)
    _probe_is_visible(probe_id)
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output_dir, prefix=f".{build.preset}-capture-") as temporary:
        tempdir = Path(temporary)
        artifact = cohort_binding.get("artifact")
        if not isinstance(artifact, dict):
            raise verifier.EvidenceError("cohort artifact binding is invalid")
        before_readback = tempdir / "target-flash-before.bin"
        before_started = _utc_now()
        _read_target_flash(probe_id, before_readback)
        before_ended = _utc_now()
        before_identity = cohort.target_readback_identity(
            before_readback, artifact,
        )
        transcript = tempdir / "rtt.typescript"
        started, ended = _run_rtt(probe_id, transcript, duration_seconds)
        target_identity = cohort.target_identity_from_transcript(
            transcript.read_text(encoding="utf-8", errors="replace"),
            build.preset,
        )
        after_readback = tempdir / "target-flash-after.bin"
        after_started = _utc_now()
        _read_target_flash(probe_id, after_readback)
        after_ended = _utc_now()
        after_identity = cohort.target_readback_identity(
            after_readback, artifact,
        )
        if (
            before_identity["code_sector_map_sha256"]
            != after_identity["code_sector_map_sha256"]
        ):
            raise verifier.EvidenceError(
                "target code identity changed during the RTT capture"
            )
        transcript_name = f"{build.preset}-{ended.strftime('%Y%m%dT%H%M%SZ')}.typescript"
        final_transcript = output_dir / transcript_name
        transcript.replace(final_transcript)
    provenance = {
        "tool": verifier.CAPTURE_TOOL_RELATIVE,
        "tool_sha256": verifier._sha256(Path(__file__)),
        "workflow": verifier.CAPTURE_WORKFLOW,
        "rtt_command": [
            "pyocd", "rtt", "-t", "nrf52833", "-M", "pre-reset",
            "-a", "0x20000410", "-s", "0x100",
            "-u", probe_id, "--up-channel-id", "0",
        ],
        "tty_wrapper": "script",
        "started_at_utc": _utc_text(started),
        "ended_at_utc": _utc_text(ended),
    }
    data = {
        "schema": verifier.CAPTURE_SCHEMA,
        "preset": build.preset,
        "probe_id": probe_id,
        "artifact": {
            "elf_sha256": build.elf_sha256,
            "hex_sha256": build.hex_sha256,
            "artifact_id": cohort_binding["artifact_id"],
        },
        "target": {
            "preset": build.preset,
            "build_identity": build.build_identity,
            "flash_sha256": after_identity["flash_sha256"],
            "pre_capture_flash_sha256": before_identity["flash_sha256"],
            "post_capture_flash_sha256": after_identity["flash_sha256"],
            "code_sector_map_sha256": after_identity["code_sector_map_sha256"],
            "pre_readback_started_at_utc": _utc_text(before_started),
            "pre_readback_completed_at_utc": _utc_text(before_ended),
            "post_readback_started_at_utc": _utc_text(after_started),
            "post_readback_completed_at_utc": _utc_text(after_ended),
            **target_identity,
        },
        "cohort": {
            "manifest_path": cohort_binding["manifest_path"],
            "cohort_id": cohort_binding["cohort_id"],
            "source_id": cohort_binding["source_id"],
            "artifact_id": cohort_binding["artifact_id"],
        },
        "transcript": {"path": transcript_name, "sha256": verifier._sha256(final_transcript)},
        "provenance": provenance,
        "evidence_mode": "bench_only" if bench_only else "production_candidate",
        "promotion_allowed": not bench_only,
    }
    data["capture_id"] = verifier._capture_id(data)
    data["cohort_capture_id"] = cohort.capture_binding_id(data)
    manifest = output_dir / f"{build.preset}-{data['capture_id'][:16]}.json"
    temporary_manifest = manifest.with_suffix(".json.tmp")
    temporary_manifest.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary_manifest.replace(manifest)
    if bench_only:
        result = verifier._load_hardware_manifest(manifest, build, policy)
        failures = list(result.issues)
    else:
        results, issues = verifier.verify_hardware(
            [manifest], [build], policies, True, {build.preset},
        )
        failures = issues + [
            issue for result in results for issue in result.issues
        ]
    if failures:
        manifest.unlink(missing_ok=True)
        raise verifier.EvidenceError("captured transcript was rejected: " + "; ".join(failures))
    return manifest


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--probe-id", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--cohort-manifest", type=Path)
    parser.add_argument("--bench-only", action="store_true")
    parser.add_argument("--duration-seconds", type=int, default=300)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.duration_seconds < 1 or args.duration_seconds > int(verifier.MAX_CAPTURE_DURATION.total_seconds()):
        print("capture blocked: duration must be between 1 and 900 seconds", file=sys.stderr)
        return 1
    try:
        manifest = capture(
            args.build_dir,
            args.probe_id,
            args.output_dir,
            args.duration_seconds,
            args.cohort_manifest,
            args.bench_only,
        )
    except (OSError, cohort.CohortError, verifier.EvidenceError) as exc:
        print(f"capture blocked: {exc}", file=sys.stderr)
        return 1
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
