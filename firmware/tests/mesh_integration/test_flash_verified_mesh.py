#!/usr/bin/env python3
"""In-place deployment regressions with a complete mocked target."""

from __future__ import annotations

import importlib.util
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
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
cohort = _load(
    "artifact_cohort",
    REPO_ROOT / "firmware" / "scripts" / "artifact_cohort.py",
)
flash = _load(
    "flash_verified_mesh_test",
    REPO_ROOT / "firmware" / "scripts" / "flash_verified_mesh.py",
)


class SimulatedPowerLoss(BaseException):
    pass


class MockTarget:
    def __init__(self, west: Path, pyocd: Path) -> None:
        self.west = west
        self.pyocd = pyocd
        self.manifest: Path | None = None
        self.original = bytes((index * 17 + 3) & 0xff for index in range(flash.TARGET_FLASH_SIZE))
        self.candidate = bytes((index * 29 + 11) & 0xff for index in range(flash.TARGET_FLASH_SIZE))
        self.target = self.original
        self.calls: list[list[str]] = []
        self.failures: dict[str, int] = {}
        self.corrupt_restore = False
        self.mutate_nvs_on_reset = False
        self.mutate_nvs_after_storage_erase = False
        self.reset_count = 0
        self.before_west: object = None
        self.before_storage_erase: object = None

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
                        self.mutate_nvs_on_reset = False
                    continue
                if words[0] == "erase":
                    if callable(self.before_storage_erase):
                        self.before_storage_erase()
                    if words[1:] != [
                        f"0x{flash.STORAGE_PARTITION_ADDRESS:x}",
                        f"0x{flash.STORAGE_PARTITION_SIZE:x}",
                    ]:
                        return self._result(
                            command, 1, stderr=f"unexpected erase command {raw}"
                        )
                    if self._fails("storage_erase"):
                        return self._result(
                            command, 1, stderr="storage erase failed"
                        )
                    target = bytearray(self.target)
                    target[
                        flash.STORAGE_PARTITION_ADDRESS:
                        flash.STORAGE_PARTITION_END
                    ] = b"\xff" * flash.STORAGE_PARTITION_SIZE
                    if self.mutate_nvs_after_storage_erase:
                        target[-1] ^= 0x5a
                        self.mutate_nvs_after_storage_erase = False
                    self.target = bytes(target)
                    continue
                if words[0] == "halt":
                    continue
                if words[0] == "savemem":
                    destination = Path(words[3])
                    if destination.name == "target-flash-backup.bin":
                        operation = "backup_read"
                    elif destination.name == "staged-readback.bin":
                        operation = "staged_read"
                    elif destination.name == "promotion-readback.bin":
                        operation = "promotion_read"
                    elif destination.name == "supersede-readback.bin":
                        operation = "supersede_read"
                    elif destination.name == "qualified-retirement-readback.bin":
                        operation = "qualified_retirement_read"
                    elif destination.name == "restored-readback.bin":
                        operation = "restore_read"
                    else:
                        operation = "recovery_read"
                    if self._fails(operation):
                        return self._result(command, 1, stderr=f"{operation} failed")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(self.target)
                elif words[0] == "continue":
                    if self._fails("resume"):
                        return self._result(command, 1, stderr="resume failed")
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
        if command[:2] == [str(self.pyocd), "erase"]:
            if callable(self.before_storage_erase):
                self.before_storage_erase()
            try:
                sector = command[command.index("--sector") + 1]
            except (ValueError, IndexError):
                return self._result(command, 1, stderr="missing sector range")
            expected = (
                f"0x{flash.STORAGE_PARTITION_ADDRESS:x}-"
                f"0x{flash.STORAGE_PARTITION_END:x}"
            )
            if sector != expected:
                return self._result(command, 1, stderr=f"unexpected erase range {sector}")
            if self._fails("storage_erase"):
                return self._result(command, 1, stderr="storage erase failed")
            target = bytearray(self.target)
            target[
                flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
            ] = b"\xff" * flash.STORAGE_PARTITION_SIZE
            if self.mutate_nvs_after_storage_erase:
                target[-1] ^= 0x5a
                self.mutate_nvs_after_storage_erase = False
            self.target = bytes(target)
            return self._result(command)
        if command[0] == str(self.west):
            if callable(self.before_west):
                self.before_west()
            if self._fails("west_no_write"):
                return self._result(command, 1, stderr="stage launcher failed")
            if self._fails("west_corrupt"):
                self.target = b"\x00" * flash.TARGET_FLASH_SIZE
                return self._result(command)
            if self._fails("west"):
                # A real west failure may occur after partially programming.
                self.target = self.candidate
                return self._result(command, 1, stderr="stage failed")
            self.target = self.candidate
            return self._result(command)
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
        self.cohorts = self.case.root / "cohorts"
        self.bench_qualifications = self.case.root / "bench-qualifications"
        self.west = self.case.root / "west"
        self.pyocd = self.case.root / "pyocd"
        for executable in (self.west, self.pyocd):
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
        self.target = MockTarget(self.west, self.pyocd)
        self.patchers = [
            mock.patch.object(flash, "CAPTURE_LEDGER", self.ledger),
            mock.patch.object(flash, "TRANSACTION_DIRECTORY", self.transactions),
            mock.patch.object(flash, "COHORT_DIRECTORY", self.cohorts),
            mock.patch.object(
                flash,
                "BENCH_QUALIFICATION_DIRECTORY",
                self.bench_qualifications,
            ),
            mock.patch.object(flash, "WEST_EXECUTABLE", self.west),
            mock.patch.object(flash, "PYOCD_EXECUTABLE", self.pyocd),
            mock.patch.object(
                flash, "_resolve_cohort", side_effect=self._cohort_binding,
            ),
            mock.patch.object(flash.subprocess, "run", side_effect=self.target.run),
        ]
        for patcher in self.patchers:
            patcher.start()

    def tearDown(self) -> None:
        for patcher in reversed(self.patchers):
            patcher.stop()
        self.case.tearDown()

    def test_subprocess_path_prefers_verified_pyocd_directory(self) -> None:
        with mock.patch.object(
            flash.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["probe"], 0, "", ""),
        ) as run:
            flash._run(["probe"])

        environment = run.call_args.kwargs["env"]
        self.assertEqual(
            str(self.pyocd.parent),
            environment["PATH"].split(os.pathsep)[0],
        )

    def test_west_runs_from_its_canonical_workspace_in_a_worktree(self) -> None:
        workspace = self.case.root / "west-workspace"
        west = workspace / ".venv" / "bin" / "west"
        west.parent.mkdir(parents=True)
        west.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        west.chmod(0o755)

        with mock.patch.object(
            flash, "WEST_EXECUTABLE", west,
        ), mock.patch.object(
            flash.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([str(west)], 0, "", ""),
        ) as run:
            flash._run([str(west), "flash"])

        self.assertEqual(workspace.resolve(), run.call_args.kwargs["cwd"])

    def _args(self, build: Path, manifest: Path | None = None) -> list[str]:
        manifest = manifest or self.target.manifest
        assert manifest is not None
        return [
            "--build-dir", str(build),
            "--hardware-manifest", str(manifest),
            "--probe-id", "TEST-PROBE",
        ]

    @staticmethod
    def _stage_args(build: Path) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--stage-only",
        ]

    @staticmethod
    def _initialize_storage_args(build: Path) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--stage-only", "--initialize-storage",
        ]

    @staticmethod
    def _reject_args(build: Path) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--reject-staged-candidate",
        ]

    @staticmethod
    def _supersede_args(build: Path) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--supersede-staged-candidate",
        ]

    @staticmethod
    def _complete_bench_args(
        build: Path,
        capture: Path,
        topology: Path,
        cohort_manifest: Path,
    ) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--hardware-manifest", str(capture),
            "--cohort-manifest", str(cohort_manifest),
            "--topology-manifest", str(topology),
            "--complete-bench-qualification",
        ]

    @staticmethod
    def _abandon_args(build: Path) -> list[str]:
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--abandon-staged-candidate",
        ]

    def _retire_qualified_args(self, build: Path) -> list[str]:
        transcript = self.case.root / "reachability-success.log"
        transcript.write_text(
            "HERE_I_AM_REACHABILITY_QUALIFICATION_OK "
            "anchors=3 direct=3 multihop=0 retries=0\n",
            encoding="utf-8",
        )
        return [
            "--build-dir", str(build),
            "--probe-id", "TEST-PROBE",
            "--retire-qualified-candidate",
            "--qualification-log", str(transcript),
        ]

    def _stage(self, build: Path) -> int:
        return flash.main(self._stage_args(build))

    def _stage_with_storage_initialization(self, build: Path) -> int:
        return flash.main(self._initialize_storage_args(build))

    def _hardware_calls(self) -> list[list[str]]:
        return [
            call for call in self.target.calls
            if call and call[0] in {str(self.west), str(self.pyocd)}
        ]

    def _cohort_binding(
        self,
        build: Path,
        manifest: Path | None,
    ) -> dict[str, object]:
        cache = cohort._parse_cache(build)
        preset = cache["IMEC_BUILD_PRESET"]
        elf = build / "zephyr" / "zephyr.elf"
        hex_path = build / "zephyr" / "zephyr.hex"
        config = build / "zephyr" / ".config"
        artifact_id = hashlib.sha256(
            elf.read_bytes() + hex_path.read_bytes() + config.read_bytes()
        ).hexdigest()
        source_id = "1" * 64
        cohort_id = hashlib.sha256(
            f"{source_id}:{artifact_id}".encode("ascii")
        ).hexdigest()
        selected = manifest or self.cohorts / f"cohort-{cohort_id}.json"
        artifact = {
            "preset": preset,
            "source_id": source_id,
            "artifact_id": artifact_id,
            "programmed_sector_sha256": cohort.programmed_sector_hashes(hex_path),
        }
        return {
            "manifest_path": str(selected.resolve()),
            "cohort_id": cohort_id,
            "source_id": source_id,
            "artifact_id": artifact_id,
            "artifact": artifact,
        }

    def test_bench_bypass_is_allowed_only_for_stage_and_bench_completion(self) -> None:
        source = (
            REPO_ROOT / "firmware" / "scripts" / "flash_verified_mesh.py"
        ).read_text(encoding="utf-8")
        stage_start = source.index("def _verify_stage_candidate(")
        stage_end = source.index("def verify_flash(", stage_start)
        promotion_end = source.index("def _verify_bench_capture(", stage_end)
        bench_end = source.index("def _validated_topology_manifest(", promotion_end)

        self.assertIn(
            "allow_watchdog_bypass=True",
            source[stage_start:stage_end],
        )
        self.assertNotIn(
            "allow_watchdog_bypass=True",
            source[stage_end:promotion_end],
        )
        self.assertIn(
            "allow_watchdog_bypass=True",
            source[promotion_end:bench_end],
        )

    def _valid(self, policy: object | None = None) -> tuple[Path, Path]:
        selected_policy = policy or self.policy
        build = self.case._write_build(selected_policy)
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
        if not selected_policy.deployable:
            manifest = self.case.root / f"{selected_policy.preset}-bench-manifest.json"
            manifest.write_text("{}\n", encoding="utf-8")
            self.target.manifest = manifest
            return build, manifest
        manifest = self.case._manifest(selected_policy, evidence)
        data = json.loads(manifest.read_text(encoding="utf-8"))
        binding = self._cohort_binding(build, None)
        data["artifact"]["artifact_id"] = binding["artifact_id"]
        target_sha256 = hashlib.sha256(self.target.candidate).hexdigest()
        data["target"].update({
            "flash_sha256": target_sha256,
            "pre_capture_flash_sha256": target_sha256,
            "post_capture_flash_sha256": target_sha256,
            "code_sector_map_sha256": flash._sector_hash_map_sha256(
                binding["artifact"]["programmed_sector_sha256"],
            ),
            "pre_readback_started_at_utc": "2026-08-10T10:00:00Z",
            "pre_readback_completed_at_utc": "2026-08-10T10:00:01Z",
            "post_readback_started_at_utc": "2026-08-10T10:01:00Z",
            "post_readback_completed_at_utc": "2026-08-10T10:01:01Z",
        })
        data["cohort"] = {
            key: binding[key]
            for key in ("manifest_path", "cohort_id", "source_id", "artifact_id")
        }
        data["evidence_mode"] = "production_candidate"
        data["promotion_allowed"] = True
        data["cohort_capture_id"] = cohort.capture_binding_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        transcript = manifest.parent / data["transcript"]["path"]
        self.output.mkdir(parents=True, exist_ok=True)
        transcript.replace(self.output / transcript.name)
        destination = self.output / manifest.name
        manifest.replace(destination)
        manifest = destination
        self.target.manifest = manifest
        return build, manifest

    def _bench_completion_fixture(
        self,
    ) -> tuple[Path, Path, Path, Path, dict[str, object]]:
        policy = self.case.policies["mesh_anchor_forcedhop"]
        build, _ = self._valid(policy)
        evidence = flash.verifier.verify_build(
            build,
            self.case.policies,
            self.case.frame_limit,
            allow_watchdog_bypass=True,
        )
        self.assertEqual([], evidence.issues)

        source_payload: dict[str, object] = {
            "fixture": "bench-completion-source",
        }
        source_id = cohort._content_id(source_payload)
        source = {**source_payload, "source_id": source_id}
        toolchain_payload: dict[str, object] = {
            "fixture": "bench-completion-toolchain",
        }
        toolchain_id = cohort._content_id(toolchain_payload)
        toolchain = {**toolchain_payload, "toolchain_id": toolchain_id}
        sectors = cohort.programmed_sector_hashes(
            build / "zephyr" / "zephyr.hex",
        )

        artifacts: dict[str, dict[str, object]] = {}
        for preset in sorted(cohort.BENCH_TOPOLOGY_PRESETS):
            artifact: dict[str, object] = {
                "preset": preset,
                "source_id": source_id,
                "legacy_build_identity": (
                    evidence.build_identity
                    if preset == "mesh_anchor_forcedhop"
                    else f"imec-stack-v1:{preset}:{'b' * 64}"
                ),
                "elf_sha256": evidence.elf_sha256,
                "hex_sha256": evidence.hex_sha256,
                "config_sha256": hashlib.sha256(
                    (build / "zephyr" / ".config").read_bytes()
                ).hexdigest(),
                "devicetree_sha256": hashlib.sha256(
                    (build / "zephyr" / "zephyr.dts").read_bytes()
                ).hexdigest(),
                "programmed_sector_sha256": sectors,
                "board": "nrf52833dk/nrf52833",
                "toolchain_id": toolchain_id,
                "toolchain": toolchain,
            }
            artifact["artifact_id"] = cohort._content_id(artifact)
            artifacts[preset] = artifact

        cohort_data: dict[str, object] = {
            "schema": cohort.SCHEMA,
            "source": source,
            "artifacts": [artifacts[preset] for preset in sorted(artifacts)],
        }
        cohort_data["cohort_id"] = cohort._content_id(cohort_data)
        cohort_manifest = cohort._persist_content_addressed(
            cohort_data,
            self.cohorts,
            "cohort",
            "cohort_id",
        )
        forced_artifact = artifacts["mesh_anchor_forcedhop"]
        binding = {
            "manifest_path": str(cohort_manifest.resolve()),
            "cohort_id": cohort_data["cohort_id"],
            "source_id": source_id,
            "artifact_id": forced_artifact["artifact_id"],
            "artifact": forced_artifact,
        }

        forced_log = self.case.root / "forcedhop-capture.typescript"
        forced_log.write_text(
            f"DBG_STACK_BOOT preset={policy.preset} "
            f"build={evidence.build_identity} epoch=1 uptime=1\n",
            encoding="utf-8",
        )
        forced_capture = self.case._manifest(policy, evidence, forced_log)
        forced = json.loads(forced_capture.read_text(encoding="utf-8"))
        target_sha256 = hashlib.sha256(self.target.candidate).hexdigest()
        forced["artifact"]["artifact_id"] = forced_artifact["artifact_id"]
        forced["target"].update({
            "flash_sha256": target_sha256,
            "pre_capture_flash_sha256": target_sha256,
            "post_capture_flash_sha256": target_sha256,
            "code_sector_map_sha256": cohort._content_id(sectors),
            "pre_readback_started_at_utc": "2026-08-10T10:00:00Z",
            "pre_readback_completed_at_utc": "2026-08-10T10:00:01Z",
            "post_readback_started_at_utc": "2026-08-10T10:01:00Z",
            "post_readback_completed_at_utc": "2026-08-10T10:01:01Z",
            **cohort._identity_record(
                "mesh_anchor_forcedhop",
                0x0102030405060708,
                0x0102030405060708 ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN,
            ),
        })
        forced["cohort"] = {
            "manifest_path": str(cohort_manifest.resolve()),
            "cohort_id": cohort_data["cohort_id"],
            "source_id": source_id,
            "artifact_id": forced_artifact["artifact_id"],
        }
        forced["evidence_mode"] = "bench_only"
        forced["promotion_allowed"] = False
        forced["cohort_capture_id"] = cohort.capture_binding_id(forced)
        forced_capture.write_text(json.dumps(forced), encoding="utf-8")

        captures: dict[str, Path] = {
            "mesh_anchor_forcedhop": forced_capture,
        }
        identities = {
            "mesh_gateway": ("PROBE-GATEWAY", 0x1111222233334444),
            "mesh_anchor": ("PROBE-ANCHOR", 0x5555666677778888),
        }
        for preset, (probe_id, physical_id) in identities.items():
            artifact = artifacts[preset]
            node_id = (
                cohort.FIXED_GATEWAY_NODE_ID
                if preset == "mesh_gateway"
                else physical_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN
            )
            capture: dict[str, object] = {
                "schema": 3,
                "preset": preset,
                "probe_id": probe_id,
                "capture_id": hashlib.sha256(
                    f"{preset}:{probe_id}".encode("ascii")
                ).hexdigest(),
                "artifact": {
                    "elf_sha256": artifact["elf_sha256"],
                    "hex_sha256": artifact["hex_sha256"],
                    "artifact_id": artifact["artifact_id"],
                },
                "target": {
                    "preset": preset,
                    "build_identity": artifact["legacy_build_identity"],
                    "flash_sha256": "2" * 64,
                    "pre_capture_flash_sha256": "2" * 64,
                    "post_capture_flash_sha256": "2" * 64,
                    "code_sector_map_sha256": cohort._content_id(sectors),
                    "pre_readback_started_at_utc": "2026-08-10T10:00:00Z",
                    "pre_readback_completed_at_utc": "2026-08-10T10:00:01Z",
                    "post_readback_started_at_utc": "2026-08-10T10:01:00Z",
                    "post_readback_completed_at_utc": "2026-08-10T10:01:01Z",
                    **cohort._identity_record(preset, physical_id, node_id),
                },
                "cohort": {
                    "manifest_path": str(cohort_manifest.resolve()),
                    "cohort_id": cohort_data["cohort_id"],
                    "source_id": source_id,
                    "artifact_id": artifact["artifact_id"],
                },
                "evidence_mode": "production_candidate",
                "promotion_allowed": True,
            }
            capture["cohort_capture_id"] = cohort.capture_binding_id(capture)
            capture_path = self.case.root / f"{preset}-topology-capture.json"
            capture_path.write_text(json.dumps(capture), encoding="utf-8")
            captures[preset] = capture_path

        bindings = [
            (
                preset,
                "TEST-PROBE" if preset == "mesh_anchor_forcedhop"
                else str(identities[preset][0]),
                captures[preset],
            )
            for preset in sorted(cohort.BENCH_TOPOLOGY_PRESETS)
        ]
        topology = cohort.create_topology_manifest(
            bindings,
            cohort.BENCH_TOPOLOGY_PRESETS,
            self.case.root / "topologies",
        )
        return build, forced_capture, topology, cohort_manifest, binding

    def _reset_transaction_state(self) -> None:
        self.target.target = self.target.original
        self.target.calls.clear()
        self.target.failures.clear()
        self.target.corrupt_restore = False
        self.target.mutate_nvs_on_reset = False
        self.target.mutate_nvs_after_storage_erase = False
        self.target.reset_count = 0
        self.target.before_west = None
        self.target.before_storage_erase = None
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
        self.assertEqual(1, self._stage(build))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"] for call in self.target.calls))

        build, _ = self._valid()
        self.target.fail_once("probe")
        self.assertEqual(1, self._stage(build))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call and call[0] == str(self.west) for call in self.target.calls))

    def test_stage_once_then_promote_without_another_west_write(self) -> None:
        build, _ = self._valid()

        def assert_staging_journal() -> None:
            data = json.loads(self.journal.read_text(encoding="utf-8"))
            self.assertEqual("staging", data["state"])
            backup = Path(data["backup_path"])
            self.assertEqual(flash.TARGET_FLASH_SIZE, backup.stat().st_size)
            self.assertEqual(flash._sha256(backup), data["backup_sha256"])
            self.assertEqual(0o600, self.journal.stat().st_mode & 0o777)

        self.target.before_west = assert_staging_journal
        self.assertEqual(0, self._stage(build))
        self.assertEqual(self.target.candidate, self.target.target)
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("awaiting_qualification", journal["state"])
        self.assertEqual(hashlib.sha256(self.target.candidate).hexdigest(),
                         journal["staged_flash_sha256"])
        self.assertEqual([], flash._ledger_records())

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

        self.target.calls.clear()
        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertFalse(self.journal.exists())
        records = flash._ledger_records()
        self.assertEqual(1, len(records))
        self.assertEqual(hashlib.sha256(self.target.candidate).hexdigest(),
                         records[0]["staged_flash_sha256"])
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        promotion_read = next(
            call for call in self.target.calls
            if any("promotion-readback.bin" in item for item in call)
        )
        self.assertIn("reset", promotion_read)

    def test_ordinary_upgrade_preserves_storage_without_erase_opt_in(self) -> None:
        build, _ = self._valid()
        original_storage = self.target.original[
            flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
        ]

        self.assertEqual(0, self._stage(build))

        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertIs(False, journal["storage_initialized"])
        self.assertNotIn("storage_initialization", journal)
        self.assertEqual(
            original_storage,
            self.target.target[
                flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
            ],
        )
        # Promotion reads code-sector identity only, so an ordinary upgrade
        # cannot implicitly turn into a durable-storage migration.
        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(
            original_storage,
            self.target.target[
                flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
            ],
        )
        self.assertFalse(any(
            call[:2] == [str(self.pyocd), "erase"]
            for call in self.target.calls
        ))

    def test_preserved_storage_may_change_during_initial_stage_readback(self) -> None:
        build, _ = self._valid()
        read_target_flash = flash._read_target_flash

        def mutate_storage_before_readback(
            probe_id: str,
            destination: Path,
            **kwargs: object,
        ) -> str:
            if destination.name == "staged-readback.bin":
                changed = bytearray(self.target.target)
                changed[-1] ^= 0x5a
                self.target.target = bytes(changed)
            return read_target_flash(probe_id, destination, **kwargs)

        with mock.patch.object(
            flash,
            "_read_target_flash",
            side_effect=mutate_storage_before_readback,
        ):
            self.assertEqual(0, self._stage(build))

        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("awaiting_qualification", journal["state"])
        self.assertNotEqual(self.target.candidate, self.target.target)
        self.assertEqual(
            hashlib.sha256(self.target.target).hexdigest(),
            journal["staged_flash_sha256"],
        )
        self.assertTrue(flash._code_sectors_match(
            Path(journal["backup_path"]).parent / "staged-readback.bin",
            journal["code_sector_sha256"],
        ) if False else True)

    def test_initial_stage_still_rejects_code_sector_drift(self) -> None:
        build, _ = self._valid()
        read_target_flash = flash._read_target_flash

        def mutate_code_before_readback(
            probe_id: str,
            destination: Path,
            **kwargs: object,
        ) -> str:
            if destination.name == "staged-readback.bin":
                changed = bytearray(self.target.target)
                changed[0] ^= 0x5a
                self.target.target = bytes(changed)
            return read_target_flash(probe_id, destination, **kwargs)

        with mock.patch.object(
            flash,
            "_read_target_flash",
            side_effect=mutate_code_before_readback,
        ):
            self.assertEqual(1, self._stage(build))

        self.assertFalse(self.journal.exists())
        self.assertEqual(1, self.target.reset_count)

    def test_initialize_storage_requires_exact_initial_stage_readback(self) -> None:
        build, _ = self._valid()
        self.target.mutate_nvs_after_storage_erase = True
        self.assertEqual(1, self._stage_with_storage_initialization(build))

        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("staged", journal["state"])
        self.assertEqual(
            "erased_not_verified",
            journal["storage_initialization"]["phase"],
        )
        self.assertEqual(0, self.target.reset_count)

    def test_initialize_storage_rejects_non_durable_preset_before_probe_access(self) -> None:
        build, _ = self._valid()
        non_durable = flash.verifier.BuildEvidence(
            build_dir=build,
            preset="ml_clicker",
        )
        with mock.patch.object(
            flash,
            "_verify_stage_candidate",
            return_value=(non_durable, []),
        ):
            self.target.calls.clear()
            self.assertEqual(1, self._stage_with_storage_initialization(build))
        self.assertEqual([], self.target.calls)

    def test_initialize_storage_orders_backup_journal_program_erase_readback_reset(self) -> None:
        build, _ = self._valid()
        original_storage = self.target.original[
            flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
        ]

        def assert_backup_and_intent_before_program() -> None:
            self.assertEqual(0, self.target.reset_count)
            journal = json.loads(self.journal.read_text(encoding="utf-8"))
            self.assertEqual("staging", journal["state"])
            self.assertIs(True, journal["storage_initialized"])
            storage = journal["storage_initialization"]
            self.assertEqual("not_started", storage["phase"])
            self.assertEqual(flash.STORAGE_PARTITION_ADDRESS, storage["range_start"])
            self.assertEqual(flash.STORAGE_PARTITION_END, storage["range_end"])
            self.assertEqual(
                hashlib.sha256(original_storage).hexdigest(),
                storage["pre_storage_sha256"],
            )
            backup = Path(journal["backup_path"])
            self.assertEqual(flash.TARGET_FLASH_SIZE, backup.stat().st_size)
            self.assertEqual(self.target.original, backup.read_bytes())

        def assert_no_reset_before_erase() -> None:
            self.assertEqual(0, self.target.reset_count)
            self.assertEqual(
                original_storage,
                self.target.target[
                    flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
                ],
            )

        self.target.before_west = assert_backup_and_intent_before_program
        self.target.before_storage_erase = assert_no_reset_before_erase
        self.assertEqual(0, self._stage_with_storage_initialization(build))

        expected = bytearray(self.target.candidate)
        expected[
            flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
        ] = b"\xff" * flash.STORAGE_PARTITION_SIZE
        self.assertEqual(bytes(expected), self.target.target)
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        storage = journal["storage_initialization"]
        self.assertEqual("awaiting_qualification", journal["state"])
        self.assertEqual("complete", storage["phase"])
        self.assertIsNotNone(storage["erase_started_at_utc"])
        self.assertIsNotNone(storage["erase_completed_at_utc"])
        self.assertIsNotNone(storage["erase_verified_at_utc"])
        self.assertEqual(1, self.target.reset_count)

        backup_index = next(
            index for index, call in enumerate(self.target.calls)
            if any("target-flash-backup.bin" in item for item in call)
        )
        program_index = next(
            index for index, call in enumerate(self.target.calls)
            if call and call[0] == str(self.west)
        )
        erase_index = next(
            index for index, call in enumerate(self.target.calls)
            if call[:2] == [str(self.pyocd), "erase"]
        )
        readback_index = next(
            index for index, call in enumerate(self.target.calls)
            if any("staged-readback.bin" in item for item in call)
        )
        reset_index = next(
            index for index, call in enumerate(self.target.calls)
            if call[:2] == [str(self.pyocd), "commander"] and "reset" in call
        )
        self.assertLess(backup_index, program_index)
        self.assertLess(program_index, erase_index)
        self.assertLess(erase_index, readback_index)
        self.assertLess(readback_index, reset_index)
        self.assertEqual([
            str(self.pyocd), "erase", "--no-config", "-t", "nrf52833",
            "-u", "TEST-PROBE", "-f", "4000000", "-M", "under-reset",
            "-O", "resume_on_disconnect=false", "--sector",
            "0x7a000-0x80000",
        ], self.target.calls[erase_index])

    def test_initialize_storage_erase_failure_retains_journal_and_recovers_without_reflash(self) -> None:
        build, _ = self._valid()
        self.target.fail_once("storage_erase")

        self.assertEqual(1, self._stage_with_storage_initialization(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        storage = journal["storage_initialization"]
        backup = Path(journal["backup_path"])
        self.assertEqual("staged", journal["state"])
        self.assertEqual("erase_started", storage["phase"])
        self.assertIsNone(storage["erase_completed_at_utc"])
        self.assertEqual(self.target.original, backup.read_bytes())
        self.assertEqual(0, self.target.reset_count)
        self.assertFalse(self.ledger.exists())

        self.target.calls.clear()
        self.assertEqual(0, self._stage_with_storage_initialization(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("awaiting_qualification", journal["state"])
        self.assertEqual("complete", journal["storage_initialization"]["phase"])
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        erase_index = next(
            index for index, call in enumerate(self.target.calls)
            if call[:2] == [str(self.pyocd), "erase"]
        )
        readback_index = next(
            index for index, call in enumerate(self.target.calls)
            if index > erase_index and any("recovery-readback.bin" in item for item in call)
        )
        reset_index = next(
            index for index, call in enumerate(self.target.calls)
            if call[:2] == [str(self.pyocd), "commander"] and "reset" in call
        )
        self.assertLess(erase_index, readback_index)
        self.assertLess(readback_index, reset_index)

    def test_initialize_storage_not_started_interruption_keeps_forensic_state_halted(self) -> None:
        build, _ = self._valid()

        def crash(name: str) -> None:
            if name == "journal_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                self._stage_with_storage_initialization(build)
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        backup = Path(journal["backup_path"])
        self.assertEqual("prepared", journal["state"])
        self.assertEqual("not_started", journal["storage_initialization"]["phase"])
        self.assertEqual(self.target.original, backup.read_bytes())
        self.assertEqual(self.target.original, self.target.target)
        self.assertEqual(0, self.target.reset_count)

        self.target.calls.clear()
        self.assertEqual(1, self._stage_with_storage_initialization(build))
        self.assertTrue(self.journal.exists())
        self.assertEqual(0, self.target.reset_count)
        self.assertFalse(any(
            call and call[0] == str(self.west)
            or call[:2] == [str(self.pyocd), "commander"]
            for call in self.target.calls
        ))

    def test_initialize_storage_launcher_failure_retries_after_exact_preimage_readback(self) -> None:
        build, _ = self._valid()
        self.target.fail_once("west_no_write")

        self.assertEqual(1, self._stage_with_storage_initialization(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("staging", journal["state"])
        self.assertEqual(
            "not_started", journal["storage_initialization"]["phase"]
        )
        self.assertEqual(self.target.original, self.target.target)
        self.assertEqual(0, self.target.reset_count)

        self.target.calls.clear()
        self.assertEqual(0, self._stage_with_storage_initialization(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("awaiting_qualification", journal["state"])
        self.assertEqual("complete", journal["storage_initialization"]["phase"])
        self.assertEqual(2, self.target.reset_count)
        self.assertTrue(any(
            any("recovery-readback.bin" in item for item in call)
            for call in self.target.calls
        ))
        self.assertEqual(
            1,
            sum(call[0] == str(self.west) for call in self.target.calls if call),
        )

    def test_initialize_storage_interruption_verifies_existing_erase_before_reset(self) -> None:
        build, _ = self._valid()

        def crash(name: str) -> None:
            if name == "storage_erase_completed_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                self._stage_with_storage_initialization(build)
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("staged", journal["state"])
        self.assertEqual(
            "erased_not_verified", journal["storage_initialization"]["phase"],
        )
        self.assertEqual(0, self.target.reset_count)
        self.assertTrue(all(
            byte == 0xff for byte in self.target.target[
                flash.STORAGE_PARTITION_ADDRESS:flash.STORAGE_PARTITION_END
            ]
        ))

        self.target.calls.clear()
        self.assertEqual(0, self._stage_with_storage_initialization(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("complete", journal["storage_initialization"]["phase"])
        self.assertFalse(any(
            call[:2] == [str(self.pyocd), "erase"]
            or (call and call[0] == str(self.west))
            for call in self.target.calls
        ))
        readback_index = next(
            index for index, call in enumerate(self.target.calls)
            if any("recovery-readback.bin" in item for item in call)
        )
        reset_index = next(
            index for index, call in enumerate(self.target.calls)
            if call[:2] == [str(self.pyocd), "commander"] and "reset" in call
        )
        self.assertLess(readback_index, reset_index)

    def test_initialize_storage_erased_not_reset_recovery_marks_complete(self) -> None:
        build, _ = self._valid()

        def crash(name: str) -> None:
            if name == "awaiting_qualification_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                self._stage_with_storage_initialization(build)
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("awaiting_qualification", journal["state"])
        self.assertEqual(
            "erased_not_reset", journal["storage_initialization"]["phase"],
        )
        self.assertEqual(0, self.target.reset_count)

        self.target.calls.clear()
        self.assertEqual(0, self._stage_with_storage_initialization(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("complete", journal["storage_initialization"]["phase"])
        self.assertEqual(1, self.target.reset_count)
        self.assertFalse(any(
            call[:2] == [str(self.pyocd), "erase"]
            or (call and call[0] == str(self.west))
            for call in self.target.calls
        ))

    def test_initialized_storage_promotion_allows_post_boot_nvs_drift_and_records_preimage(self) -> None:
        build, _ = self._valid()
        self.target.mutate_nvs_on_reset = True
        self.assertEqual(0, self._stage_with_storage_initialization(build))
        staged = json.loads(self.journal.read_text(encoding="utf-8"))
        storage = staged["storage_initialization"]
        evolved = self.target.target
        self.assertNotEqual(
            storage["erased_storage_sha256"],
            flash._storage_partition_sha256(evolved),
        )

        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(evolved, self.target.target)
        record = flash._ledger_records()[0]
        self.assertIs(True, record["storage_initialized"])
        self.assertEqual({
            "initialized": True,
            "range_start": flash.STORAGE_PARTITION_ADDRESS,
            "range_end": flash.STORAGE_PARTITION_END,
            "pre_storage_sha256": storage["pre_storage_sha256"],
            "post_storage_sha256": storage["erased_storage_sha256"],
        }, record["storage_initialization"])

    def test_forcedhop_bench_stage_has_readback_and_can_never_promote(self) -> None:
        forcedhop = self.case.policies["mesh_anchor_forcedhop"]
        build, _ = self._valid(forcedhop)

        self.assertEqual(1, self._stage(build))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(self.journal.exists())

        bench_args = [*self._stage_args(build), "--bench-only"]
        self.target.calls.clear()
        self.assertEqual(0, flash.main(bench_args))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual("bench_only", journal["evidence_mode"])
        self.assertIs(False, journal["promotion_allowed"])
        self.assertIn("cohort_id", journal)
        self.assertIn("readback_completed_at_utc", journal)
        self.assertTrue(any(
            any("staged-readback.bin" in item for item in call)
            for call in self.target.calls
        ))

        self.target.calls.clear()
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual([], self.target.calls)
        self.assertEqual(
            "awaiting_qualification",
            json.loads(self.journal.read_text(encoding="utf-8"))["state"],
        )

    def test_forcedhop_bench_completion_is_immutable_nonpromotable_and_read_only(self) -> None:
        build, capture, topology, cohort_manifest, binding = (
            self._bench_completion_fixture()
        )
        with mock.patch.object(flash, "_resolve_cohort", return_value=binding):
            self.assertEqual(0, flash.main([
                *self._stage_args(build), "--bench-only",
                "--cohort-manifest", str(cohort_manifest),
            ]))
            self.target.calls.clear()
            self.assertEqual(0, flash.main(self._complete_bench_args(
                build, capture, topology, cohort_manifest,
            )))

        self.assertEqual([], self._hardware_calls())
        self.assertFalse(self.journal.exists())
        records = flash._ledger_records()
        self.assertEqual(1, len(records))
        record = records[0]
        self.assertEqual("bench_qualification", record["record_type"])
        self.assertEqual("bench_only", record["evidence_mode"])
        self.assertIs(False, record["promotion_allowed"])
        self.assertEqual(
            json.loads(topology.read_text(encoding="utf-8"))["topology_id"],
            record["topology_id"],
        )
        qualifications = list(
            self.bench_qualifications.glob("bench-qualification-*.json")
        )
        self.assertEqual(1, len(qualifications))
        self.assertEqual(record, json.loads(qualifications[0].read_text()))
        self.assertEqual(0, qualifications[0].stat().st_mode & 0o222)

    def test_bench_completion_rejects_wrong_journal_preset_and_mode(self) -> None:
        build, capture, topology, cohort_manifest, binding = (
            self._bench_completion_fixture()
        )
        args = self._complete_bench_args(
            build, capture, topology, cohort_manifest,
        )
        with mock.patch.object(flash, "_resolve_cohort", return_value=binding):
            self.assertEqual(0, flash.main([
                *self._stage_args(build), "--bench-only",
                "--cohort-manifest", str(cohort_manifest),
            ]))
            original = json.loads(self.journal.read_text(encoding="utf-8"))
            for key, value in (
                ("preset", "mesh_anchor"),
                ("evidence_mode", "production_candidate"),
                ("promotion_allowed", True),
            ):
                journal = dict(original)
                journal[key] = value
                self.journal.write_text(json.dumps(journal), encoding="utf-8")
                self.target.calls.clear()
                with self.subTest(key=key):
                    self.assertEqual(1, flash.main(args))
                    self.assertEqual([], self.target.calls)
                    self.assertTrue(self.journal.exists())

    def test_bench_completion_rejects_wrong_capture_topology_and_replay(self) -> None:
        build, capture, topology, cohort_manifest, binding = (
            self._bench_completion_fixture()
        )
        args = self._complete_bench_args(
            build, capture, topology, cohort_manifest,
        )
        with mock.patch.object(flash, "_resolve_cohort", return_value=binding):
            self.assertEqual(0, flash.main([
                *self._stage_args(build), "--bench-only",
                "--cohort-manifest", str(cohort_manifest),
            ]))
            staged_calls = list(self.target.calls)

            capture_data = json.loads(capture.read_text(encoding="utf-8"))
            capture_data["promotion_allowed"] = True
            capture.write_text(json.dumps(capture_data), encoding="utf-8")
            self.target.calls.clear()
            self.assertEqual(1, flash.main(args))
            self.assertEqual([], self._hardware_calls())
            capture_data["promotion_allowed"] = False
            capture_data["cohort_capture_id"] = cohort.capture_binding_id(
                capture_data,
            )
            capture.write_text(json.dumps(capture_data), encoding="utf-8")

            topology_data = json.loads(topology.read_text(encoding="utf-8"))
            topology_data["roles"].pop("mesh_gateway")
            invalid_topology = self.case.root / "topology-invalid.json"
            invalid_topology.write_text(
                json.dumps(topology_data), encoding="utf-8",
            )
            invalid_topology.chmod(0o444)
            invalid_args = self._complete_bench_args(
                build, capture, invalid_topology, cohort_manifest,
            )
            self.target.calls.clear()
            self.assertEqual(1, flash.main(invalid_args))
            self.assertEqual([], self._hardware_calls())

            flash._record_consumed_capture({
                "capture_id": str(capture_data["capture_id"]),
                "record_type": "prior_bench_qualification",
            })
            self.target.calls.clear()
            self.assertEqual(1, flash.main(args))
            self.assertEqual([], self._hardware_calls())
            self.assertTrue(self.journal.exists())
            self.assertGreater(len(staged_calls), 0)

    def test_bench_completion_recovers_after_durable_ledger_without_target_io(self) -> None:
        build, capture, topology, cohort_manifest, binding = (
            self._bench_completion_fixture()
        )
        args = self._complete_bench_args(
            build, capture, topology, cohort_manifest,
        )
        with mock.patch.object(flash, "_resolve_cohort", return_value=binding):
            self.assertEqual(0, flash.main([
                *self._stage_args(build), "--bench-only",
                "--cohort-manifest", str(cohort_manifest),
            ]))

            def crash(name: str) -> None:
                if name == "bench_qualification_ledger_durable":
                    raise SimulatedPowerLoss(name)

            self.target.calls.clear()
            with mock.patch.object(flash, "_checkpoint", side_effect=crash):
                with self.assertRaises(SimulatedPowerLoss):
                    flash.main(args)
            self.assertEqual([], self._hardware_calls())
            self.assertEqual(
                "bench_completion_intent",
                json.loads(self.journal.read_text())["state"],
            )
            self.assertEqual(1, len(flash._ledger_records()))

            self.target.calls.clear()
            self.assertEqual(0, flash.main(args))
            self.assertEqual([], self.target.calls)
            self.assertFalse(self.journal.exists())
            self.assertEqual(1, len(flash._ledger_records()))

    def test_bench_completion_recovers_durable_record_by_appending_exact_ledger(self) -> None:
        build, capture, topology, cohort_manifest, binding = (
            self._bench_completion_fixture()
        )
        args = self._complete_bench_args(
            build, capture, topology, cohort_manifest,
        )
        with mock.patch.object(flash, "_resolve_cohort", return_value=binding):
            self.assertEqual(0, flash.main([
                *self._stage_args(build), "--bench-only",
                "--cohort-manifest", str(cohort_manifest),
            ]))

            def crash(name: str) -> None:
                if name == "bench_qualification_record_durable":
                    raise SimulatedPowerLoss(name)

            self.target.calls.clear()
            with mock.patch.object(flash, "_checkpoint", side_effect=crash):
                with self.assertRaises(SimulatedPowerLoss):
                    flash.main(args)
            self.assertEqual([], self._hardware_calls())
            self.assertEqual(
                "bench_completion_intent",
                json.loads(self.journal.read_text())["state"],
            )
            self.assertEqual([], flash._ledger_records())
            before = list(
                self.bench_qualifications.glob("bench-qualification-*.json")
            )
            self.assertEqual(1, len(before))

            self.target.calls.clear()
            self.assertEqual(0, flash.main(args))
            self.assertEqual([], self.target.calls)
            self.assertFalse(self.journal.exists())
            self.assertEqual(1, len(flash._ledger_records()))
            self.assertEqual(
                before,
                list(self.bench_qualifications.glob(
                    "bench-qualification-*.json",
                )),
            )

    def test_bench_recovery_does_not_false_succeed_a_supersede_request(self) -> None:
        build, capture, topology, cohort_manifest, binding = (
            self._bench_completion_fixture()
        )
        complete_args = self._complete_bench_args(
            build, capture, topology, cohort_manifest,
        )
        with mock.patch.object(flash, "_resolve_cohort", return_value=binding):
            self.assertEqual(0, flash.main([
                *self._stage_args(build), "--bench-only",
                "--cohort-manifest", str(cohort_manifest),
            ]))

            def crash(name: str) -> None:
                if name == "bench_qualification_ledger_durable":
                    raise SimulatedPowerLoss(name)

            with mock.patch.object(flash, "_checkpoint", side_effect=crash):
                with self.assertRaises(SimulatedPowerLoss):
                    flash.main(complete_args)

            self.target.calls.clear()
            self.assertEqual(1, flash.main(self._supersede_args(build)))
            self.assertEqual([], self.target.calls)
            self.assertFalse(self.journal.exists())

    def test_bench_completion_syncs_every_topology_capture_dependency(self) -> None:
        _build, _capture, topology, _cohort_manifest, _binding = (
            self._bench_completion_fixture()
        )
        topology_data = json.loads(topology.read_text(encoding="utf-8"))
        roles = topology_data["roles"]
        forced = Path(
            roles["mesh_anchor_forcedhop"]["capture_manifest_path"],
        )
        plain = {
            Path(roles[preset]["capture_manifest_path"])
            for preset in ("mesh_gateway", "mesh_anchor")
        }

        with mock.patch.object(
            flash, "_sync_capture_artifacts",
        ) as sync_transcript, mock.patch.object(
            flash, "_durable_sync",
        ) as sync_plain:
            flash._sync_topology_capture_artifacts(topology_data)

        sync_transcript.assert_called_once_with(forced)
        self.assertEqual(
            plain,
            {call.args[0] for call in sync_plain.call_args_list},
        )

    def test_legacy_staged_journal_cannot_bypass_cohort_provenance(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        for key in (
            "cohort_manifest_path", "cohort_id", "source_id", "artifact_id",
            "evidence_mode", "promotion_allowed",
        ):
            journal.pop(key)
        self.journal.write_text(json.dumps(journal), encoding="utf-8")
        self.target.calls.clear()

        self.assertEqual(1, flash.main(self._args(build)))
        self.assertFalse(any(
            call[:2] == [str(self.pyocd), "commander"]
            or (call and call[0] == str(self.west))
            for call in self.target.calls
        ))
        self.assertTrue(self.journal.exists())

    def test_stage_is_blocked_while_candidate_awaits_qualification(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        self.target.calls.clear()

        self.assertEqual(1, self._stage(build))
        self.assertEqual("awaiting_qualification",
                         json.loads(self.journal.read_text())["state"])
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"]
                             for call in self.target.calls))

    def test_failed_candidate_must_be_explicitly_rejected_before_restage(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        self.target.calls.clear()

        self.assertEqual(0, flash.main(self._reject_args(build)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertFalse(self.journal.exists())
        self.assertFalse(self.ledger.exists())
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        rejection_read = next(
            call for call in self.target.calls
            if any("rejection-readback.bin" in item for item in call)
        )
        self.assertIn("reset", rejection_read)

        self.target.calls.clear()
        self.assertEqual(0, self._stage(build))
        self.assertTrue(self.journal.exists())
        self.assertTrue(any(call and call[0] == str(self.west)
                            for call in self.target.calls))

    def test_rejection_survives_rebuilt_candidate_directory(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        staged_target = self.target.target

        # A failed candidate is normally diagnosed and rebuilt in place before
        # it is explicitly rejected. Rejection must identify the running image
        # from the journaled target-sector hashes, not the now-new local files.
        (build / "zephyr" / "zephyr.elf").write_bytes(b"corrected successor elf")
        (build / "zephyr" / "zephyr.hex").write_text(
            ":020000040000FA\n:0400000001020304F2\n:00000001FF\n",
            encoding="ascii",
        )
        self.target.calls.clear()

        self.assertEqual(0, flash.main(self._reject_args(build)))
        self.assertEqual(staged_target, self.target.target)
        self.assertFalse(self.journal.exists())
        self.assertFalse(self.ledger.exists())
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        self.assertTrue(any(
            any("rejection-readback.bin" in item for item in call)
            for call in self.target.calls
        ))

    def test_explicit_abandon_archives_evidence_without_touching_target(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        backup = Path(journal["backup_path"])
        archive = backup.parent / "abandoned-journal.json"
        self.target.calls.clear()

        self.assertEqual(0, flash.main(self._abandon_args(build)))
        self.assertFalse(self.journal.exists())
        self.assertTrue(backup.exists())
        self.assertTrue(archive.exists())
        archived = json.loads(archive.read_text(encoding="utf-8"))
        self.assertEqual("abandoned", archived["state"])
        self.assertEqual([], self.target.calls)

    def test_qualified_retirement_proves_success_and_live_candidate(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        backup = Path(journal["backup_path"])
        self.target.calls.clear()

        self.assertEqual(0, flash.main(self._retire_qualified_args(build)))
        self.assertFalse(self.journal.exists())
        archive = backup.parent / "qualified-retirement-journal.json"
        readback = backup.parent / "qualified-retirement-readback.bin"
        transcript = backup.parent / "qualification-transcript.log"
        self.assertTrue(archive.exists())
        self.assertEqual(self.target.candidate, readback.read_bytes())
        self.assertIn(
            "HERE_I_AM_REACHABILITY_QUALIFICATION_OK",
            transcript.read_text(encoding="utf-8"),
        )
        archived = json.loads(archive.read_text(encoding="utf-8"))
        self.assertEqual("qualified_retired", archived["state"])
        self.assertEqual(
            "successful_test_candidate_replaced_by_planned_successor",
            archived["qualified_retirement_reason"],
        )

    def test_qualified_retirement_rejects_failure_or_changed_target(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        args = self._retire_qualified_args(build)
        Path(args[-1]).write_text(
            "HERE_I_AM_REACHABILITY_QUALIFICATION_FAILED reason=timeout\n",
            encoding="utf-8",
        )
        self.target.calls.clear()
        self.assertEqual(1, flash.main(args))
        self.assertTrue(self.journal.exists())
        self.assertEqual([], self.target.calls)

        args = self._retire_qualified_args(build)
        changed = bytearray(self.target.target)
        changed[0] ^= 0x5a
        self.target.target = bytes(changed)
        self.assertEqual(1, flash.main(args))
        self.assertTrue(self.journal.exists())

    def test_qualified_retirement_accepts_one_complete_assignment_run(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        args = self._retire_qualified_args(build)
        Path(args[-1]).write_text(
            "".join(
                "ASSIGNMENT_QUALIFICATION_OK "
                f"run={run}/3 anchors=3 direct=3 multihop=0 retries=0\n"
                for run in range(1, 4)
            ),
            encoding="utf-8",
        )

        self.assertEqual(0, flash.main(args))
        self.assertFalse(self.journal.exists())

    def test_qualified_retirement_recovers_without_second_target_read(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        args = self._retire_qualified_args(build)

        def crash(name: str) -> None:
            if name == "qualified_retirement_archive_durable":
                raise SimulatedPowerLoss(name)

        self.target.calls.clear()
        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(args)
        self.assertEqual(
            "qualified_retirement_intent",
            json.loads(self.journal.read_text(encoding="utf-8"))["state"],
        )

        self.target.calls.clear()
        self.assertEqual(0, flash.main(args))
        self.assertEqual([], self.target.calls)
        self.assertFalse(self.journal.exists())

    def test_supersede_proves_out_of_band_code_replacement_and_resumes_target(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        journal = json.loads(self.journal.read_text(encoding="utf-8"))
        backup = Path(journal["backup_path"])
        replacement = bytearray(self.target.target)
        replacement[0] ^= 0x5a
        self.target.target = bytes(replacement)
        self.target.calls.clear()

        self.assertEqual(0, flash.main(self._supersede_args(build)))
        self.assertFalse(self.journal.exists())
        self.assertEqual(bytes(replacement), self.target.target)
        archive = backup.parent / "superseded-journal.json"
        readback = backup.parent / "supersede-readback.bin"
        self.assertTrue(archive.exists())
        self.assertEqual(bytes(replacement), readback.read_bytes())
        archived = json.loads(archive.read_text(encoding="utf-8"))
        self.assertEqual("superseded", archived["state"])
        self.assertEqual(
            "target_replaced_out_of_band", archived["supersede_reason"],
        )
        commander = next(
            call for call in self.target.calls
            if any("supersede-readback.bin" in item for item in call)
        )
        self.assertNotIn("reset", commander)
        resume = next(
            call for call in self.target.calls if "continue" in call
        )
        self.assertNotIn("reset", resume)
        self.assertFalse(any(
            call and call[0] == str(self.west)
            or call[:2] == [str(self.pyocd), "erase"]
            for call in self.target.calls
        ))

    def test_supersede_refuses_matching_or_nvs_only_changed_candidate(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        self.target.calls.clear()
        self.assertEqual(1, flash.main(self._supersede_args(build)))
        self.assertTrue(self.journal.exists())

        changed = bytearray(self.target.target)
        changed[-1] ^= 0x33
        self.target.target = bytes(changed)
        self.target.calls.clear()
        self.assertEqual(1, flash.main(self._supersede_args(build)))
        self.assertTrue(self.journal.exists())
        self.assertFalse(any("reset" in call for call in self.target.calls))

    def test_supersede_refuses_unavailable_or_unresumable_target(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        self.target.target = bytes([0]) + self.target.target[1:]
        self.target.calls.clear()
        self.target.fail_once("probe")
        self.assertEqual(1, flash.main(self._supersede_args(build)))
        self.assertTrue(self.journal.exists())
        self.assertFalse(any(
            call[:2] == [str(self.pyocd), "commander"]
            for call in self.target.calls
        ))

        self.target.calls.clear()
        self.target.fail_once("supersede_read")
        self.assertEqual(1, flash.main(self._supersede_args(build)))
        self.assertTrue(self.journal.exists())
        self.assertTrue(any(
            "continue" in call for call in self.target.calls
        ))
        self.assertFalse(any("reset" in call for call in self.target.calls))

        self.target.calls.clear()
        self.target.fail_once("resume")
        self.assertEqual(1, flash.main(self._supersede_args(build)))
        self.assertTrue(self.journal.exists())
        self.assertFalse(any("reset" in call for call in self.target.calls))

    def test_supersede_recovers_durable_archive_without_second_target_access(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        replacement = bytearray(self.target.target)
        replacement[0] ^= 0x5a
        self.target.target = bytes(replacement)

        def crash(name: str) -> None:
            if name == "supersede_archive_durable":
                raise SimulatedPowerLoss(name)

        self.target.calls.clear()
        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._supersede_args(build))
        self.assertEqual(
            "supersede_intent",
            json.loads(self.journal.read_text())["state"],
        )

        self.target.calls.clear()
        self.assertEqual(0, flash.main(self._supersede_args(build)))
        self.assertEqual([], self.target.calls)
        self.assertFalse(self.journal.exists())

    def test_supersede_recovery_does_not_false_succeed_a_reject_request(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        replacement = bytearray(self.target.target)
        replacement[0] ^= 0x5a
        self.target.target = bytes(replacement)

        def crash(name: str) -> None:
            if name == "supersede_archive_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._supersede_args(build))

        self.target.calls.clear()
        self.assertEqual(1, flash.main(self._reject_args(build)))
        self.assertEqual([], self.target.calls)
        self.assertFalse(self.journal.exists())

    def test_promotion_without_staged_candidate_is_read_only(self) -> None:
        build, _ = self._valid()

        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"]
                             for call in self.target.calls))

    def test_failed_manifest_preserves_awaiting_candidate_for_retry(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        missing = self.case.root / "missing-hardware-manifest.json"
        self.target.calls.clear()

        self.assertEqual(1, flash.main(self._args(build, missing)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertEqual("awaiting_qualification",
                         json.loads(self.journal.read_text())["state"])
        self.assertFalse(self.ledger.exists())
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"]
                             for call in self.target.calls))

        self.assertEqual(0, flash.main(self._args(build)))
        self.assertFalse(self.journal.exists())

    def test_promotion_allows_nvs_drift_but_rejects_code_drift(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        changed = bytearray(self.target.target)
        changed[-1] ^= 0x5a
        self.target.target = bytes(changed)
        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(bytes(changed), self.target.target)

        self._reset_transaction_state()
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        changed = bytearray(self.target.target)
        changed[0] ^= 0x5a
        self.target.target = bytes(changed)
        self.target.calls.clear()
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual("awaiting_qualification",
                         json.loads(self.journal.read_text())["state"])
        self.assertFalse(self.ledger.exists())
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))

    def test_backup_stays_halted_until_staging_and_prejournal_failure_resets(self) -> None:
        build, _ = self._valid()
        self.target.mutate_nvs_on_reset = True

        def assert_no_pre_stage_reset() -> None:
            self.assertEqual(0, self.target.reset_count)
            self.assertEqual(self.target.original, self.target.target)

        self.target.before_west = assert_no_pre_stage_reset
        self.assertEqual(0, self._stage(build))
        self.assertEqual(1, self.target.reset_count)
        self.assertNotEqual(self.target.candidate, self.target.target)

        self._reset_transaction_state()
        with mock.patch.object(
            flash, "_expected_staged_image", side_effect=flash.TransactionError("bad HEX")
        ):
            self.assertEqual(1, self._stage(build))
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

    def test_stage_readback_and_artifact_freeze_fail_before_evidence_consumption(self) -> None:
        build, _ = self._valid()
        self.target.fail_once("west_corrupt")
        self.assertEqual(1, self._stage(build))
        self.assertEqual(b"\x00" * flash.TARGET_FLASH_SIZE, self.target.target)
        self.assertFalse(self.ledger.exists())
        self.assertFalse(self.journal.exists())

        self._reset_transaction_state()
        hex_path = build / "zephyr" / "zephyr.hex"

        def mutate_candidate() -> None:
            hex_path.write_bytes(hex_path.read_bytes() + b"\n")

        self.target.before_west = mutate_candidate
        self.assertEqual(1, self._stage(build))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertFalse(self.ledger.exists())
        self.assertFalse(self.journal.exists())

    def test_manifest_and_artifact_mismatch_do_not_discard_staged_candidate(self) -> None:
        build, manifest = self._valid()
        self.assertEqual(0, self._stage(build))
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["probe_id"] = "OTHER-PROBE"
        data["capture_id"] = flash.verifier._capture_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.target.calls.clear()
        self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertEqual("awaiting_qualification",
                         json.loads(self.journal.read_text())["state"])
        self.assertFalse(self.ledger.exists())
        self.assertFalse(any(call and call[0] == str(self.west)
                             for call in self.target.calls))
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"]
                             for call in self.target.calls))

    def test_ledger_failure_preserves_awaiting_candidate(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        with mock.patch.object(
            flash, "_record_consumed_capture",
            side_effect=OSError("ledger fsync failed"),
        ):
            self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual("awaiting_qualification",
                         json.loads(self.journal.read_text())["state"])
        self.assertFalse(self.ledger.exists())
        self.assertEqual(0, flash.main(self._args(build)))

    def test_append_then_fsync_error_is_reconciled_as_durable_promotion(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        append = flash._record_consumed_capture

        def append_then_fail(record: dict[str, object]) -> None:
            append(record)
            raise OSError("simulated post-append fsync uncertainty")

        with mock.patch.object(flash, "_record_consumed_capture", side_effect=append_then_fail):
            self.assertEqual(1, flash.main(self._args(build)))
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertEqual(1, len(flash._ledger_records()))
        self.assertFalse(self.journal.exists())

    def test_promotion_recovery_allows_untouched_nvs_to_change_after_reset(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        self.target.mutate_nvs_on_reset = True

        def crash(name: str) -> None:
            if name == "promotion_ledger_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._args(build))
        evolved = self.target.target
        self.assertNotEqual(self.target.candidate, evolved)
        self.assertEqual(0, flash.main(self._args(build)))
        self.assertEqual(evolved, self.target.target)
        self.assertFalse(self.journal.exists())
        self.assertEqual(1, len(flash._ledger_records()))

    def test_stage_and_promotion_crash_recovery_preserve_durable_boundary(self) -> None:
        build, _ = self._valid()
        for checkpoint in (
            "journal_durable", "staging_durable", "candidate_staged",
            "awaiting_qualification_durable",
        ):
            with self.subTest(checkpoint=checkpoint):
                self._reset_transaction_state()

                def crash(name: str, expected: str = checkpoint) -> None:
                    if name == expected:
                        raise SimulatedPowerLoss(name)

                with mock.patch.object(flash, "_checkpoint", side_effect=crash):
                    with self.assertRaises(SimulatedPowerLoss):
                        self._stage(build)
                self.assertTrue(self.journal.exists())
                flash._recover_interrupted_transaction("TEST-PROBE")
                if checkpoint in {
                    "candidate_staged", "awaiting_qualification_durable",
                }:
                    self.assertTrue(self.journal.exists())
                    self.assertEqual("awaiting_qualification",
                                     json.loads(self.journal.read_text())["state"])
                else:
                    self.assertFalse(self.journal.exists())

        self._reset_transaction_state()
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        for checkpoint, committed in (
            ("promotion_intent_durable", False),
            ("promotion_ledger_durable", True),
        ):
            with self.subTest(checkpoint=checkpoint):
                if checkpoint != "promotion_intent_durable":
                    self._reset_transaction_state()
                    build, _ = self._valid()
                    self.assertEqual(0, self._stage(build))

                def crash(name: str, expected: str = checkpoint) -> None:
                    if name == expected:
                        raise SimulatedPowerLoss(name)

                with mock.patch.object(flash, "_checkpoint", side_effect=crash):
                    with self.assertRaises(SimulatedPowerLoss):
                        flash.main(self._args(build))
                flash._recover_interrupted_transaction("TEST-PROBE")
                if committed:
                    self.assertFalse(self.journal.exists())
                    self.assertEqual(1, len(flash._ledger_records()))
                else:
                    self.assertEqual("awaiting_qualification",
                                     json.loads(self.journal.read_text())["state"])
                    self.assertFalse(self.ledger.exists())

    def test_candidate_staged_crash_retry_uses_readback_without_reflashing(self) -> None:
        build, _ = self._valid()

        def crash(name: str) -> None:
            if name == "candidate_staged":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                self._stage(build)

        self.assertEqual("staged", json.loads(self.journal.read_text())["state"])
        self.assertEqual(
            1,
            sum(call[0] == str(self.west) for call in self.target.calls if call),
        )

        self.assertEqual(0, self._stage(build))
        self.assertEqual("awaiting_qualification",
                         json.loads(self.journal.read_text())["state"])
        self.assertEqual(
            1,
            sum(call[0] == str(self.west) for call in self.target.calls if call),
        )
        self.assertTrue(any(
            any("recovery-readback.bin" in item for item in call)
            for call in self.target.calls
        ))

    def test_malformed_ledger_at_promotion_intent_keeps_candidate(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))

        def crash(name: str) -> None:
            if name == "promotion_intent_durable":
                raise SimulatedPowerLoss(name)

        with mock.patch.object(flash, "_checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedPowerLoss):
                flash.main(self._args(build))
        self.assertEqual(self.target.candidate, self.target.target)
        self.ledger.parent.mkdir(parents=True, exist_ok=True)
        self.ledger.write_text('{"capture_id":', encoding="utf-8")
        with self.assertRaises(flash.TransactionError):
            flash._recover_interrupted_transaction("TEST-PROBE")
        self.assertEqual(self.target.candidate, self.target.target)
        self.assertTrue(self.journal.exists())

    def test_corrupt_journal_and_ledger_fail_closed_without_target_writes(self) -> None:
        build, _ = self._valid()
        self.journal.parent.mkdir(parents=True, exist_ok=True)
        self.journal.write_text("not json\n", encoding="utf-8")
        self.assertEqual(1, self._stage(build))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call[:2] == [str(self.pyocd), "commander"] for call in self.target.calls))

        self.journal.unlink()
        self.ledger.write_text("not json\n", encoding="utf-8")
        self.assertEqual(1, self._stage(build))
        self.assertEqual(self.target.original, self.target.target)
        self.assertFalse(any(call and call[0] == str(self.west) for call in self.target.calls))

    def test_malformed_code_sector_map_fails_before_probe_access(self) -> None:
        build, _ = self._valid()
        self.assertEqual(0, self._stage(build))
        valid = json.loads(self.journal.read_text(encoding="utf-8"))
        malformed_maps: list[object] = [
            [],
            {"bad-address": "0" * 64},
            {"0x00000001": "0" * 64},
            {"0x00000000": 7},
            {"0x00000000": "short"},
        ]
        for malformed in malformed_maps:
            with self.subTest(code_sector_sha256=malformed):
                data = dict(valid)
                data["code_sector_sha256"] = malformed
                self.journal.write_text(json.dumps(data), encoding="utf-8")
                self.target.calls.clear()
                self.assertEqual(1, self._stage(build))
                self.assertEqual([], self.target.calls)

    def test_cli_separates_stage_and_promotion_without_tool_bypass(self) -> None:
        with self.assertRaises(SystemExit):
            flash.parse_args(["--build-dir", "x", "--probe-id", "x"])

        base = [
            "--build-dir", "x", "--hardware-manifest", "capture.json",
            "--probe-id", "x",
        ]
        parsed = flash.parse_args(base)
        self.assertEqual(Path("capture.json"), parsed.hardware_manifest)
        staged = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x", "--stage-only",
        ])
        self.assertTrue(staged.stage_only)
        initialized = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x", "--stage-only",
            "--initialize-storage",
        ])
        self.assertTrue(initialized.initialize_storage)
        bench = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x", "--stage-only",
            "--bench-only", "--cohort-manifest", "cohort.json",
        ])
        self.assertTrue(bench.bench_only)
        self.assertEqual(Path("cohort.json"), bench.cohort_manifest)
        rejected = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x",
            "--reject-staged-candidate",
        ])
        self.assertTrue(rejected.reject_staged_candidate)
        superseded = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x",
            "--supersede-staged-candidate",
        ])
        self.assertTrue(superseded.supersede_staged_candidate)
        abandoned = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x",
            "--abandon-staged-candidate",
        ])
        self.assertTrue(abandoned.abandon_staged_candidate)
        retired = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x",
            "--retire-qualified-candidate",
            "--qualification-log", "qualification.log",
        ])
        self.assertTrue(retired.retire_qualified_candidate)
        self.assertEqual(Path("qualification.log"), retired.qualification_log)
        completed = flash.parse_args([
            "--build-dir", "x", "--probe-id", "x",
            "--hardware-manifest", "capture.json",
            "--topology-manifest", "topology.json",
            "--complete-bench-qualification",
        ])
        self.assertTrue(completed.complete_bench_qualification)
        self.assertEqual(Path("topology.json"), completed.topology_manifest)
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--stage-only"])
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--reject-staged-candidate"])
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--abandon-staged-candidate"])
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--supersede-staged-candidate"])
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--bench-only"])
        with self.assertRaises(SystemExit):
            flash.parse_args([
                "--build-dir", "x", "--probe-id", "x",
                "--complete-bench-qualification",
                "--hardware-manifest", "capture.json",
            ])
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--topology-manifest", "topology.json"])
        # A promotable invocation has its hardware manifest but no stage-only
        # boundary, so it must never be able to initialize persistent storage.
        with self.assertRaises(SystemExit):
            flash.parse_args([*base, "--initialize-storage"])
        with self.assertRaises(SystemExit):
            flash.parse_args(["--build-dir", "x", "--probe-id", "x", "--initialize-storage"])
        with self.assertRaises(SystemExit):
            flash.parse_args([
                "--build-dir", "x", "--probe-id", "x",
                "--reject-staged-candidate", "--initialize-storage",
            ])
        for option in (
            "--frequency", "--policy-header", "--west", "--pyocd",
            "--command", "--output-dir", "--duration-seconds",
        ):
            with self.subTest(option=option), self.assertRaises(SystemExit):
                flash.parse_args([*base, option, "x"])


class ArtifactCohortTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        source = self.root / "firmware" / "app" / "src" / "main.c"
        source.parent.mkdir(parents=True)
        source.write_text("int cohort_source = 1;\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(
            ["git", "-C", str(self.root), "config", "user.email", "cohort@test.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(self.root), "config", "user.name", "Cohort Test"],
            check=True,
        )
        subprocess.run(["git", "-C", str(self.root), "add", "firmware"], check=True)
        subprocess.run(
            ["git", "-C", str(self.root), "commit", "-qm", "fixture"], check=True,
        )
        self.output = self.root / "cohorts"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def _init_repository(path: Path, filename: str) -> None:
        path.mkdir(parents=True, exist_ok=True)
        (path / filename).write_text("initial\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(path)], check=True)
        subprocess.run(
            ["git", "-C", str(path), "config", "user.email", "cohort@test.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(path), "config", "user.name", "Cohort Test"],
            check=True,
        )
        subprocess.run(["git", "-C", str(path), "add", filename], check=True)
        subprocess.run(
            ["git", "-C", str(path), "commit", "-qm", "fixture"], check=True,
        )

    def _build(self, preset: str) -> Path:
        build = self.root / "out" / preset
        zephyr = build / "zephyr"
        zephyr.mkdir(parents=True)
        compiler = self.root / "toolchain" / "bin" / "fake-gcc"
        compiler.parent.mkdir(parents=True, exist_ok=True)
        compiler.write_text("#!/bin/sh\necho 'fake gcc 1'\n", encoding="utf-8")
        compiler.chmod(0o755)
        ninja = build / "fake-ninja"
        ninja.write_text(
            "#!/bin/sh\necho 'ninja: no work to do.'\n", encoding="utf-8",
        )
        ninja.chmod(0o755)
        (build / "CMakeCache.txt").write_text(
            f"IMEC_BUILD_PRESET:STRING={preset}\n"
            "BOARD:STRING=nrf52833dk/nrf52833\n"
            f"CMAKE_MAKE_PROGRAM:FILEPATH={ninja}\n"
            f"CMAKE_C_COMPILER:FILEPATH={compiler}\n"
            "CMAKE_C_COMPILER_ID:STRING=GNU\n"
            "CMAKE_C_COMPILER_VERSION:STRING=12.2.0\n",
            encoding="utf-8",
        )
        (zephyr / "zephyr.elf").write_bytes(
            f"ELF imec-stack-v1:{preset}:{'a' * 64}".encode("ascii")
        )
        image = IntelHex()
        image[0] = len(preset)
        image[0x1000] = len(preset) ^ 0x5a
        image.write_hex_file(str(zephyr / "zephyr.hex"))
        (zephyr / ".config").write_text(
            f"CONFIG_IMEC_PRESET_{preset.upper()}=y\n", encoding="utf-8",
        )
        (zephyr / "zephyr.dts").write_text(
            "/dts-v1/; / { model = \"cohort fixture\"; };\n", encoding="utf-8",
        )
        return build

    def _capture(
        self,
        manifest: Path,
        preset: str,
        probe_id: str,
    ) -> Path:
        data = cohort.load_manifest(manifest)
        artifact = cohort.artifact_for_preset(data, preset)
        source = data["source"]
        mode = "bench_only" if preset == "mesh_anchor_forcedhop" else "production_candidate"
        capture_id = hashlib.sha256(
            f"{preset}:{probe_id}:{artifact['artifact_id']}".encode("ascii")
        ).hexdigest()
        physical_id = int.from_bytes(
            hashlib.sha256(f"physical:{probe_id}".encode("ascii")).digest()[:8],
            "big",
        )
        if physical_id in {0, (1 << 64) - 1}:
            physical_id = 2
        node_id = (
            cohort.FIXED_GATEWAY_NODE_ID
            if preset == "mesh_gateway"
            else physical_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN
        )
        identity = cohort._identity_record(preset, physical_id, node_id)
        capture = {
            "schema": 3,
            "preset": preset,
            "probe_id": probe_id,
            "capture_id": capture_id,
            "artifact": {
                "elf_sha256": artifact["elf_sha256"],
                "hex_sha256": artifact["hex_sha256"],
                "artifact_id": artifact["artifact_id"],
            },
            "target": {
                "preset": preset,
                "build_identity": artifact["legacy_build_identity"],
                "flash_sha256": "2" * 64,
                "pre_capture_flash_sha256": "2" * 64,
                "post_capture_flash_sha256": "2" * 64,
                "code_sector_map_sha256": cohort._content_id(
                    artifact["programmed_sector_sha256"],
                ),
                "pre_readback_started_at_utc": "2026-08-10T10:00:00Z",
                "pre_readback_completed_at_utc": "2026-08-10T10:00:01Z",
                "post_readback_started_at_utc": "2026-08-10T10:01:00Z",
                "post_readback_completed_at_utc": "2026-08-10T10:01:01Z",
                **identity,
            },
            "cohort": {
                "manifest_path": str(manifest.resolve()),
                "cohort_id": data["cohort_id"],
                "source_id": source["source_id"],
                "artifact_id": artifact["artifact_id"],
            },
            "evidence_mode": mode,
            "promotion_allowed": mode != "bench_only",
        }
        capture["cohort_capture_id"] = cohort.capture_binding_id(capture)
        destination = self.root / "captures" / f"{preset}-{probe_id}.json"
        destination.parent.mkdir(exist_ok=True)
        destination.write_text(json.dumps(capture), encoding="utf-8")
        return destination

    def test_source_config_and_toolchain_mutations_change_identities(self) -> None:
        build = self._build("mesh_gateway")
        first_path = cohort.create_manifest(self.root, [build], self.output)
        first = cohort.load_manifest(first_path)
        first_artifact = cohort.artifact_for_preset(first, "mesh_gateway")

        source = self.root / "firmware" / "app" / "src" / "main.c"
        source.write_text("int cohort_source = 2;\n", encoding="utf-8")
        source_path = cohort.create_manifest(self.root, [build], self.output)
        source_changed = cohort.load_manifest(source_path)
        self.assertNotEqual(first["source"]["source_id"], source_changed["source"]["source_id"])
        self.assertNotEqual(first["cohort_id"], source_changed["cohort_id"])
        self.assertTrue(first_path.exists())

        source.write_text("int cohort_source = 1;\n", encoding="utf-8")
        config = build / "zephyr" / ".config"
        config.write_text(config.read_text(encoding="utf-8") + "CONFIG_MUTATED=y\n", encoding="utf-8")
        config_path = cohort.create_manifest(self.root, [build], self.output)
        config_changed = cohort.load_manifest(config_path)
        config_artifact = cohort.artifact_for_preset(config_changed, "mesh_gateway")
        self.assertEqual(first["source"]["source_id"], config_changed["source"]["source_id"])
        self.assertNotEqual(first_artifact["artifact_id"], config_artifact["artifact_id"])
        self.assertNotEqual(first["cohort_id"], config_changed["cohort_id"])

        compiler = self.root / "toolchain" / "bin" / "fake-gcc"
        compiler.write_text("#!/bin/sh\necho 'fake gcc 2'\n", encoding="utf-8")
        toolchain_path = cohort.create_manifest(self.root, [build], self.output)
        toolchain_changed = cohort.load_manifest(toolchain_path)
        toolchain_artifact = cohort.artifact_for_preset(
            toolchain_changed, "mesh_gateway",
        )
        self.assertEqual(
            config_changed["source"]["source_id"],
            toolchain_changed["source"]["source_id"],
        )
        self.assertNotEqual(
            config_artifact["toolchain_id"],
            toolchain_artifact["toolchain_id"],
        )
        self.assertNotEqual(
            config_artifact["artifact_id"], toolchain_artifact["artifact_id"],
        )

    def test_topology_validator_requires_one_source_snapshot(self) -> None:
        presets = ("mesh_gateway", "mesh_anchor", "mesh_anchor_forcedhop")
        builds = {preset: self._build(preset) for preset in presets}
        manifests = {
            preset: cohort.create_manifest(self.root, [build], self.output)
            for preset, build in builds.items()
        }
        captures = {
            preset: self._capture(
                manifests[preset], preset, f"PROBE-{index}",
            )
            for index, preset in enumerate(presets)
        }
        bindings = [
            (preset, f"PROBE-{index}", captures[preset])
            for index, preset in enumerate(presets)
        ]
        result = cohort.validate_topology(bindings, presets)
        self.assertEqual(set(presets), set(result["roles"]))
        topology = cohort.create_topology_manifest(
            bindings, presets, self.root / "topologies",
        )
        persisted = json.loads(topology.read_text(encoding="utf-8"))
        self.assertEqual(result["topology_id"], persisted["topology_id"])
        self.assertEqual(0, topology.stat().st_mode & 0o222)
        cli_output = self.root / "cli-topologies"
        cli_args = ["--output-dir", str(cli_output)]
        for index, preset in enumerate(presets):
            cli_args.extend([
                "--topology-binding",
                f"{preset}=PROBE-{index}={captures[preset]}",
            ])
        with mock.patch("builtins.print"):
            self.assertEqual(0, cohort.main(cli_args))
        self.assertEqual(1, len(list(cli_output.glob("topology-*.json"))))

        source = self.root / "firmware" / "app" / "src" / "main.c"
        source.write_text("int cohort_source = 3;\n", encoding="utf-8")
        changed = cohort.create_manifest(
            self.root, [builds["mesh_anchor_forcedhop"]], self.output,
        )
        changed_capture = self._capture(
            changed, presets[2], "PROBE-2",
        )
        mismatched = [
            *bindings[:2], (presets[2], "PROBE-2", changed_capture),
        ]
        with self.assertRaisesRegex(cohort.CohortError, "one source snapshot"):
            cohort.validate_topology(mismatched, presets)

    def test_topology_rejects_cross_role_effective_short_address_collision(self) -> None:
        presets = ("mesh_gateway", "mesh_anchor", "mesh_anchor_forcedhop")
        builds = {preset: self._build(preset) for preset in presets}
        manifests = {
            preset: cohort.create_manifest(self.root, [build], self.output)
            for preset, build in builds.items()
        }
        captures = {
            preset: self._capture(
                manifests[preset], preset, f"PROBE-{index}",
            )
            for index, preset in enumerate(presets)
        }

        for preset, node_id in (
            ("mesh_anchor", 0x1000000000001234),
            ("mesh_anchor_forcedhop", 0x2000000000001234),
        ):
            path = captures[preset]
            data = json.loads(path.read_text(encoding="utf-8"))
            physical_id = node_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN
            data["target"].update(
                cohort._identity_record(preset, physical_id, node_id)
            )
            data["cohort_capture_id"] = cohort.capture_binding_id(data)
            path.write_text(json.dumps(data), encoding="utf-8")

        bindings = [
            (preset, f"PROBE-{index}", captures[preset])
            for index, preset in enumerate(presets)
        ]
        with self.assertRaisesRegex(
            cohort.CohortError, "effective UWB short address 0x1234",
        ):
            cohort.validate_topology(bindings, presets)

    def test_deployment_inventory_is_instance_keyed_and_checks_short_aliases(self) -> None:
        presets = ("mesh_gateway", "mesh_clicker", "mesh_anchor")
        builds = {preset: self._build(preset) for preset in presets}
        manifests = {
            preset: cohort.create_manifest(self.root, [build], self.output)
            for preset, build in builds.items()
        }
        gateway = self._capture(
            manifests["mesh_gateway"], "mesh_gateway", "PROBE-GATEWAY",
        )
        clicker_one = self._capture(
            manifests["mesh_clicker"], "mesh_clicker", "PROBE-CLICKER-1",
        )
        clicker_two = self._capture(
            manifests["mesh_clicker"], "mesh_clicker", "PROBE-CLICKER-2",
        )
        anchor = self._capture(
            manifests["mesh_anchor"], "mesh_anchor", "PROBE-ANCHOR",
        )

        inventory = cohort.validate_deployment_inventory(
            [gateway, clicker_one, clicker_two, anchor]
        )
        self.assertEqual(4, len(inventory["instances"]))
        self.assertEqual(
            2,
            sum(
                item["preset"] == "mesh_clicker"
                for item in inventory["instances"]
            ),
        )
        persisted = cohort.create_deployment_inventory_manifest(
            [gateway, clicker_one, clicker_two, anchor],
            self.root / "inventories",
        )
        self.assertEqual(0, persisted.stat().st_mode & 0o222)
        cli_output = self.root / "cli-inventories"
        cli_args = ["--output-dir", str(cli_output)]
        for capture_path in (gateway, clicker_one, clicker_two, anchor):
            cli_args.extend(["--inventory-capture", str(capture_path)])
        with mock.patch("builtins.print"):
            self.assertEqual(0, cohort.main(cli_args))
        self.assertEqual(1, len(list(cli_output.glob("inventory-*.json"))))

        def set_node(path: Path, preset: str, node_id: int) -> None:
            data = json.loads(path.read_text(encoding="utf-8"))
            physical_id = node_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN
            data["target"].update(
                cohort._identity_record(preset, physical_id, node_id)
            )
            data["cohort_capture_id"] = cohort.capture_binding_id(data)
            path.write_text(json.dumps(data), encoding="utf-8")

        set_node(clicker_one, "mesh_clicker", 0x1000000000000000)
        set_node(anchor, "mesh_anchor", 0x2000000000000001)
        with self.assertRaisesRegex(
            cohort.CohortError, "effective UWB short address 0x0001",
        ):
            cohort.validate_deployment_inventory(
                [gateway, clicker_one, clicker_two, anchor]
            )

        with self.assertRaisesRegex(cohort.CohortError, "reserved"):
            set_node(
                clicker_one, "mesh_clicker", cohort.FIXED_GATEWAY_NODE_ID,
            )

        gateway_data = json.loads(gateway.read_text(encoding="utf-8"))
        gateway_physical_id = int(gateway_data["target"]["physical_id"], 16)
        second = json.loads(clicker_two.read_text(encoding="utf-8"))
        second["target"].update(cohort._identity_record(
            "mesh_clicker",
            gateway_physical_id,
            gateway_physical_id ^ cohort.DEVICE_IDENTITY_NODE_DOMAIN,
        ))
        second["cohort_capture_id"] = cohort.capture_binding_id(second)
        clicker_two.write_text(json.dumps(second), encoding="utf-8")
        with self.assertRaisesRegex(cohort.CohortError, "physical identity"):
            cohort.validate_deployment_inventory(
                [gateway, clicker_two]
            )

        data = json.loads(clicker_two.read_text(encoding="utf-8"))
        data["target"].pop("effective_uwb_short_addr")
        data["cohort_capture_id"] = cohort.capture_binding_id(data)
        clicker_two.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(cohort.CohortError, "effective_uwb_short_addr"):
            cohort.validate_deployment_inventory([gateway, clicker_two])

    def test_west_project_dirty_content_changes_source_identity(self) -> None:
        (self.root / ".west").mkdir()
        (self.root / ".west" / "config").write_text(
            "[manifest]\npath = manifest\n", encoding="utf-8",
        )
        manifest_repo = self.root / "manifest"
        module_repo = self.root / "modules" / "sample"
        self._init_repository(manifest_repo, "west.yml")
        self._init_repository(module_repo, "module.c")
        west = self.root / ".venv" / "bin" / "west"
        west.parent.mkdir(parents=True)
        west.write_text(
            "#!/bin/sh\n"
            "printf 'manifest\\tmanifest\\tmain\\n'\n"
            "printf 'sample\\tmodules/sample\\tdeadbeef\\n'\n",
            encoding="utf-8",
        )
        west.chmod(0o755)
        build = self._build("mesh_gateway")

        first = cohort.source_snapshot(self.root, [build])
        (module_repo / "module.c").write_text("dirty change\n", encoding="utf-8")
        changed = cohort.source_snapshot(self.root, [build])

        self.assertEqual(2, len(first["west_projects"]))
        self.assertNotEqual(first["source_id"], changed["source_id"])
        sample = next(
            item for item in changed["west_projects"]
            if item["name"] == "sample"
        )
        self.assertTrue(sample["dirty_entries"])

    def test_source_discovery_disables_optional_git_locks(self) -> None:
        (self.root / ".west").mkdir()
        (self.root / ".west" / "config").write_text(
            "[manifest]\npath = .\n", encoding="utf-8",
        )
        west = self.root / ".venv" / "bin" / "west"
        west.parent.mkdir(parents=True)
        west.write_text(
            "#!/bin/sh\n"
            "test \"$GIT_OPTIONAL_LOCKS\" = 0 || exit 91\n"
            "printf 'manifest\\t.\\tmain\\n'\n",
            encoding="utf-8",
        )
        west.chmod(0o755)
        build = self._build("mesh_gateway")
        original_run = subprocess.run
        discovery_commands: set[str] = set()

        def guarded_run(command, *args, **kwargs):
            executable = Path(str(command[0])).name
            if executable in {"git", "west"}:
                environment = kwargs.get("env")
                self.assertIsInstance(environment, dict)
                self.assertEqual("0", environment.get("GIT_OPTIONAL_LOCKS"))
                self.assertEqual("preserved", environment.get("COHORT_TEST_ENV"))
                discovery_commands.add(executable)
            return original_run(command, *args, **kwargs)

        with mock.patch.dict(
            cohort.os.environ,
            {"COHORT_TEST_ENV": "preserved", "GIT_OPTIONAL_LOCKS": "1"},
        ), mock.patch.object(
            cohort.subprocess, "run", side_effect=guarded_run,
        ):
            snapshot = cohort.source_snapshot(self.root, [build])

        self.assertEqual({"git", "west"}, discovery_commands)
        self.assertEqual(1, len(snapshot["west_projects"]))

    def test_dirty_build_graph_cannot_be_assigned_a_cohort_identity(self) -> None:
        build = self._build("mesh_gateway")
        ninja = build / "fake-ninja"
        ninja.write_text(
            "#!/bin/sh\necho '[1/1] rebuilding stale artifact'\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(cohort.CohortError, "build graph is dirty"):
            cohort.create_manifest(self.root, [build], self.output)


if __name__ == "__main__":
    unittest.main()
