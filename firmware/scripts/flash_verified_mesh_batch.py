#!/usr/bin/env python3
"""Flash one production mesh role to several probes concurrently.

The single-target flasher remains the transaction authority. This wrapper
verifies one immutable build/cohort, holds its deployment lock once, then runs
the independent per-probe transactions in parallel. ``--factory-new`` is an
explicit destructive assertion: any journal belongs to a replaced target,
and each selected chip may be mass-erased to unlock APPROTECT.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, TypeVar

import flash_verified_mesh as flash


ROLE_BUILDS = {
    "anchor": "mesh-anchor",
    "clicker": "mesh-clicker",
}
_ERROR_RE = re.compile(r"(?:^|\n)\s*(?:Error|Traceback)\b", re.IGNORECASE)
T = TypeVar("T")


@dataclass(frozen=True)
class ProbeResult:
    probe_id: str
    ok: bool
    detail: str
    elapsed_seconds: float


def _build_directory(role: str) -> Path:
    return flash.REPO_ROOT / "build" / ROLE_BUILDS[role]


def _parallel(
    probe_ids: list[str],
    jobs: int,
    operation: Callable[[str], T],
) -> tuple[dict[str, T], list[ProbeResult]]:
    values: dict[str, T] = {}
    failures: list[ProbeResult] = []

    def invoke(probe_id: str) -> tuple[str, T | None, ProbeResult]:
        started = time.monotonic()
        try:
            value = operation(probe_id)
        except Exception as exc:  # Each target keeps its own durable journal.
            result = ProbeResult(
                probe_id, False, str(exc), time.monotonic() - started,
            )
            return probe_id, None, result
        result = ProbeResult(probe_id, True, "ok", time.monotonic() - started)
        return probe_id, value, result

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = [executor.submit(invoke, probe_id) for probe_id in probe_ids]
        for future in concurrent.futures.as_completed(futures):
            probe_id, value, result = future.result()
            state = "OK" if result.ok else "FAILED"
            print(
                f"BATCH_PROBE_{state} probe={probe_id} "
                f"elapsed={result.elapsed_seconds:.1f}s detail={result.detail}",
                flush=True,
            )
            if result.ok:
                assert value is not None
                values[probe_id] = value
            else:
                failures.append(result)
    return values, failures


def _mass_unlock(probe_id: str) -> bool:
    command = [
        str(flash.PYOCD_EXECUTABLE), "erase", "--mass",
        "-t", flash.TARGET_NAME,
        "-u", probe_id,
        "-f", str(flash.FLASH_FREQUENCY_HZ),
    ]
    result = flash._run(command, capture_output=True)
    combined = f"{result.stdout}\n{result.stderr}"
    if result.returncode or _ERROR_RE.search(combined):
        detail = result.stderr.strip() or result.stdout.strip()
        raise flash.TransactionError(
            f"factory-new unlock failed with exit status "
            f"{result.returncode}: {detail}"
        )
    return True


def _build_once(build_dir: Path) -> None:
    result = subprocess.run(
        ["cmake", "--build", str(build_dir), "-j4"],
        check=False,
        cwd=flash.REPO_ROOT,
    )
    if result.returncode:
        raise flash.TransactionError(
            f"production build failed with exit status {result.returncode}"
        )


def _verify_batch(
    build_dir: Path,
    probe_ids: list[str],
) -> tuple[object, dict[str, object]]:
    build, issues = flash._verify_stage_candidate(build_dir, probe_ids[0])
    if build is not None:
        issues.extend(flash._storage_initialization_issues(build))
    for probe_id in probe_ids[1:]:
        try:
            flash._probe_is_visible(probe_id)
        except flash.TransactionError as exc:
            issues.append(str(exc))
    if issues or build is None:
        joined = "\n".join(f"  {issue}" for issue in issues)
        raise flash.TransactionError(f"verified batch staging blocked:\n{joined}")
    cohort_binding = flash._resolve_cohort(build_dir, None)
    return build, cohort_binding


def _archive_replaced_targets(probe_ids: list[str]) -> None:
    for probe_id in probe_ids:
        data = flash._load_journal(probe_id)
        if data is None:
            continue
        if data.get("state") != "awaiting_qualification":
            raise flash.TransactionError(
                f"probe {probe_id} has an interrupted old-target transaction; "
                "resolve it before factory-new batch flashing"
            )
        flash._abandon_staged_candidate(data)


def _require_empty_journals(probe_ids: list[str]) -> None:
    active = [probe_id for probe_id in probe_ids if flash._load_journal(probe_id)]
    if active:
        raise flash.TransactionError(
            "selected probes already have candidates awaiting qualification: "
            + ", ".join(active)
        )


def run_batch(
    role: str,
    probe_ids: list[str],
    *,
    factory_new: bool,
    jobs: int,
) -> int:
    build_dir = _build_directory(role)
    started = time.monotonic()
    _build_once(build_dir)

    with flash._ledger_lock():
        build, cohort_binding = _verify_batch(build_dir, probe_ids)
        if factory_new:
            _archive_replaced_targets(probe_ids)
            _, unlock_failures = _parallel(probe_ids, jobs, _mass_unlock)
            failed_unlocks = {result.probe_id for result in unlock_failures}
            stage_probes = [
                probe_id for probe_id in probe_ids
                if probe_id not in failed_unlocks
            ]
        else:
            _require_empty_journals(probe_ids)
            unlock_failures = []
            stage_probes = probe_ids

        def stage(probe_id: str) -> bool:
            flash._stage_for_qualification(
                build, build_dir, probe_id, cohort_binding,
                bench_only=False,
                initialize_storage=factory_new,
            )
            return True

        _, stage_failures = _parallel(stage_probes, jobs, stage)

    failures = [*unlock_failures, *stage_failures]
    elapsed = time.monotonic() - started
    if failures:
        failed = ",".join(result.probe_id for result in failures)
        print(
            f"BATCH_FLASH_FAILED role={role} total={len(probe_ids)} "
            f"failed={failed} elapsed={elapsed:.1f}s",
            file=sys.stderr,
        )
        return 1
    print(
        f"BATCH_FLASH_OK role={role} count={len(probe_ids)} "
        f"elapsed={elapsed:.1f}s",
        flush=True,
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Concurrently stage one production role on multiple probes.",
    )
    parser.add_argument("--role", choices=sorted(ROLE_BUILDS), required=True)
    parser.add_argument(
        "--probe-id", action="append", required=True,
        help="Exact pyOCD probe ID; repeat for every target.",
    )
    parser.add_argument(
        "--factory-new", action="store_true",
        help=(
            "Confirm the selected probes now hold replaceable factory-new "
            "targets; abandon old-target journals, mass-unlock, and initialize "
            "durable storage."
        ),
    )
    parser.add_argument(
        "--jobs", type=int, default=0,
        help="Maximum concurrent probes; defaults to the selected probe count.",
    )
    args = parser.parse_args(argv)
    if len(set(args.probe_id)) != len(args.probe_id):
        parser.error("--probe-id values must be unique")
    if args.jobs < 0:
        parser.error("--jobs must be positive or zero for automatic")
    args.jobs = min(args.jobs or len(args.probe_id), len(args.probe_id))
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return run_batch(
            args.role,
            args.probe_id,
            factory_new=args.factory_new,
            jobs=args.jobs,
        )
    except (OSError, flash.TransactionError) as exc:
        print(f"verified batch flashing failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
