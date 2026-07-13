#!/usr/bin/env python3
"""Transactional deployment regressions with a complete mocked target."""

from __future__ import annotations

import importlib.util
import hashlib
import json
import shlex
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

from intelhex import IntelHex


REPO_ROOT = Path(__file__).resolve().parents[3]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


fixtures = _load(
    "stack_evidence_fixtures",
    REPO_ROOT / "firmware" / "tests" / "mesh_integration" / "test_verify_stack_evidence.py",
)
flash = _load(
    "flash_verified_mesh_test",
    REPO_ROOT / "firmware" / "scripts" / "flash_verified_mesh.py",
)


class SimulatedPowerLoss(BaseException):
    pass


class MockTarget:
    def __init__(self, west: Path, pyocd: Path, capture: Path,
                 manifest: Path | None = None) -> None:
        self.west = west
        self.pyocd = pyocd
        self.capture = capture
        self.manifest = manifest
        self.original = bytes((index * 17 + 3) & 0xff for index in range(flash.TARGET_FLASH_SIZE))
        self.candidate = bytes((index * 29 + 11) & 0xff for index in range(flash.TARGET_FLASH_SIZE))
        self.target = self.original
        self.calls: list[list[str]] = []
        self.failures: dict[str, int] = {}
        self.corrupt_restore = False
        self.mutate_nvs_on_capture = False
        self.mutate_nvs_on_reset = False
        self.capture_stdout: str | None = None
        self.reset_count = 0
        self.before_west: object = None

    def fail_once(self, operation: str) -> None:
        self.failures[operation] = self.failures.get(operation, 0) + 1

    def _fails(self, operation: str) -> bool:
        remaining = self.failures.get(operation, 0)
        if remaining:
            self.failures[operation] = remaining - 1
            return True
        return False

    @staticmethod
    def _result(command: list[str], returncode: int = 0,
                stdout: str = "", stderr: str = "") -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(command, returncode, stdout, stderr)

    def run(self, command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
        command = [str(item) for item in command]
        self.calls.append(command)
        if command[0].endswith("fake-ninja"):
            return self._result(command, stdout="ninja: no work to do.\n")
        if command[:3] == [str(self.pyocd), "list", "--json"]:
            if self._fails("probe"):
                return self._result(command, 1, stderr="probe enumeration failed")
            return self._result(command, stdout=json.dumps([{"unique_id": "TEST-PROBE"}]))
        if command[:2] == [str(self.pyocd), "commander"]:
            commands = [command[index + 1] for index, item in enumerate(command) if item == "-c"]
            for raw in commands:
                words = shlex.split(raw)
                if not words:
                    continue
                if words[0] == "reset":
                    self.reset_count += 1
                    if self.mutate_nvs_on_reset:
                        changed = bytearray(self.target)
                        changed[-1] ^= 0x33
                        self.target = bytes(changed)
                    continue
                if words[0] == "savemem":
                    destination = Path(words[3])
                    if destination.name == "target-flash-backup.bin":
                        operation = "backup_read"
                    elif destination.name == "staged-readback.bin":
                        operation = "staged_read"
                    elif destination.name == "restored-readback.bin":
                        operation = "restore_read"
                    else:
                        operation = "recovery_read"
                    if self._fails(operation):
                        return self._result(command, 1, stderr=f"{operation} failed")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(self.target)
                elif words[0] == "load":
                    if self._fails("restore_load"):
                        return self._result(command, 1, stderr="restore failed")
                    restored = Path(words[1]).read_bytes()
                    self.target = self.candidate if self.corrupt_restore else restored
                else:
                    return self._result(command, 1, stderr=f"unknown command {raw}")
            return self._result(command)
        if command[:2] == [str(self.pyocd), "flash"]:
            if self._fails("restore_load"):
                return self._result(command, 1, stderr="restore failed")
            restored = Path(command[-1]).read_bytes()
            self.target = self.candidate if self.corrupt_restore else restored
            return self._result(command)
        if command[0] == str(self.west):
            if callable(self.before_west):
                self.before_west()
            if self._fails("west_corrupt"):
                self.target = b"\x00" * flash.TARGET_FLASH_SIZE
                return self._result(command)
            if self._fails("west"):
                # A real west failure may occur after partially programming.
                self.target = self.candidate
                return self._result(command, 1, stderr="stage failed")
            self.target = self.candidate
            return self._result(command)
        if len(command) > 1 and command[1] == str(self.capture):
            if self._fails("capture"):
                return self._result(command, 1, stderr="capture rejected")
            if self.mutate_nvs_on_capture:
                changed = bytearray(self.target)
                changed[-1] ^= 0x5a
                self.target = bytes(changed)
            assert self.manifest is not None
            stdout = self.capture_stdout
            return self._result(
                command, stdout=stdout if stdout is not None else f"{self.manifest}\n"
            )
        return self._result(command, 1, stderr=f"unexpected command: {command}")


class VerifiedFlashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        fixtures.StackEvidenceVerifierTests.setUpClass()

    def setUp(self) -> None:
        self.case = fixtures.StackEvidenceVerifierTests("runTest")
        self.case.setUp()
        self.policy = self.case.policies["mesh_clicker"]
        self.ledger = self.case.root / "verified-captures.jsonl"
        self.transactions = self.case.root / "transactions"
        self.journal = self.transactions / f"{flash._probe_key('TEST-PROBE')}.json"
        self.output = self.case.root / "captures"
        self.west = self.case.root / "west"
        self.pyocd = self.case.root / "pyocd"
        self.capture = self.case.root / "capture_stack_evidence.py"
        for executable in (self.west, self.pyocd, self.capture):
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
        self.target = MockTarget(self.west, self.pyocd, self.capture)
        self.patchers = [
            mock.patch.object(flash, "CAPTURE_LEDGER", self.ledger),
            mock.patch.object(flash, "TRANSACTION_DIRECTORY", self.transactions),
            mock.patch.object(flash, "WEST_EXECUTABLE", self.west),
            mock.patch.object(flash, "PYOCD_EXECUTABLE", self.pyocd),
            mock.patch.object(flash, "CAPTURE_EXECUTABLE", self.capture),
            mock.patch.object(flash.subprocess, "run", side_effect=self.target.run),
        ]
        for patcher in self.patchers:
            patcher.start()

    def tearDown(self) -> None:
        for patcher in reversed(self.patchers):
            patcher.stop()
        self.case.tearDown()

    def _args(self, build: Path) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--output-dir", str(self.output),
            "--duration-seconds", "7",
        ]

    def _valid(self) -> tuple[Path, Path]:
        build = self.case._write_build(self.policy)
        hex_path = build / "zephyr" / "zephyr.hex"
        image = IntelHex()
        for address in range(96):
            image[address] = (address * 7 + 19) & 0xff
        for address in range(0x2100, 0x2140):
            image[address] = (address * 11 + 5) & 0xff
        image.write_hex_file(str(hex_path))
        original = self.case.root / "simulated-original.bin"
        original.write_bytes(self.target.original)
        self.target.candidate = flash._expected_staged_image(original, hex_path)
        evidence = flash.verifier.verify_build(build, self.case.policies, self.case.frame_limit)
        self.assertEqual([], evidence.issues)
        manifest = self.case._manifest(self.policy, evidence)
        data = json.loads(manifest.read_text(encoding="utf-8"))
        transcript = manifest.parent / data["transcript"]["path"]
        self.output.mkdir(parents=True, exist_ok=True)
        transcript.replace(self.output / transcript.name)
        destination = self.output / manifest.name
        manifest.replace(destination)
        manifest = destination
        self.target.manifest = manifest
        return build, manifest

    def _reset_transaction_state(self) -> None:
        self.target.target = self.target.original
        self.target.calls.clear()
        self.target.failures.clear()
        self.target.corrupt_restore = False
        self.target.mutate_nvs_on_capture = False
        self.target.mutate_nvs_on_reset = False
        self.target.capture_stdout = None
        self.target.reset_count = 0
        if self.ledger.exists():
            self.ledger.unlink()
        if self.journal.exists():
            self.journal.unlink()
        if self.transactions.exists():
            for path in sorted(self.transactions.rglob("*"), reverse=True):
                if path.is_file():
                    path.unlink()
                else:
                    path.rmdir()
            self.transactions.rmdir()

    def test_static_eligibility_and_probe_checks_precede_every_write(self) -> None:
        build, _ = self._valid()
        usage = build / "CMakeFiles" / "app.dir" / "src" / "main.c.su"
        usage.unlink()
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"] for call in self.target.calls))

        build, _ = self._valid()
        self.target.fail_once("probe")
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call and call[0] == str(self.west) for call in self.target.calls))

    def test_success_backs_up_stages_qualifies_and_promotes_at_fixed_frequency(self) -> None:
        build, manifest = self._valid()
        self.target.capture_stdout = (
            "\x1b[33mpyOCD: RTT capture complete\x1b[0m\r\n"
            f"\x1b[0m\x1b[32m{manifest}\x1b[0m\r\n"
        )

        def assert_staging_journal() -> None:
            data = json.loads(self.journal.read_text(encoding="utf-8"))
            self.assertEqual("staging", data["state"])
            backup = Path(data["backup_path"])
            self.assertEqual(flash.TARGET_FLASH_SIZE, backup.stat().st_size)
            self.assertEqual(flash._sha256(backup), data["backup_sha256"])
            self.assertEqual(0o600, self.journal.stat().st_mode & 0o777)

        self.target.before_west = assert_staging_journal
        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertFalse(self.journal.exists())
        records = flash._ledger_records()
        self.assertEqual(1, len(records))
        self.assertEqual(hashlib.sha256(self.target.candidate).hexdigest(), records[0]["staged_flash_sha256"])

        west = next(call for call in self.target.calls if call[0] == str(self.west))
        self.assertEqual([
            str(self.west), "flash", "--runner", "pyocd", "--build-dir", str(build),
            "--", "--dev-id", "TEST-PROBE", "--frequency", "4000000",
            "--flash-opt=--no-reset",
        ], west)
        commanders = [call for call in self.target.calls if call[:2] == [str(self.pyocd), "commander"]]
        self.assertGreaterEqual(len(commanders), 2)
        self.assertTrue(all(call[call.index("-f") + 1] == "4000000" for call in commanders))
        backup_command = next(call for call in commanders if any("target-flash-backup.bin" in item for item in call))
        self.assertEqual(1, backup_command.count("-c"))
        self.assertNotIn("reset", backup_command)
        capture = next(call for call in self.target.calls if len(call) > 1 and call[1] == str(self.capture))
        self.assertEqual("7", capture[capture.index("--duration-seconds") + 1])
        self.assertNotIn("--command", capture)

    def test_capture_manifest_parser_rejects_control_and_path_injection(self) -> None:
        _, manifest = self._valid()
        outside = self.case.root / "outside.json"
        outside.write_text("{}\n", encoding="utf-8")
        wrong_suffix = self.output / "capture.txt"
        wrong_suffix.write_text("{}\n", encoding="utf-8")

        cases = (
            (f"{self.output / '..' / outside.name}\n", "outside"),
            (f"\x1b[2K{manifest}\n", "control"),
            (f"{manifest}\x08\n", "control"),
            (f"\x1b]8;;file://capture\x07{manifest}\x1b]8;;\x07\n", "control"),
            (f"{wrong_suffix}\n", "JSON"),
        )
        for stdout, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(
                flash.TransactionError, message
            ):
                flash._capture_manifest_path(stdout, self.output)

    def test_backup_stays_halted_until_staging_and_prejournal_failure_resets(self) -> None:
        build, _ = self._valid()
        self.target.mutate_nvs_on_reset = True

        def assert_no_pre_stage_reset() -> None:
            self.assertEqual(0, self.target.reset_count)
            self.assertEqual(self.target.original, self.target.target)

        self.target.before_west = assert_no_pre_stage_reset
        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(1, self.target.reset_count)
        self.assertNotEqual(self.target.candidate, self.target.target)

        self._reset_transaction_state()
        with mock.patch.object(
            flash, "_expected_staged_image", side_effect=flash.TransactionError("bad HEX")
        ):
            self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(1, self.target.reset_count)
        self.assertFalse(self.journal.exists())
        self.assertFalse(self.transactions.exists() and any(self.transactions.rglob("*")))
        self.assertFalse(any(call and call[0] == str(self.west) for call in self.target.calls))

    def test_commander_repeats_command_switch_for_every_action(self) -> None:
        completed = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(flash, "_run", return_value=completed) as run:
            flash._commander("TEST-PROBE", "savemem 0x0 0x80000 backup.bin", "reset")
        self.assertEqual([
            str(self.pyocd), "commander", "--no-config", "-t", "nrf52833",
            "-u", "TEST-PROBE", "-f", "4000000", "-M", "halt",
            "-c", "savemem 0x0 0x80000 backup.bin", "-c", "reset",
        ], run.call_args.args[0])

    def test_expected_image_preserves_untouched_flash_and_rejects_uicr(self) -> None:
        build, _ = self._valid()
        original = self.case.root / "original.bin"
        original.write_bytes(self.target.original)
        expected = flash._expected_staged_image(original, build / "zephyr" / "zephyr.hex")
        self.assertEqual(b"\xff" * 16, expected[0x100:0x110])
        self.assertEqual(self.target.original[0x3000:], expected[0x3000:])

        outside = self.case.root / "uicr.hex"
        image = IntelHex()
        image[flash.TARGET_FLASH_SIZE] = 0x42
        image.write_hex_file(str(outside))
        with self.assertRaisesRegex(flash.TransactionError, "outside"):
            flash._expected_staged_image(original, outside)

    def test_stage_readback_and_artifact_freeze_fail_before_capture(self) -> None:
        build, _ = self._valid()
        self.target.fail_once("west_corrupt")
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(len(call) > 1 and call[1] == str(self.capture) for call in self.target.calls))

        self._reset_transaction_state()
        hex_path = build / "zephyr" / "zephyr.hex"

        def mutate_candidate() -> None:
            hex_path.write_bytes(hex_path.read_bytes() + b"\n")

        self.target.before_west = mutate_candidate
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(len(call) > 1 and call[1] == str(self.capture) for call in self.target.calls))

    def test_every_runtime_failure_rolls_back_and_verifies_original_flash(self) -> None:
        build, manifest = self._valid()
        scenarios = ("west", "staged_read", "capture", "ledger")
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                self._reset_transaction_state()
                if scenario == "ledger":
                    context = mock.patch.object(
                        flash, "_record_consumed_capture", side_effect=OSError("ledger fsync failed")
                    )
                else:
                    self.target.fail_once(scenario)
                    context = mock.patch.object(flash, "_checkpoint", side_effect=lambda _: None)
                with context:
                    self.assertEqual(1, flash.main(self._args(build)))
                self.assertEqual(self.target.original, self.target.target)
                self.assertFalse(self.journal.exists())
                self.assertFalse(self.ledger.exists())

        restore = next(call for call in self.target.calls if call[:2] == [str(self.pyocd), "flash"])
        self.assertIn("--no-config", restore)
        self.assertIn("--no-reset", restore)
        self.assertEqual("4000000", restore[restore.index("-f") + 1])

        self._reset_transaction_state()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["probe_id"] = "OTHER-PROBE"
        data["capture_id"] = flash.verifier._capture_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(self.journal.exists())

    def test_append_then_fsync_error_is_reconciled_as_durable_promotion(self) -> None:
        build, _ = self._valid()
        append = flash._record_consumed_capture

        def append_then_fail(record: dict[str, object]) -> None:
            append(record)
            raise OSError("simulated post-append fsync uncertainty")

        with mock.patch.object(flash, "_record_consumed_capture", side_effect=append_then_fail):
            self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertEqual(1, len(flash._ledger_records()))
        self.assertFalse(self.journal.exists())

    def test_promotion_recovery_allows_untouched_nvs_to_change_during_capture(self) -> None:
        build, _ = self._valid()
        self.target.mutate_nvs_on_capture = True

        def crash(name: str) -> None:
            if name == "promotion_ledger_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._args(build))
        evolved = self.target.target
        self.assertNotEqual(self.target.candidate, evolved)
        flash._recover_interrupted_transaction("TEST-PROBE")
        self.assertEqual(evolved, self.target.target)
        self.assertFalse(self.journal.exists())
        self.assertEqual(1, len(flash._ledger_records()))

    def test_crash_recovery_rolls_back_or_completes_durable_promotion(self) -> None:
        build, _ = self._valid()
        rollback_points = (
            "journal_durable", "staging_durable", "candidate_staged",
            "staged_hash_durable", "promotion_intent_durable",
        )
        promotion_points = ("promotion_ledger_durable", "commit_durable")
        for checkpoint in rollback_points + promotion_points:
            with self.subTest(checkpoint=checkpoint):
                self._reset_transaction_state()

                def crash(name: str, expected: str = checkpoint) -> None:
                    if name == expected:
                        raise SimulatedPowerLoss(name)

                with mock.patch.object(flash, "_checkpoint", side_effect=crash):
                    with self.assertRaises(SimulatedPowerLoss):
                        flash.main(self._args(build))
                self.assertTrue(self.journal.exists())
                flash._recover_interrupted_transaction("TEST-PROBE")
                expected = self.target.original if checkpoint in rollback_points else self.target.candidate
                self.assertEqual(expected, self.target.target)
                self.assertFalse(self.journal.exists())
                if checkpoint in promotion_points:
                    self.assertEqual(1, len(flash._ledger_records()))
                else:
                    self.assertFalse(self.ledger.exists())

    def test_failed_restore_stays_journaled_until_verified_recovery(self) -> None:
        build, _ = self._valid()
        self.target.fail_once("capture")
        self.target.corrupt_restore = True
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertTrue(self.journal.exists())
        self.assertNotEqual(self.target.original, self.target.target)

        self.target.corrupt_restore = False
        flash._recover_interrupted_transaction("TEST-PROBE")
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(self.journal.exists())

    def test_malformed_ledger_at_promotion_intent_forces_rollback(self) -> None:
        build, _ = self._valid()

        def crash(name: str) -> None:
            if name == "promotion_intent_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._args(build))
        self.assertEqual(self.target.candidate, self.target.target)
        self.ledger.parent.mkdir(parents=True, exist_ok=True)
        self.ledger.write_text('{"capture_id":', encoding="utf-8")
        flash._recover_interrupted_transaction("TEST-PROBE")
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(self.journal.exists())

    def test_persisted_transaction_recovers_before_later_candidate_validation(self) -> None:
        build, _ = self._valid()

        def crash(name: str) -> None:
            if name == "candidate_staged":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._args(build))
        self.assertEqual(self.target.candidate, self.target.target)

        (build / "CMakeFiles" / "app.dir" / "src" / "main.c.su").unlink()
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(self.journal.exists())

    def test_corrupt_journal_and_ledger_fail_closed_without_target_writes(self) -> None:
        build, _ = self._valid()
        self.journal.parent.mkdir(parents=True, exist_ok=True)
        self.journal.write_text("not json\n", encoding="utf-8")
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"] for call in self.target.calls))

        self.journal.unlink()
        self.ledger.write_text("not json\n", encoding="utf-8")
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call and call[0] == str(self.west) for call in self.target.calls))

    def test_cli_exposes_no_frequency_tool_manifest_or_shell_bypass(self) -> None:
        base = ["--build-dir", "x", "--probe-id", "x"]
        for option in ("--frequency", "--policy-header", "--west", "--pyocd", "--hardware-manifest", "--command"):
            with self.subTest(option=option), self.assertRaises(SystemExit):
                flash.parse_args([*base, option, "x"])


if __name__ == "__main__":
    unittest.main()
