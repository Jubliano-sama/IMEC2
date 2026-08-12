#!/usr/bin/env python3
"""Host capture workflow regressions without touching USB hardware."""

from __future__ import annotations

import importlib.util
import hashlib
import json
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
cohort = _load("artifact_cohort", REPO_ROOT / "firmware" / "scripts" / "artifact_cohort.py")
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
            self.assertIn(
                "pyocd rtt -t nrf52833 -M pre-reset "
                "-a 0x20000410 -s 0x100 -u TEST-PROBE",
                command[4],
            )
            self.assertIn("timeout --foreground --signal=INT 30", command[4])

    def test_target_readback_uses_full_flash_commander_capture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "target.bin"

            def fake_run(command: list[str], **_kwargs: object) -> object:
                destination.write_bytes(b"\xff" * cohort.FLASH_SIZE)
                return mock.Mock(returncode=0, stdout="", stderr="")

            with mock.patch.object(capture.subprocess, "run", side_effect=fake_run) as run:
                capture._read_target_flash("TEST-PROBE", destination)
            command = run.call_args.args[0]
            self.assertEqual("pyocd", command[0])
            self.assertIn("commander", command)
            self.assertIn("halt", command)
            self.assertTrue(any(
                item.startswith("savemem 0x0 0x80000 ") for item in command
            ))
            self.assertIn("reset", command)

    def test_target_readback_rejects_truncated_capture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "target.bin"

            def fake_run(command: list[str], **_kwargs: object) -> object:
                destination.write_bytes(b"\xff" * (cohort.FLASH_SIZE - 1))
                return mock.Mock(returncode=0, stdout="", stderr="")

            with mock.patch.object(capture.subprocess, "run", side_effect=fake_run):
                with self.assertRaisesRegex(verifier.EvidenceError, "expected 524288"):
                    capture._read_target_flash("TEST-PROBE", destination)

    def test_capture_binds_pre_and_post_rtt_target_readbacks(self) -> None:
        sector_sha256 = hashlib.sha256(b"\xff" * cohort.FLASH_SECTOR_SIZE).hexdigest()
        artifact = {
            "programmed_sector_sha256": {"0x00000000": sector_sha256},
        }
        binding = {
            "manifest_path": "/tmp/cohort.json",
            "cohort_id": "1" * 64,
            "source_id": "2" * 64,
            "artifact_id": "3" * 64,
            "artifact": artifact,
        }
        build = mock.Mock(
            issues=[], preset="mesh_anchor", elf_sha256="4" * 64,
            hex_sha256="5" * 64, build_identity="imec-stack-v1:test:" + "6" * 64,
        )
        policy = mock.Mock(deployable=True)

        def readback(_probe_id: str, destination: Path) -> None:
            destination.write_bytes(b"\xff" * cohort.FLASH_SIZE)

        def rtt(_probe_id: str, transcript: Path, _duration: int):
            physical_id = 0x1020304050607080
            node_id = physical_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN
            transcript.write_text(
                "typed RTT fixture\n"
                f"mesh node identity: ficr=0x{physical_id:016x} "
                f"node=0x{node_id:016x} preset=mesh_anchor\n",
                encoding="utf-8",
            )
            now = capture._utc_now()
            return now, now

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with (
                mock.patch.object(capture.verifier, "load_policy", return_value=({"mesh_anchor": policy}, 8192)),
                mock.patch.object(capture.verifier, "verify_build", return_value=build),
                mock.patch.object(capture.verifier, "verify_hardware", return_value=([mock.Mock(issues=[])], [])),
                mock.patch.object(capture, "_resolve_cohort", return_value=binding),
                mock.patch.object(capture, "_probe_is_visible"),
                mock.patch.object(capture, "_read_target_flash", side_effect=readback) as reads,
                mock.patch.object(capture, "_run_rtt", side_effect=rtt),
            ):
                manifest = capture.capture(
                    Path("unused"), "TEST-PROBE", output, 1, Path("cohort.json"),
                )
            data = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(2, reads.call_count)
            self.assertEqual("production_candidate", data["evidence_mode"])
            self.assertIs(True, data["promotion_allowed"])
            self.assertEqual(
                data["target"]["pre_capture_flash_sha256"],
                data["target"]["post_capture_flash_sha256"],
            )
            self.assertEqual(
                cohort.capture_binding_id(data), data["cohort_capture_id"],
            )
            self.assertEqual("nrf_ficr_mapped", data["target"]["identity_source"])
            self.assertEqual("0x1020304050607080", data["target"]["physical_id"])
            self.assertEqual(
                f"0x{(0x1020304050607080 ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN):016x}",
                data["target"]["node_id"],
            )

    def test_capture_identity_rejects_missing_conflicting_or_mismapped_records(self) -> None:
        physical_id = 0x1020304050607080
        node_id = physical_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN
        valid = (
            f"mesh node identity: ficr=0x{physical_id:016x} "
            f"node=0x{node_id:016x} preset=mesh_clicker\n"
        )
        identity = cohort.target_identity_from_transcript(valid + valid, "mesh_clicker")
        self.assertEqual("nrf_ficr_mapped", identity["identity_source"])
        self.assertEqual(
            f"0x{(node_id & 0xffff) or 1:04x}",
            identity["effective_uwb_short_addr"],
        )
        with self.assertRaisesRegex(cohort.CohortError, "lacks"):
            cohort.target_identity_from_transcript("no identity\n", "mesh_clicker")
        with self.assertRaisesRegex(cohort.CohortError, "conflicting"):
            cohort.target_identity_from_transcript(
                valid + valid.replace(f"{physical_id:016x}", "1020304050607081"),
                "mesh_clicker",
            )
        with self.assertRaisesRegex(cohort.CohortError, "mapping"):
            cohort.target_identity_from_transcript(
                valid.replace(f"{node_id:016x}", "123456789abcdef0"),
                "mesh_clicker",
            )
        with self.assertRaisesRegex(cohort.CohortError, "fixed gateway"):
            cohort.target_identity_from_transcript(
                "mesh node identity: ficr=0x1020304050607080 "
                "node=0x123456789abcdef0 preset=mesh_gateway\n",
                "mesh_gateway",
            )

    def test_capture_duration_bounds_are_enforced_before_usb_access(self) -> None:
        self.assertEqual(1, capture.main(["--build-dir", "x", "--probe-id", "x", "--output-dir", "x", "--duration-seconds", "0"]))
        self.assertEqual(1, capture.main(["--build-dir", "x", "--probe-id", "x", "--output-dir", "x", "--duration-seconds", "901"]))

    def test_forcedhop_capture_mode_is_explicit(self) -> None:
        args = capture.parse_args([
            "--build-dir", "build/mesh-anchor-forcedhop",
            "--probe-id", "TEST-PROBE",
            "--output-dir", "captures",
            "--cohort-manifest", "cohort.json",
            "--bench-only",
        ])
        self.assertTrue(args.bench_only)
        self.assertEqual(Path("cohort.json"), args.cohort_manifest)


if __name__ == "__main__":
    unittest.main()
