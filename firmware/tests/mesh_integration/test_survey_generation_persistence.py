#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile


FIRMWARE = Path(__file__).resolve().parents[2]
ROOT = FIRMWARE.parent
WEST = ROOT / ".venv/bin/west"
CASES = (
    (
        "gateway",
        FIRMWARE / "app/tests/mesh_persistence",
        FIRMWARE / "build-mesh-persistence-survey-generation-gateway",
        "mesh_persistence::"
        "test_gateway_survey_generation_reservation_and_corruption",
    ),
    (
        "anchor",
        FIRMWARE / "app/tests/mesh_persistence_anchor",
        FIRMWARE / "build-mesh-persistence-survey-generation-anchor",
        "mesh_persistence_anchor::"
        "test_anchor_generation_high_water_and_corruption",
    ),
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


for role, app, build, test_name in CASES:
    run_checked(
        [
            str(WEST),
            "build",
            "--no-sysbuild",
            "--pristine=always",
            "-s",
            str(app),
            "-b",
            "native_sim/native/64",
            "--build-dir",
            str(build),
        ],
        240,
    )
    executable = build / "zephyr/zephyr.exe"
    assert executable.exists(), f"missing {role} native_sim executable"
    with tempfile.TemporaryDirectory(
        prefix=f"imec-survey-generation-{role}-"
    ) as temp_dir:
        output = run_checked(
            [
                str(executable),
                f"-test={test_name}",
                f"-flash={Path(temp_dir) / 'flash.bin'}",
                "-flash_erase",
            ],
            60,
        )
    assert "PROJECT EXECUTION SUCCESSFUL" in output, output

print("gateway reservation and anchor survey-generation high-water persistence passed")
