#!/usr/bin/env python3
"""Host capture workflow regressions without touching USB hardware."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


verifier = _load("verify_stack_evidence", REPO_ROOT / "firmware" / "scripts" / "verify_stack_evidence.py")
capture = _load("capture_stack_evidence_test", REPO_ROOT / "firmware" / "scripts" / "capture_stack_evidence.py")


class CaptureStackEvidenceTests(unittest.TestCase):
    def test_probe_enumeration_accepts_json_and_plain_fallback(self) -> None:
        json_result = mock.Mock(
            returncode=0,
            stdout='[{"unique_id":"TEST-PROBE"}]',
            stderr="",
        )
        with mock.patch.object(capture.subprocess, "run",
                               return_value=json_result) as run:
            capture._probe_is_visible("TEST-PROBE")
        self.assertEqual(["pyocd", "list", "--json"], run.call_args.args[0])

        unsupported = mock.Mock(returncode=2, stdout="", stderr="unsupported")
        plain = mock.Mock(
            returncode=0,
            stdout="0  CMSIS-DAP  TEST-PROBE  n/a\n",
            stderr="",
        )
        with mock.patch.object(capture.subprocess, "run",
                               side_effect=[unsupported, plain]) as run:
            capture._probe_is_visible("TEST-PROBE")
        self.assertEqual(["pyocd", "list"], run.call_args_list[1].args[0])

    def test_probe_enumeration_fallback_rejects_missing_probe(self) -> None:
        unsupported = mock.Mock(returncode=2, stdout="", stderr="unsupported")
        plain = mock.Mock(returncode=0, stdout="0 CMSIS-DAP OTHER n/a\n",
                          stderr="")

        with mock.patch.object(capture.subprocess, "run",
                               side_effect=[unsupported, plain]):
            with self.assertRaises(verifier.EvidenceError):
                capture._probe_is_visible("TEST-PROBE")

    def test_rtt_capture_uses_tty_and_fixed_pre_reset_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            transcript = Path(temporary) / "capture.typescript"

            def fake_run(command: list[str], check: bool) -> object:
                transcript.write_text("DBG_STACK_BOOT preset=mesh_clicker build=x epoch=1 uptime=1\n", encoding="utf-8")
                return mock.Mock(returncode=124)

            with mock.patch.object(capture.subprocess, "run", side_effect=fake_run) as run:
                capture._run_rtt("TEST-PROBE", transcript, 30)
            command = run.call_args.args[0]
            self.assertEqual("script", command[0])
            self.assertIn("pyocd rtt -t nrf52833 -M pre-reset -u TEST-PROBE", command[4])
            self.assertIn("timeout --foreground --signal=INT 30", command[4])

    def test_capture_duration_bounds_are_enforced_before_usb_access(self) -> None:
        self.assertEqual(1, capture.main(["--build-dir", "x", "--probe-id", "x", "--output-dir", "x", "--duration-seconds", "0"]))
        self.assertEqual(1, capture.main(["--build-dir", "x", "--probe-id", "x", "--output-dir", "x", "--duration-seconds", "901"]))


if __name__ == "__main__":
    unittest.main()
