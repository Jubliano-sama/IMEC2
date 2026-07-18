#!/usr/bin/env python3
"""The production anchor is one hardware-identified deployment artifact."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
APP_SOURCE = REPO_ROOT / "firmware" / "app" / "CMakeLists.txt"
ANCHOR_RADIO_SOURCE = (
    REPO_ROOT / "firmware" / "app" / "src" / "app_anchor_radio.inc"
)


class MeshAnchorPresetTests(unittest.TestCase):
    def test_only_single_anchor_preset_is_listed(self) -> None:
        source = APP_SOURCE.read_text(encoding="utf-8")

        self.assertIn('IMEC_BUILD_PRESET STREQUAL "mesh_anchor"', source)
        self.assertIn('IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop"', source)
        for index in range(1, 6):
            self.assertNotIn(f"mesh_anchor_{index}", source)

    def test_numbered_anchor_presets_fail_before_zephyr_configuration(self) -> None:
        for index in range(1, 6):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as tempdir:
                result = subprocess.run(
                    [
                        "cmake",
                        "-S",
                        str(REPO_ROOT / "firmware" / "app"),
                        "-B",
                        tempdir,
                        f"-DIMEC_BUILD_PRESET=mesh_anchor_{index}",
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertNotEqual(0, result.returncode)
                self.assertIn("Unknown IMEC_BUILD_PRESET", result.stdout + result.stderr)

    def test_anchor_preset_cannot_accept_a_compile_time_device_id(self) -> None:
        source = APP_SOURCE.read_text(encoding="utf-8")
        branch = source.split('IMEC_BUILD_PRESET STREQUAL "mesh_anchor"', 1)[1]
        branch = branch.split("else()", 1)[0]

        self.assertIn('set(IMEC_DEVICE_ID "" CACHE STRING', branch)
        self.assertIn("set(IMEC_USE_HARDWARE_ANCHOR_ID ON)", branch)
        self.assertNotIn("MESH_ROUTE_TEST_ANCHOR_INDEX", branch)

    def test_forcedhop_anchor_is_bench_only_and_hardware_identified(self) -> None:
        source = APP_SOURCE.read_text(encoding="utf-8")

        self.assertIn(
            'IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop"', source
        )
        self.assertIn(
            '"CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ=y\\n"',
            source,
        )
        self.assertIn(
            '"CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL=y\\n"',
            source,
        )

    def test_forcedhop_direct_gateway_wake_is_rejected_before_handoff(self) -> None:
        source = ANCHOR_RADIO_SOURCE.read_text(encoding="utf-8")
        guard = source.index("!app_mesh_c5_route_wake_claim_allowed(")
        reject = source.index("goto scan_complete;", guard)
        handoff = source.index("route_wake_handoff = true;", guard)

        self.assertLess(guard, reject)
        self.assertLess(reject, handoff)

if __name__ == "__main__":
    unittest.main()
