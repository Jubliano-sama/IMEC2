#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import tempfile


FIRMWARE = Path(__file__).resolve().parents[2]
ROOT = FIRMWARE.parent
WEST = ROOT / ".venv/bin/west"
APP = FIRMWARE / "app/tests/click_event_sequence_persistence"
BUILD = FIRMWARE / "build-click-event-sequence-two-boot-native"
MARKER = re.compile(
    r"CLICK_EVENT_SEQUENCE first=(\d+) second=(\d+) "
    r"block=(\d+) max_boots=(\d+)"
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
        timeout=120,
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

with tempfile.TemporaryDirectory(prefix="imec-click-sequence-") as temp_dir:
    flash = Path(temp_dir) / "flash.bin"
    first_output = run_checked(
        [str(executable), f"-flash={flash}", "-flash_erase"]
    )
    second_output = run_checked([str(executable), f"-flash={flash}"])

first_match = MARKER.search(first_output)
second_match = MARKER.search(second_output)
assert first_match is not None, first_output
assert second_match is not None, second_output

first_boot = tuple(int(value) for value in first_match.groups())
second_boot = tuple(int(value) for value in second_match.groups())
first_id, first_second_id, block_size, max_boots = first_boot
second_id, second_second_id, second_block_size, second_max_boots = second_boot

assert first_id == 0x01000001
assert first_second_id == 0x01000002
assert second_id == first_id + block_size
assert second_second_id == second_id + 1
assert second_block_size == block_size == 256
assert second_max_boots == max_boots == 16_711_679

print(
    "click event sequence survived two native_sim boots: "
    f"{first_id},{first_second_id} -> {second_id},{second_second_id}"
)
