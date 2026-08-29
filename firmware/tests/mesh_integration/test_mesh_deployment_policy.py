#!/usr/bin/env python3
"""Negative coverage for repository deployment-path enforcement."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "firmware" / "scripts" / "check_mesh_deployment_policy.py"
SPEC = importlib.util.spec_from_file_location("mesh_deployment_policy", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
policy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(policy)


class MeshDeploymentPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        flasher = self.root / "firmware/scripts/flash_verified_mesh.py"
        flasher.parent.mkdir(parents=True)
        flasher.write_text(
            "FLASH_FREQUENCY_HZ = 4_000_000\n"
            "WEST_EXECUTABLE = _venv_executable(\"west\")\n"
            "verify_flash(\n--frequency\n--stage-only\n--hardware-manifest\n"
            "awaiting_qualification\n_code_sectors_match\n_record_consumed_capture\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_rejects_direct_west_and_pyocd_production_shapes(self) -> None:
        self._write("CODEMAP.md", ".venv/bin/west flash --build-dir build/mesh-clicker -- --frequency 4000000\n")
        self._write("Documentation/deploy.md", "pyocd flash build/mesh_anchor_1/zephyr/zephyr.hex\n")
        issues = policy.check_repository(self.root)
        self.assertEqual(2, len(issues), issues)
        self.assertTrue(any("CODEMAP.md:1" in issue for issue in issues), issues)
        self.assertTrue(any("Documentation/deploy.md:1" in issue for issue in issues), issues)

    def test_allows_only_explicit_bench_or_legacy_direct_flash(self) -> None:
        self._write("AGENTS.md", ".venv/bin/west flash --build-dir build/mesh-transmitter-forcedhop -- --frequency 4000000\n")
        self._write("Documentation/legacy.md", "pyocd flash build/firmware-anchor/zephyr/zephyr.hex\n")
        self.assertEqual([], policy.check_repository(self.root))

    def test_allows_forcedhop_anchor_bench_flash(self) -> None:
        self._write(
            "Documentation/bench.md",
            ".venv/bin/west flash --build-dir build/mesh-anchor-forcedhop -- --frequency 4000000\n",
        )
        self.assertEqual([], policy.check_repository(self.root))

    def test_rejects_generic_direct_flash_bypass(self) -> None:
        self._write("firmware/README.md", ".venv/bin/west flash --build-dir build/<preset>\n")
        issues = policy.check_repository(self.root)
        self.assertEqual(1, len(issues), issues)
        self.assertIn("firmware/README.md:1", issues[0])

    def test_requires_two_phase_stage_and_promotion_policy(self) -> None:
        flasher = self.root / "firmware/scripts/flash_verified_mesh.py"
        flasher.write_text(
            flasher.read_text(encoding="utf-8").replace("--stage-only\n", ""),
            encoding="utf-8",
        )
        issues = policy.check_repository(self.root)
        self.assertEqual(1, len(issues), issues)
        self.assertIn("'--stage-only'", issues[0])


if __name__ == "__main__":
    unittest.main()
