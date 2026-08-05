#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import tempfile


FIRMWARE = Path(__file__).resolve().parents[2]
ROOT = FIRMWARE.parent
WEST = ROOT / ".venv/bin/west"
APP = FIRMWARE / "app/tests/clicker_shared_nvs_persistence"
BUILD = FIRMWARE / "build-clicker-shared-nvs-four-boot-native"
MARKER = re.compile(
    r"SHARED_NVS boot=(\d+) first=(\d+) second=(\d+) "
    r"state=(original|confirm|terminal|empty)"
)


def run_checked(command: list[str]) -> str:
    environment = os.environ.copy()
    environment["CCACHE_DISABLE"] = "1"
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=180,
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
    ]
)
executable = BUILD / "zephyr/zephyr.exe"
assert executable.exists(), f"missing native_sim executable: {executable}"

with tempfile.TemporaryDirectory(prefix="imec-clicker-shared-nvs-") as temp_dir:
    flash = Path(temp_dir) / "flash.bin"
    outputs = [
        run_checked(
            [str(executable), f"-flash={flash}", "-flash_erase"]
            if boot == 0
            else [str(executable), f"-flash={flash}"]
        )
        for boot in range(4)
    ]

matches = [MARKER.search(output) for output in outputs]
assert all(match is not None for match in matches), "\n".join(outputs)
records = [tuple(match.groups()) for match in matches if match is not None]
states = ["original", "confirm", "terminal", "empty"]
block = 256
floor = 0x01000000
for boot, record in enumerate(records):
    boot_text, first_text, second_text, state = record
    first = int(first_text)
    second = int(second_text)
    assert int(boot_text) == boot
    assert first == floor + 1 + boot * block
    assert second == first + 1
    assert state == states[boot]

print(
    "shared clicker NVS survived sequence/original/confirm/terminal "
    "interleaving across four native_sim boots"
)
