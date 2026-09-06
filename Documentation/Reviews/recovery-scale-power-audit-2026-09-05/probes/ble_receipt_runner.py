#!/usr/bin/env python3
"""Run the exact current BLE RX worker with bounded fake queue/transport callbacks."""

import hashlib
import json
import pathlib
import subprocess
import tempfile
import time


def main():
    here = pathlib.Path(__file__).parent
    repo = here.parents[3]
    source = repo / "firmware/app/src/app_gateway_ble.c"
    original = source.read_text()
    signature = "static void gateway_ble_rx_work_handler(struct k_work *work)\n{"
    start = original.index(signature)
    end = original.index("\n}\n", start) + 3
    function = original[start:end]
    extracted = here / "ble_receipt_worker.inc"
    extracted.write_text(function)
    report = {
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo,
                                        text=True).strip(),
        "source": str(source.relative_to(repo)),
        "source_line": original[:start].count("\n") + 1,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "worker_sha256": hashlib.sha256(function.encode()).hexdigest(),
        "fixture_sha256": hashlib.sha256((here / "ble_receipt_probe.c").read_bytes()).hexdigest(),
        "deadline_seconds": 1,
        "cases": [],
    }
    cases = ("receipt_first_no_credit", "command_first_one_credit",
             "command_only_no_credit", "command_first_no_credit",
             "command_first_then_reorder")
    with tempfile.TemporaryDirectory(prefix="imec-ble-receipt-") as work:
        binary = pathlib.Path(work) / "probe"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
             "-I", str(repo / "firmware/include"), str(here / "ble_receipt_probe.c"),
             "-o", str(binary)], check=True, timeout=10)
        for case in cases:
            started = time.monotonic()
            try:
                result = subprocess.run([str(binary), case], capture_output=True,
                                        text=True, check=False, timeout=1)
                item = {"case": case, "returncode": result.returncode,
                        "stdout": result.stdout, "stderr": result.stderr,
                        "status": "PASS" if result.returncode == 0 else "FAIL"}
            except subprocess.TimeoutExpired as error:
                item = {"case": case, "returncode": None, "status": "TIMEOUT",
                        "stdout": (error.stdout or b"").decode(errors="replace"),
                        "stderr": (error.stderr or b"").decode(errors="replace")}
            item["elapsed_seconds"] = round(time.monotonic() - started, 6)
            report["cases"].append(item)
            print(f'{case}: {item["status"]} ({item["elapsed_seconds"]:.6f}s)')
            print(item["stdout"], end="")
    report["contract_passed"] = all(item["status"] == "PASS" for item in report["cases"])
    (here / "ble_receipt_results.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["contract_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
