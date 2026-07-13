#!/usr/bin/env python3
"""Verified deployment flash regressions: failure must precede pyOCD west."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fixtures = _load("stack_evidence_fixtures", REPO_ROOT / "firmware" / "tests" / "mesh_integration" / "test_verify_stack_evidence.py")
flash = _load("flash_verified_mesh_test", REPO_ROOT / "firmware" / "scripts" / "flash_verified_mesh.py")


class VerifiedFlashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        fixtures.StackEvidenceVerifierTests.setUpClass()

    def setUp(self) -> None:
        self.case = fixtures.StackEvidenceVerifierTests("runTest")
        self.case.setUp()
        self.policy = self.case.policies["mesh_clicker"]
        self.ledger = self.case.root / "verified-captures.jsonl"
        self.west = self.case.root / "west"
        self.west.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.west.chmod(0o755)
        self.patchers = [
            mock.patch.object(flash, "CAPTURE_LEDGER", self.ledger),
            mock.patch.object(flash, "WEST_EXECUTABLE", self.west),
        ]
        for patcher in self.patchers:
            patcher.start()

    def tearDown(self) -> None:
        for patcher in reversed(self.patchers):
            patcher.stop()
        self.case.tearDown()

    @staticmethod
    def _args(build: Path, manifest: Path) -> list[str]:
        return ["--build-dir", str(build), "--hardware-manifest", str(manifest), "--probe-id", "TEST-PROBE"]

    def _valid(self) -> tuple[Path, Path]:
        build = self.case._write_build(self.policy)
        evidence = flash.verifier.verify_build(build, self.case.policies, self.case.frame_limit)
        self.assertEqual([], evidence.issues)
        return build, self.case._manifest(self.policy, evidence)

    def test_missing_usage_and_invalid_manifest_block_before_west(self) -> None:
        build, manifest = self._valid()
        (build / "CMakeFiles" / "app.dir" / "src" / "main.c.su").unlink()
        self.assertEqual(1, flash.main(self._args(build, manifest)))
        build, manifest = self._valid()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["probe_id"] = "OTHER-PROBE"
        data["capture_id"] = flash.verifier._capture_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertEqual(1, flash.main(self._args(build, manifest)))

    def test_replayed_capture_blocks_before_west(self) -> None:
        build, manifest = self._valid()
        capture_id = json.loads(manifest.read_text(encoding="utf-8"))["capture_id"]
        self.ledger.write_text(json.dumps({"capture_id": capture_id}) + "\n", encoding="utf-8")
        self.assertEqual(1, flash.main(self._args(build, manifest)))

    def test_fixed_command_and_single_use_ledger(self) -> None:
        build, manifest = self._valid()
        def fake_run(command: list[str], **_: object) -> object:
            if command[0] == str(self.west):
                return mock.Mock(returncode=0)
            return mock.Mock(returncode=0, stdout="ninja: no work to do.\n", stderr="")

        with mock.patch.object(flash.subprocess, "run", side_effect=fake_run) as run:
            self.assertEqual(0, flash.main(self._args(build, manifest)))
        command = next(call.args[0] for call in run.call_args_list if call.args[0][0] == str(self.west))
        self.assertEqual(str(self.west), command[0])
        self.assertEqual(["--dev-id", "TEST-PROBE", "--frequency", "4000000"], command[-4:])
        self.assertIn("pyocd", command)
        self.assertEqual(1, len(self.ledger.read_text(encoding="utf-8").splitlines()))

    def test_cli_has_no_policy_frequency_or_west_bypass(self) -> None:
        for option in ("--frequency", "--policy-header", "--west"):
            with self.assertRaises(SystemExit):
                flash.parse_args(["--build-dir", "x", "--hardware-manifest", "x", "--probe-id", "x", option, "x"])


if __name__ == "__main__":
    unittest.main()
