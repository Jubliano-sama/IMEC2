#!/usr/bin/env python3
"""Compile production application seams; record observations without hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=HERE / "application_results.json")
    args = parser.parse_args()
    build = args.build.resolve()
    sources = {
        "anchor_restart": [],
        "c5_bank_recovery": [
            "firmware/app/src/app_wake_train_politeness.c",
            "firmware/app/src/app_mesh_ch9_ack.c",
            "firmware/app/src/app_gateway_command_observability.c",
            "firmware/app/src/app_gateway_ble_stream.c",
        ],
        "survey_scale": [],
    }
    evidence = {
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "library_sha256": hashlib.sha256((build / "libcore.a").read_bytes()).hexdigest(),
        "method": "Existing production seams with fake clocks, locks, queues and radio; observations are not hardware qualification.",
        "cases": [],
    }
    with tempfile.TemporaryDirectory(prefix="imec-app-audit-") as temporary:
        for name, extra in sources.items():
            source = HERE / f"{name}.c"
            binary = Path(temporary) / name
            command = ["cc", "-std=c11", "-Ifirmware/include", "-Ifirmware/app/src",
                       f"-I{build}", str(source), *extra, str(build / "libcore.a"),
                       "-lm", "-pthread", "-o", str(binary)]
            subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True, timeout=2)
            evidence["cases"].append({"name": name, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                      "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
            print(name + ":\n" + result.stdout, end="")
            result.check_returncode()
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
