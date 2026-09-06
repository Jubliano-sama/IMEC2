#!/usr/bin/env python3
"""Run unmodified survey core probes with one short subprocess deadline each.

Exit 0 means all completion/atomicity contracts passed; exit 1 records failures.
This deliberately remains an audit probe, outside the production test suite.
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile
import time


def decoded(value):
    return value.decode(errors="replace") if isinstance(value, bytes) else value or ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=pathlib.Path, required=True)
    parser.add_argument("--deadline-seconds", type=float, default=1.0)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.deadline_seconds <= 0 or args.deadline_seconds > 10:
        parser.error("deadline must be positive and no more than 10 seconds")
    source = pathlib.Path(__file__).with_name("survey_lane_probe.c")
    repo = source.parents[4]
    report = {
        "head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "library": str(args.library.resolve()),
        "library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "deadline_seconds": args.deadline_seconds,
        "cases": [],
    }
    cases = ("compact30", "sparse30", "pairs100_baseline", "pairs100", "full50",
             "merge_baseline", "merge_conflict", "replay_ack_new_generation",
             "replay_ack_same_generation")
    with tempfile.TemporaryDirectory(prefix="imec-survey-lane-probe-") as work:
        binary = pathlib.Path(work) / "survey_lane_probe"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
             "-I", str(repo / "firmware/include"), str(source),
             str(args.library.resolve()), "-lm", "-o", str(binary)], check=True)
        for case in cases:
            started = time.monotonic()
            try:
                result = subprocess.run(
                    [str(binary), case], capture_output=True, text=True,
                    timeout=args.deadline_seconds, check=False)
                item = {"case": case, "status": "PASS" if result.returncode == 0 else "FAIL",
                        "returncode": result.returncode, "stdout": result.stdout,
                        "stderr": result.stderr}
            except subprocess.TimeoutExpired as error:
                item = {"case": case, "status": "TIMEOUT", "returncode": None,
                        "stdout": decoded(error.stdout), "stderr": decoded(error.stderr)}
            item["elapsed_seconds"] = round(time.monotonic() - started, 6)
            report["cases"].append(item)
            print(f'{case}: {item["status"]} ({item["elapsed_seconds"]:.6f}s)', flush=True)
            print(item["stdout"], end="", flush=True)
            if item["stderr"]:
                print(item["stderr"], end="", flush=True)
    report["contract_passed"] = all(item["status"] == "PASS" for item in report["cases"])
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["contract_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
