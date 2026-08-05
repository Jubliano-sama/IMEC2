#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile


FIRMWARE = Path(__file__).resolve().parents[2]
ROOT = FIRMWARE.parent
WEST = ROOT / ".venv/bin/west"
APP = FIRMWARE / "app/tests/mesh_persistence"
BUILD = FIRMWARE / "build-mesh-persistence-nvs-idempotence"
TEST = (
    "mesh_persistence::"
    "test_active_collection_retry_outbox_round_trip"
)


def run_checked(command: list[str], timeout: int) -> str:
    environment = os.environ.copy()
    environment["CCACHE_DISABLE"] = "1"
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"{completed.stdout}"
        )
    return completed.stdout


run_checked(
    [
        str(WEST),
        "build",
        "--no-sysbuild",
        "--pristine=always",
        "-s",
        str(APP),
        "-b",
        "native_sim/native/64",
        "--build-dir",
        str(BUILD),
        "--",
        f"-DEXTRA_CONF_FILE={APP / 'prj-no-data-crc.conf'}",
    ],
    180,
)

executable = BUILD / "zephyr/zephyr.exe"
assert executable.exists(), f"missing native_sim executable: {executable}"

with tempfile.TemporaryDirectory(prefix="imec-nvs-idempotence-") as temp_dir:
    flash = Path(temp_dir) / "flash.bin"
    output = run_checked(
        [
            str(executable),
            f"-test={TEST}",
            f"-flash={flash}",
            "-flash_erase",
        ],
        60,
    )

assert "PROJECT EXECUTION SUCCESSFUL" in output, output
print("production-config NVS duplicate write accepted as durable success")
