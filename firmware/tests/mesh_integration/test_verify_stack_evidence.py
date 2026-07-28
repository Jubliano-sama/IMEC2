#!/usr/bin/env python3
"""Regression tests for repository-owned typed stack evidence."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from source_text import read_composed_source


REPO_ROOT = Path(__file__).resolve().parents[3]
VERIFIER_PATH = REPO_ROOT / "firmware" / "scripts" / "verify_stack_evidence.py"
POLICY_PATH = REPO_ROOT / "firmware" / "include" / "stack_budget.h"
SPEC = importlib.util.spec_from_file_location("verify_stack_evidence", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
verifier = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verifier
SPEC.loader.exec_module(verifier)


def _line(key: str, value: object) -> str:
    if value is True:
        return f"{key}=y"
    if value is False:
        return f"# {key} is not set"
    return f"{key}={value}"


class StackEvidenceVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policies, cls.frame_limit = verifier.load_policy(POLICY_PATH)

    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _synchronous_evidence(
        self,
        frames: dict[str, int],
        calls: dict[str, set[str]],
        *,
        roots: tuple[str, ...] = ("root",),
    ) -> object:
        """Run ownership and synchronous-depth checks on one synthetic TU."""
        source = "sync.c"
        policy = self.policies["mesh_gateway"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(
                Path(source), index, function, bytes_used, "static"
            )
            for index, (function, bytes_used) in enumerate(
                frames.items(), start=1
            )
        ]
        ownership_graph: dict[tuple[str, str], set[str]] = {}
        for function in frames:
            ownership_graph.setdefault(
                (source, verifier._canonical_function(function)), set()
            )
        for function, targets in calls.items():
            node = (source, verifier._canonical_function(function))
            canonical_targets = set()
            for target in targets:
                if target.startswith(verifier._CGRAPH_REFERENCE_PREFIX):
                    target = (
                        verifier._CGRAPH_REFERENCE_PREFIX +
                        verifier._canonical_function(
                            target[len(verifier._CGRAPH_REFERENCE_PREFIX):]
                        )
                    )
                else:
                    target = verifier._canonical_function(target)
                canonical_targets.add(target)
            ownership_graph.setdefault(node, set()).update(canonical_targets)
        synchronous_graph = {
            (source, function): set(calls.get(function, set()))
            for function in frames
        }
        thread_roots = {
            (source, verifier._canonical_function(function)):
                {"system_workqueue"}
            for function in roots
        }

        verifier._attribute_linked_functions(
            evidence,
            policy,
            linked,
            ownership_graph,
            thread_roots,
            self.frame_limit,
            synchronous_graph,
        )
        return evidence

    def _write_build(self, policy: object, *, include_usage: bool = True,
                     include_cgraph: bool = True, source_name: str = "main.c", function: str = "main",
                     headroom_delta: int = 2048, identity: str | None = None) -> Path:
        build = self.root / policy.preset
        zephyr, usage = build / "zephyr", build / "CMakeFiles" / "app.dir" / "src"
        zephyr.mkdir(parents=True, exist_ok=True)
        usage.mkdir(parents=True, exist_ok=True)
        source = self.root / source_name
        source.write_text(f"void {function}(void) {{}}\n", encoding="utf-8")
        ninja = build / "fake-ninja"
        ninja.write_text("#!/bin/sh\necho 'ninja: no work to do.'\n", encoding="utf-8")
        ninja.chmod(0o755)
        (build / "CMakeCache.txt").write_text(
            f"IMEC_BUILD_PRESET:STRING={policy.preset}\nCMAKE_MAKE_PROGRAM:FILEPATH={ninja}\n", encoding="utf-8"
        )
        config: dict[str, object] = {
            "CONFIG_MAIN_STACK_SIZE": policy.main_bytes,
            "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE": policy.system_workqueue_bytes,
            "CONFIG_ISR_STACK_SIZE": policy.isr_bytes,
            "CONFIG_IDLE_STACK_SIZE": policy.idle_bytes,
            "CONFIG_INIT_STACKS": policy.init_stacks,
            "CONFIG_HW_STACK_PROTECTION": policy.hw_stack_protection,
            "CONFIG_MPU_STACK_GUARD": policy.mpu_stack_guard,
            "CONFIG_ARM_MPU_REGION_MIN_ALIGN_AND_SIZE": 32,
            "CONFIG_STACK_ALIGN_DOUBLE_WORD": True,
            "CONFIG_THREAD_STACK_INFO": policy.thread_stack_info,
            "CONFIG_STACK_SENTINEL": policy.stack_sentinel,
            "CONFIG_LOG_PROCESS_THREAD": bool(policy.log_processor_bytes),
            "CONFIG_BT": bool(policy.bt_rx_bytes or policy.bt_hci_tx_bytes),
            "CONFIG_SRAM_SIZE": 128,
        }
        if policy.log_processor_bytes:
            config["CONFIG_LOG_PROCESS_THREAD_STACK_SIZE"] = policy.log_processor_bytes
        if policy.bt_rx_bytes:
            config.update({
                "CONFIG_BT_HCI_TX_STACK_SIZE": policy.bt_hci_tx_bytes,
                "CONFIG_BT_RX_STACK_SIZE": policy.bt_rx_bytes,
                "CONFIG_BT_RECV_WORKQ_BT": True,
                "CONFIG_BT_LONG_WQ": True,
                "CONFIG_BT_LONG_WQ_STACK_SIZE": 1300,
                "CONFIG_MPSL": True,
                "CONFIG_MPSL_WORK_STACK_SIZE": 1024,
            })
        if policy.deployable:
            config.update({
                "CONFIG_IMEC_STACK_DIAGNOSTICS": True,
                "CONFIG_THREAD_MONITOR": True,
                "CONFIG_THREAD_NAME": True,
                "CONFIG_USE_SEGGER_RTT": True,
            })
        (zephyr / ".config").write_text("\n".join(_line(key, value) for key, value in config.items()) + "\n", encoding="utf-8")
        object_path = Path("CMakeFiles/app.dir/src") / f"{source_name}.obj"
        kernel_source = self.root / "kernel.c"
        kernel_source.write_text("void kernel_frame(void) {}\n", encoding="utf-8")
        (build / "build.ninja").write_text(
            "FLAGS = -fstack-usage\n"
            f"build {object_path}: C_COMPILER__app {source}\n"
            f"build zephyr/kernel/CMakeFiles/kernel.dir/kernel.c.obj: C_COMPILER__kernel {kernel_source}\n",
            encoding="utf-8",
        )
        if include_usage:
            (usage / f"{source_name}.su").write_text(f"{source}:1:1:{function}\t64\tstatic\n", encoding="utf-8")
        if include_cgraph:
            (usage / f"{source_name}.c.000i.cgraph").write_text(
                "Optimized Symbol table:\n"
                f"{function}/1 ({function})\n"
                "  Type: function definition analyzed\n"
                "  Calls: \n"
                "Final Symbol table:\n",
                encoding="utf-8",
            )
        origin, size = 0x20000000, 128 * 1024
        end = origin + size - (policy.minimum_static_ram_headroom_bytes + headroom_delta)
        (zephyr / "zephyr.map").write_text(
            "Memory Configuration\n\nName Origin Length Attributes\n"
            f"RAM 0x{origin:016x} 0x{size:016x} xw\n\nLinker script and memory map\n"
            f" .text.{function}\n                0x0000000000010000 {function}\n"
            f"                0x{end:016x} _image_ram_end = .\n", encoding="utf-8"
        )
        build_identity = identity or f"imec-stack-v1:{policy.preset}:{'a' * 64}"
        (zephyr / "zephyr.elf").write_bytes(f"ELF {build_identity}".encode("ascii"))
        (zephyr / "zephyr.hex").write_bytes(f"HEX {policy.preset}".encode("ascii"))
        return build

    def _sample(self, policy: object, build: object, run: int, sample: int, kind: str, owner: str, identity: tuple[int, int, int, int, int]) -> str:
        required = verifier._required_threads(build, policy)
        src, dst, session, seq, msg_type = identity
        lines = [f"DBG_STACK_SAMPLE_BEGIN epoch=1 run={run} sample={sample} kind={kind} owner={owner} queue=2 custody=1 credit=1 retry=0 drain=2 src={src} dst={dst} session={session} seq={seq} type={msg_type} uptime={sample * 10}", f"DBG_STACK_ISR_CONFIG size={policy.isr_bytes} run={run} sample={sample}"]
        for name, size in required.items():
            service = name in {"logging", "BT RX", "BT RX WQ", "BT LW WQ", "MPSL Work", "idle"}
            free = verifier._required_free(size, service) + 8
            lines.append(f"DBG_STACK name={name} tid=0x1 used={size - free} free={free} size={size} ret=0 run={run} sample={sample}")
        lines.append(f"DBG_STACK_SAMPLE_END run={run} sample={sample}")
        return "\n".join(lines)

    def _typed_log(self, policy: object, build: object, workloads: list[str] | None = None) -> Path:
        workload_policy = verifier.load_workload_policy(POLICY_PATH)
        if workloads is None:
            required = [
                requirement.kind
                for requirement in workload_policy[policy.preset]
                for _ in range(requirement.minimum_successes)
            ]
        else:
            required = workloads
        owners = {
            "click_spam": "clicker_action",
            "cir_handling": "anchor_uwb_scan",
            "relay_retry": "mesh_route",
            "ble_backpressure": "system_workqueue",
            "click_activity": "clicker_action",
            "anchor_survey_report": "anchor_uwb_scan",
            "gateway_report_ingress": "mesh_route",
            "gateway_priority_control": "mesh_route",
        }
        lines = [f"DBG_STACK_BOOT preset={policy.preset} build={build.build_identity} epoch=1 uptime=1"]
        entries = []
        previous_click = 0
        sequence = 0
        for run, kind in enumerate(required, start=1):
            if kind == "click_spam":
                sequence += 1
                previous = previous_click
                previous_click = run
            else:
                previous = 0
            owner = owners[kind]
            identity = (100 + run, 200, 300 + run, 400 + run, 0x20 + run)
            entries.append((run, kind, owner, sequence if kind == "click_spam" else 0, previous, identity))
        for run, kind, owner, click_sequence, previous, identity in entries:
            src, dst, session, seq, msg_type = identity
            lines.append(f"DBG_STACK_RUN_BEGIN epoch=1 run={run} kind={kind} owner={owner} queue=2 custody=1 credit=1 retry=0 drain=2 src={src} dst={dst} session={session} seq={seq} type={msg_type} sequence={click_sequence} previous={previous} uptime={run * 10}")
        for run, kind, owner, click_sequence, previous, identity in entries:
            lines.append(self._sample(policy, build, run, run, kind, owner, identity))
            src, dst, session, seq, msg_type = identity
            lines.append(f"DBG_STACK_RUN_END epoch=1 run={run} kind={kind} owner={owner} outcome=ack queue=1 custody=1 credit=1 retry=0 drain=1 src={src} dst={dst} session={session} seq={seq} type={msg_type} samples=1 sequence={click_sequence} previous={previous} uptime={run * 10 + 1}")
        log = self.root / "capture.typescript"
        log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return log

    def _manifest(self, policy: object, build: object, log: Path | None = None) -> Path:
        log = log or self._typed_log(policy, build)
        ended = datetime.now(timezone.utc)
        started = ended - timedelta(seconds=10)
        data = {
            "schema": verifier.CAPTURE_SCHEMA,
            "preset": policy.preset,
            "probe_id": "TEST-PROBE",
            "artifact": {"elf_sha256": build.elf_sha256, "hex_sha256": build.hex_sha256},
            "target": {"preset": policy.preset, "build_identity": build.build_identity},
            "transcript": {"path": log.name, "sha256": hashlib.sha256(log.read_bytes()).hexdigest()},
            "provenance": {
                "tool": verifier.CAPTURE_TOOL_RELATIVE,
                "tool_sha256": hashlib.sha256(verifier.CAPTURE_TOOL.read_bytes()).hexdigest(),
                "workflow": verifier.CAPTURE_WORKFLOW,
                "rtt_command": [
                    "pyocd", "rtt", "-t", "nrf52833", "-M", "pre-reset",
                    "-u", "TEST-PROBE", "--up-channel-id", "0",
                ],
                "tty_wrapper": "script",
                "started_at_utc": started.isoformat().replace("+00:00", "Z"),
                "ended_at_utc": ended.isoformat().replace("+00:00", "Z"),
            },
        }
        data["capture_id"] = verifier._capture_id(data)
        manifest = self.root / "capture.json"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        return manifest

    @staticmethod
    def _replace_manifest(manifest: Path, mutate: object, *, bind: bool = False) -> None:
        data = json.loads(manifest.read_text(encoding="utf-8"))
        mutate(data)
        if bind:
            data["capture_id"] = verifier._capture_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")

    def test_policy_covers_exact_deployable_presets_and_static_builds(self) -> None:
        self.assertEqual(verifier.DEPLOYABLE_PRESETS, {key for key, value in self.policies.items() if value.deployable})
        workload_policy = verifier.load_workload_policy(POLICY_PATH)
        self.assertEqual(verifier.DEPLOYABLE_PRESETS, set(workload_policy))
        self.assertEqual(
            ["click_activity"],
            [item.kind for item in workload_policy["mesh_clicker"]],
        )
        self.assertEqual(
            ["anchor_survey_report"],
            [item.kind for item in workload_policy["mesh_anchor"]],
        )
        self.assertEqual(
            ["gateway_report_ingress", "gateway_priority_control", "ble_backpressure"],
            [item.kind for item in workload_policy["mesh_gateway"]],
        )
        self.assertEqual(
            ["mesh_route", "mesh_route", "system_workqueue"],
            [item.owner for item in workload_policy["mesh_gateway"]],
        )
        builds = [verifier.verify_build(self._write_build(policy), self.policies, self.frame_limit) for policy in self.policies.values()]
        self.assertEqual([], [(build.preset, build.issues) for build in builds if build.issues])

    def test_workload_policy_parser_fails_closed(self) -> None:
        text = POLICY_PATH.read_text(encoding="utf-8")
        missing_gateway = self.root / "missing-gateway.h"
        missing_gateway.write_text(
            "\n".join(
                line for line in text.splitlines()
                if 'X("mesh_gateway"' not in line
            ) + "\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(verifier.EvidenceError,
                                    "workload policy misses mesh_gateway"):
            verifier.load_workload_policy(missing_gateway)

        unknown_owner = self.root / "unknown-owner.h"
        unknown_owner.write_text(
            text.replace('"clicker_action", 1u, false',
                         '"invented_thread", 1u, false', 1),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(verifier.EvidenceError,
                                    "unknown owner invented_thread"):
            verifier.load_workload_policy(unknown_owner)

    def test_missing_usage_and_inadequate_ram_fail(self) -> None:
        policy = self.policies["mesh_anchor"]
        missing = verifier.verify_build(self._write_build(policy, include_usage=False), self.policies, self.frame_limit)
        self.assertTrue(any("missing compiler stack evidence" in issue for issue in missing.issues))
        low = verifier.verify_build(self._write_build(policy, headroom_delta=-1), self.policies, self.frame_limit)
        self.assertTrue(any("static RAM headroom" in issue for issue in low.issues))

    def test_disabled_boolean_may_be_absent_but_required_values_may_not(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = self._write_build(policy)
        config = build / "zephyr" / ".config"
        config.write_text(
            "\n".join(line for line in config.read_text(encoding="utf-8").splitlines()
                      if not line.startswith("# CONFIG_STACK_SENTINEL is not set")) + "\n",
            encoding="utf-8",
        )
        accepted = verifier.verify_build(build, self.policies, self.frame_limit)
        self.assertEqual([], accepted.issues)
        config.write_text(config.read_text(encoding="utf-8") + "CONFIG_STACK_SENTINEL=y\n", encoding="utf-8")
        enabled = verifier.verify_build(build, self.policies, self.frame_limit)
        self.assertTrue(any("CONFIG_STACK_SENTINEL" in issue for issue in enabled.issues), enabled.issues)
        config.write_text(
            "\n".join(line for line in config.read_text(encoding="utf-8").splitlines()
                      if not line.startswith("CONFIG_MAIN_STACK_SIZE=")) + "\n",
            encoding="utf-8",
        )
        missing = verifier.verify_build(build, self.policies, self.frame_limit)
        self.assertTrue(any("CONFIG_MAIN_STACK_SIZE" in issue for issue in missing.issues), missing.issues)

    def test_rooted_call_graph_requires_exact_reachability(self) -> None:
        policy = self.policies["mesh_anchor"]
        accepted = verifier.verify_build(self._write_build(policy, source_name="app_anchor.c", function="anchor_uwb_scan_work_handler"), self.policies, self.frame_limit)
        self.assertEqual([], accepted.issues)
        rejected = verifier.verify_build(self._write_build(policy, source_name="app_anchor.c", function="unannotated_anchor_worker"), self.policies, self.frame_limit)
        self.assertTrue(any("unattributed linked application function" in issue for issue in rejected.issues), rejected.issues)
        missing_graph = verifier.verify_build(self._write_build(policy, include_cgraph=False), self.policies, self.frame_limit)
        self.assertTrue(any("missing compiler call graph" in issue for issue in missing_graph.issues), missing_graph.issues)

    def test_gcc_clone_suffixes_are_canonicalized_without_overmatching(self) -> None:
        cases = {
            "worker.constprop": "worker",
            "worker.constprop.7": "worker",
            "worker.isra": "worker",
            "worker.isra.2": "worker",
            "worker.part": "worker",
            "worker.part.0": "worker",
            "worker.part.0.constprop.isra": "worker",
            "worker.constpropagation": "worker.constpropagation",
            "worker.israel": "worker.israel",
            "worker.partition": "worker.partition",
            "worker.constprop.extra": "worker.constprop.extra",
            "worker.part.0.extra": "worker.part.0.extra",
        }

        for function, expected in cases.items():
            with self.subTest(function=function):
                self.assertEqual(expected, verifier._canonical_function(function))

    def test_compiler_west_topdir_source_path_resolves_to_checkout(self) -> None:
        expected = REPO_ROOT / "firmware" / "src" / "mesh_relay.c"

        self.assertEqual(
            expected.resolve(),
            verifier._resolve_compiler_source(
                Path("WEST_TOPDIR/firmware/src/mesh_relay.c")
            ),
        )

    def test_compiler_cmake_source_path_resolves_to_application(self) -> None:
        expected = REPO_ROOT / "firmware" / "app" / "src" / "app_anchor.c"

        self.assertEqual(
            expected.resolve(),
            verifier._resolve_compiler_source(
                Path("CMAKE_SOURCE_DIR/src/app_anchor.c")
            ),
        )

    def test_bare_clone_suffix_stack_usage_is_linked_and_attributed(self) -> None:
        policy = self.policies["mesh_anchor"]
        function = "anchor_uwb_scan_work_handler"
        build = self._write_build(
            policy, source_name="app_anchor.c", function=function
        )
        usage = build / "CMakeFiles" / "app.dir" / "src" / "app_anchor.c.su"
        source = self.root / "app_anchor.c"
        usage.write_text(
            f"{source}:1:1:{function}.constprop\t64\tstatic\n",
            encoding="utf-8",
        )

        evidence = verifier.verify_build(
            build, self.policies, self.frame_limit
        )

        self.assertEqual([], evidence.issues)
        self.assertEqual(1, evidence.linked_usage_count)
        self.assertEqual(1, evidence.attributed_usage_count)

    def test_synchronous_linear_chain_overflow_fails_owner_capacity(self) -> None:
        configured = self.policies["mesh_gateway"].system_workqueue_bytes
        evidence = self._synchronous_evidence(
            {"root": 3200, "leaf": 3200},
            {"root": {"leaf"}, "leaf": set()},
        )

        self.assertEqual(6400, evidence.synchronous_usage_bytes[
            "system_workqueue"
        ])
        self.assertTrue(any(
            "compiler synchronous stack chain 6400 plus required free 1024 "
            f"exceeds configured {configured}"
            in issue
            for issue in evidence.issues
        ), evidence.issues)

    def test_synchronous_siblings_use_maximum_branch_not_sum(self) -> None:
        evidence = self._synchronous_evidence(
            {"root": 1000, "left": 2000, "right": 2000},
            {"root": {"left", "right"}, "left": set(), "right": set()},
        )

        self.assertEqual(3000, evidence.synchronous_usage_bytes[
            "system_workqueue"
        ])
        self.assertFalse(any(
            "compiler synchronous stack chain" in issue
            for issue in evidence.issues
        ), evidence.issues)

    def test_synchronous_owner_margin_boundary_is_inclusive(self) -> None:
        configured = self.policies["mesh_gateway"].system_workqueue_bytes
        required_free = verifier._required_free(configured)
        accepted_usage = configured - required_free
        accepted = self._synchronous_evidence(
            {"root": accepted_usage}, {"root": set()}
        )
        rejected = self._synchronous_evidence(
            {"root": accepted_usage + 1}, {"root": set()}
        )

        self.assertEqual([], accepted.issues)
        self.assertTrue(any(
            f"{accepted_usage + 1} plus required free "
            f"{required_free} exceeds configured "
            f"{configured}" in issue
            for issue in rejected.issues
        ), rejected.issues)

    def test_callback_reference_owns_frame_but_is_not_synchronous(self) -> None:
        evidence = self._synchronous_evidence(
            {"root": 3000, "callback": 2000},
            {
                "root": {
                    verifier._CGRAPH_REFERENCE_PREFIX + "callback"
                },
                "callback": set(),
            },
        )

        self.assertEqual(3000, evidence.synchronous_usage_bytes[
            "system_workqueue"
        ])
        self.assertEqual([], evidence.issues)
        self.assertEqual(2, evidence.attributed_usage_count)

    def test_empty_cgraph_calls_do_not_consume_gcc_update_diagnostic(self) -> None:
        graph = self.root / "mesh_event_owner.c.000i.cgraph"
        graph.write_text(
            "Optimized Symbol table:\n\n"
            "mesh_event_owner_commit/7 (mesh_event_owner_commit)\n"
            "  Type: function definition analyzed\n"
            "  Calls: payload_fingerprint/0\n"
            "payload_fingerprint/0 (payload_fingerprint)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n"
            "updating call of mesh_event_owner_commit/7 -> "
            "__builtin_unreachable/13: value = payload_fingerprint ();\n"
            "Removing variables:\n",
            encoding="utf-8",
        )

        ownership = verifier._parse_cgraph(graph, "mesh_event_owner.c")
        synchronous = verifier._parse_synchronous_cgraph(
            graph, "mesh_event_owner.c"
        )

        leaf = ("mesh_event_owner.c", "payload_fingerprint")
        caller = ("mesh_event_owner.c", "mesh_event_owner_commit")
        self.assertEqual(set(), ownership[leaf])
        self.assertEqual(set(), synchronous[leaf])
        self.assertEqual({"payload_fingerprint"}, synchronous[caller])

    def test_recursive_synchronous_graph_fails_closed(self) -> None:
        evidence = self._synchronous_evidence(
            {"root": 64, "helper": 64},
            {"root": {"helper"}, "helper": {"root"}},
        )

        self.assertTrue(any(
            "recursive synchronous compiler call graph" in issue
            for issue in evidence.issues
        ), evidence.issues)
        self.assertNotIn("system_workqueue", evidence.synchronous_usage_bytes)

    def test_synchronous_clone_names_keep_distinct_frames_and_owners(self) -> None:
        evidence = self._synchronous_evidence(
            {"root.constprop.7": 3200, "leaf.isra.2": 3200},
            {
                "root.constprop.7": {"leaf.isra.2"},
                "leaf.isra.2": set(),
            },
            roots=("root.constprop.7",),
        )

        self.assertEqual(6400, evidence.synchronous_usage_bytes[
            "system_workqueue"
        ])
        issue = next(
            issue for issue in evidence.issues
            if "compiler synchronous stack chain" in issue
        )
        self.assertIn("sync.c:root.constprop.7", issue)
        self.assertIn("sync.c:leaf.isra.2", issue)

    def test_large_synchronous_dag_is_bounded_without_recursion(self) -> None:
        node_count = 3000
        frames = {f"node_{index}": 1 for index in range(node_count)}
        calls = {
            f"node_{index}": (
                {f"node_{index + 1}", f"node_{index + 2}"} & frames.keys()
            )
            for index in range(node_count)
        }
        evidence = self._synchronous_evidence(
            frames, calls, roots=("node_0",)
        )

        self.assertEqual(node_count, evidence.synchronous_usage_bytes[
            "system_workqueue"
        ])
        self.assertEqual([], evidence.issues)

    def test_synchronous_depth_never_crosses_owner_boundaries(self) -> None:
        policy = self.policies["mesh_gateway"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("split.c"), 1, "main_side", 3000,
                                "static"),
            verifier.StackUsage(Path("split.c"), 2, "work_side", 3000,
                                "static"),
        ]
        synchronous_graph = {
            ("split.c", "main_side"): {"work_side"},
            ("split.c", "work_side"): set(),
        }
        owners = {
            ("split.c", "main_side"): {"main"},
            ("split.c", "work_side"): {"system_workqueue"},
        }

        verifier._validate_synchronous_stack_chains(
            evidence, policy, linked, synchronous_graph, owners
        )

        self.assertEqual(
            {"main": 3000, "system_workqueue": 3000},
            evidence.synchronous_usage_bytes,
        )
        self.assertEqual([], evidence.issues)

    def test_anchor_capture_requires_survey_owner_queue_with_exact_stack(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = verifier.BuildEvidence(self.root, preset="mesh_anchor")
        required = verifier._required_threads(build, policy)

        self.assertEqual(required["anchor_uwb_scan"], 12288)
        rows = {
            name: (size - verifier._required_free(size),
                   verifier._required_free(size), size)
            for name, size in required.items()
            if name != "anchor_uwb_scan"
        }
        issues = verifier._check_sample_rows(rows, policy, build)
        self.assertTrue(any("misses configured thread anchor_uwb_scan" in issue
                            for issue in issues), issues)

        rows["anchor_uwb_scan"] = (4080, 16, 4096)
        issues = verifier._check_sample_rows(rows, policy, build)
        self.assertTrue(any("anchor_uwb_scan differs" in issue for issue in issues),
                        issues)

    def test_gateway_runtime_model_uses_only_live_exact_workqueues(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        self.assertEqual([], build.issues)
        required = verifier._required_threads(build, policy)

        self.assertNotIn("main", required)
        self.assertNotIn("BT HCI TX", required)
        self.assertNotIn("BT RX", required)
        self.assertEqual(4288, required["sysworkq"])
        self.assertEqual(8192, required["mesh_route"])
        self.assertEqual(1088, required["BT RX WQ"])
        self.assertEqual(1344, required["BT LW WQ"])

        rows = {
            name: (size - verifier._required_free(size, name in {
                "logging", "BT RX WQ", "BT LW WQ", "MPSL Work", "idle",
            }), verifier._required_free(size, name in {
                "logging", "BT RX WQ", "BT LW WQ", "MPSL Work", "idle",
            }), size)
            for name, size in required.items()
        }
        rows["BT RX WQ"] = (828, 260, 1088)
        self.assertEqual([], verifier._check_sample_rows(rows, policy, build))
        rows["BT RX WQ"] = (828, 196, 1024)
        issues = verifier._check_sample_rows(rows, policy, build)
        self.assertTrue(any("BT RX WQ differs" in issue for issue in issues),
                        issues)
        self.assertTrue(any("free space below policy for BT RX WQ" in issue
                            for issue in issues), issues)

    def test_ambiguous_root_reachability_is_charged_to_every_root(self) -> None:
        policy = self.policies["mesh_clicker"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("main.c"), 1, "main", 64, "static"),
            verifier.StackUsage(Path("app_anchor.c"), 1, "shared_callback", policy.main_bytes + 1, "static"),
        ]
        graph = {
            ("main.c", "main"): {"shared_callback"},
            ("app_anchor.c", "shared_callback"): set(),
        }
        roots = {
            ("main.c", "main"): {"main"},
            ("app_anchor.c", "shared_callback"): {"system_workqueue"},
        }
        verifier._attribute_linked_functions(evidence, policy, linked, graph, roots, self.frame_limit)
        self.assertTrue(any("owner=main" in issue for issue in evidence.issues), evidence.issues)

    def test_compiler_address_taken_callback_is_a_conservative_edge(self) -> None:
        graph = self.root / "callback.c.c.000i.cgraph"
        graph.write_text(
            "root/1 (root)\n"
            "  Type: function definition analyzed\n"
            "  References: callback/2 (addr)\n"
            "  Calls: \n"
            "callback/2 (callback)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n",
            encoding="utf-8",
        )
        parsed = verifier._parse_cgraph(graph, "callback.c")
        self.assertEqual(
            {verifier._CGRAPH_REFERENCE_PREFIX + "callback"},
            parsed[("callback.c", "root")],
        )

    def test_compiler_func_strings_do_not_join_unrelated_functions(self) -> None:
        graph = self.root / "func-string.c.c.000i.cgraph"
        graph.write_text(
            "first/1 (first)\n"
            "  Type: function definition analyzed\n"
            "  References: __func__/2 (read)\n"
            "  Calls: \n"
            "__func__/2 (__func__)\n"
            "  Type: variable definition analyzed\n"
            "  References: \n"
            "  Varpool flags: initialized read-only const-value-known\n"
            "second/3 (second)\n"
            "  Type: function definition analyzed\n"
            "  References: __func__/4 (read)\n"
            "  Calls: \n"
            "__func__/4 (__func__)\n"
            "  Type: variable definition analyzed\n"
            "  References: \n"
            "  Varpool flags: initialized read-only const-value-known\n",
            encoding="utf-8",
        )

        parsed = verifier._parse_cgraph(graph, "func-string.c")

        self.assertEqual(set(), parsed[("func-string.c", "first")])
        self.assertEqual(set(), parsed[("func-string.c", "second")])
        self.assertFalse(any(node[1].startswith("<variable>:__func__")
                             for node in parsed))

    def test_callback_and_ops_variables_preserve_exact_stack_ownership(self) -> None:
        graph_file = self.root / "callback.c.c.000i.cgraph"
        graph_file.write_text(
            "root/1 (root)\n"
            "  Type: function definition analyzed\n"
            "  References: callback_ops/2 (read)\n"
            "  Calls: \n"
            "callback_ops/2 (callback_ops)\n"
            "  Type: variable definition analyzed\n"
            "  References: callback/3 (addr)\n"
            "  Varpool flags: initialized read-only const-value-known\n"
            "callback/3 (callback)\n"
            "  Type: function definition analyzed\n"
            "  Calls: callback_helper/4\n"
            "callback_helper/4 (callback_helper)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n"
            "unrooted/5 (unrooted)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n",
            encoding="utf-8",
        )
        graph = verifier._parse_cgraph(graph_file, "callback.c")
        policy = self.policies["mesh_clicker"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("callback.c"), 1, "root", 64, "static"),
            verifier.StackUsage(Path("callback.c"), 2, "callback", 64, "static"),
            verifier.StackUsage(Path("callback.c"), 3, "callback_helper", 64, "static"),
            verifier.StackUsage(Path("callback.c"), 4, "unrooted", 64, "static"),
        ]

        verifier._attribute_linked_functions(
            evidence,
            policy,
            linked,
            graph,
            {("callback.c", "root"): {"main"}},
            self.frame_limit,
        )

        unattributed = [
            issue for issue in evidence.issues
            if "unattributed linked application function" in issue
        ]
        self.assertEqual(
            ["unattributed linked application function callback.c:unrooted"],
            unattributed,
        )
        self.assertEqual(3, evidence.attributed_usage_count)

    def test_reviewed_opaque_abi_boundaries_have_only_exact_owners(self) -> None:
        roots = verifier.load_thread_roots(POLICY_PATH)
        self.assertEqual(
            {"fatal_context"},
            roots[("main.c", "k_sys_fatal_error_handler")],
        )
        self.assertEqual(
            {"bt_rx"},
            roots[("app_gateway_ble.c", "gateway_ble_packet_ccc_changed")],
        )
        for function in ("writetospiwithcrc", "writetospi", "readfromspi", "deca_usleep"):
            self.assertEqual(
                {"main", "system_workqueue"},
                roots[("dwm3000_sdk_port.c", function)],
            )
        for source, function in (
            ("app_anchor.c", "gateway_handle_survey_discovery_report"),
            ("app_anchor.c", "gateway_survey_work_handler"),
            ("app_anchor.c", "gateway_host_command_retry_work_handler"),
            ("app_anchor.c", "gateway_host_command_work_handler"),
            ("app_anchor.c", "gateway_host_abort_work_handler"),
            ("app_gateway_ble.c", "gateway_command_result_timeout_handler"),
            ("app_stack_workload_diag.c",
             "app_stack_workload_diag_gateway_report_cycle"),
            ("app_stack_workload_diag.c",
             "app_stack_workload_diag_gateway_control_admit"),
            ("app_stack_workload_diag.c",
             "app_stack_workload_diag_gateway_control_sample"),
            ("app_stack_workload_diag.c",
             "app_stack_workload_diag_gateway_control_release"),
        ):
            with self.subTest(source=source, function=function):
                self.assertEqual({"mesh_route"}, roots[(source, function)])

        policy = self.policies["mesh_gateway"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("main.c"), 1, "k_sys_fatal_error_handler", 16, "static"),
            verifier.StackUsage(Path("app_watchdog.c"), 1, "app_watchdog_stop_feeding", 0, "static"),
            verifier.StackUsage(Path("app_gateway_ble.c"), 1, "gateway_ble_packet_ccc_changed", 8, "static"),
            verifier.StackUsage(Path("dwm3000_sdk_port.c"), 1, "writetospi", 8, "static"),
            verifier.StackUsage(Path("dwm3000_port.c"), 1, "dwm3000_port_write", 32, "static"),
            verifier.StackUsage(Path("unrelated.c"), 1, "unrelated_callback", 8, "static"),
        ]
        graph = {
            ("main.c", "k_sys_fatal_error_handler"): {"app_watchdog_stop_feeding"},
            ("app_watchdog.c", "app_watchdog_stop_feeding"): set(),
            ("app_gateway_ble.c", "gateway_ble_packet_ccc_changed"): set(),
            ("dwm3000_sdk_port.c", "writetospi"): {"dwm3000_port_write"},
            ("dwm3000_port.c", "dwm3000_port_write"): set(),
            ("unrelated.c", "unrelated_callback"): set(),
        }

        verifier._attribute_linked_functions(
            evidence, policy, linked, graph, roots, self.frame_limit
        )

        unattributed = [
            issue for issue in evidence.issues
            if "unattributed linked application function" in issue
        ]
        self.assertEqual(
            ["unattributed linked application function unrelated.c:unrelated_callback"],
            unattributed,
        )
        self.assertEqual(5, evidence.attributed_usage_count)

    def test_address_taken_thread_root_ends_inherited_stack_ownership(self) -> None:
        policy = self.policies["mesh_anchor"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("init.c"), 1, "init", 64, "static"),
            verifier.StackUsage(Path("worker.c"), 1, "work_handler", 64, "static"),
            verifier.StackUsage(
                Path("worker.c"), 2, "large_worker_helper",
                policy.main_bytes + 1, "static"
            ),
        ]
        graph = {
            ("init.c", "init"): {
                verifier._CGRAPH_REFERENCE_PREFIX + "work_handler"
            },
            ("worker.c", "work_handler"): {"large_worker_helper"},
            ("worker.c", "large_worker_helper"): set(),
        }
        roots = {
            ("init.c", "init"): {"main"},
            ("worker.c", "work_handler"): {"system_workqueue"},
        }

        verifier._attribute_linked_functions(
            evidence, policy, linked, graph, roots, self.frame_limit
        )

        self.assertFalse(
            any("owner=main" in issue for issue in evidence.issues),
            evidence.issues,
        )
        self.assertEqual(3, evidence.attributed_usage_count)

    def test_absent_service_root_does_not_authorize_linked_code(self) -> None:
        policy = self.policies["mesh_anchor"]
        self.assertEqual(0, policy.bt_rx_bytes)
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("ble.c"), 1, "bt_callback", 8, "static"),
        ]
        graph = {("ble.c", "bt_callback"): set()}

        verifier._attribute_linked_functions(
            evidence,
            policy,
            linked,
            graph,
            {("ble.c", "bt_callback"): {"bt_rx"}},
            self.frame_limit,
        )

        self.assertEqual(
            ["unattributed linked application function ble.c:bt_callback"],
            evidence.issues,
        )

    def test_fatal_handler_keeps_only_retained_breadcrumb_and_reboot_path(self) -> None:
        source = (REPO_ROOT / "firmware" / "app" / "src" / "main.c").read_text(
            encoding="utf-8"
        )
        body = source.split("void k_sys_fatal_error_handler", 1)[1].split("#endif", 1)[0]

        self.assertIn("defined(CONFIG_IMEC_MESH_ROUTE_TEST)", source)
        self.assertIn("defined(CONFIG_IMEC_STACK_STRESS_DIAGNOSTICS)", source)
        self.assertIn("#if IMEC_RETAIN_FATAL_BREADCRUMB", source)
        self.assertIn("mesh_route_test_fatal_magic", body)
        self.assertIn("mesh_route_test_fatal_thread", body)
        self.assertIn("mesh_route_test_fatal_stack_start", body)
        self.assertIn("mesh_route_test_fatal_stack_size", body)
        self.assertIn("sys_reboot(SYS_REBOOT_COLD)", body)
        self.assertNotIn("app_watchdog_", body)
        self.assertNotIn("status_debug_", body)
        self.assertNotIn("stack_diag", body)

        retained_print = source.split('printk("retained fatal:', 1)[1].split(");", 1)[0]
        self.assertIn("mesh_route_test_fatal_thread", retained_print)
        self.assertIn("mesh_route_test_fatal_stack_start", retained_print)
        self.assertIn("mesh_route_test_fatal_stack_size", retained_print)

    def test_linker_local_text_section_counts_as_linked_application_function(self) -> None:
        map_file = self.root / "zephyr.map"
        map_file.write_text(
            "RAM 0x0000000020000000 0x0000000000020000 xw\n"
            " .text.static_callback\n"
            "                0x0000000000001234 0x10 app/libapp.a(app_anchor.c.obj)\n"
            "                0x0000000020010000 _image_ram_end = .\n",
            encoding="utf-8",
        )
        _, _, _, symbols = verifier._ram_map(map_file)
        self.assertIn("static_callback", symbols)

    def test_valid_typed_capture_binds_provenance_and_artifacts(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(self._write_build(policy), self.policies, self.frame_limit)
        manifest = self._manifest(policy, build)
        captures, issues = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertEqual([], issues)
        self.assertEqual([], captures[0].issues)

        log = self.root / json.loads(manifest.read_text(encoding="utf-8"))["transcript"]["path"]
        self.assertGreater(max(map(len, log.read_text(encoding="utf-8").splitlines())), 128)

    def test_capture_provenance_requires_explicit_typed_rtt_channel(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        manifest = self._manifest(policy, build)
        self._replace_manifest(
            manifest,
            lambda data: data["provenance"]["rtt_command"].__delitem__(
                slice(-2, None)
            ),
            bind=True,
        )
        captures, _ = verifier.verify_hardware(
            [manifest], [build], self.policies, True, {policy.preset}
        )
        self.assertTrue(
            any("required pyOCD RTT" in issue for issue in captures[0].issues),
            captures[0].issues,
        )

    def test_rejects_concatenated_and_log_interleaved_typed_records(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        valid = self._typed_log(policy, build).read_text(encoding="utf-8")
        concatenated = valid.replace(
            " uptime=1\n", " uptime=1DBG_LED_SELFTEST\n", 1
        )
        _, issues = verifier.parse_typed_transcript(concatenated, policy, build)
        self.assertTrue(any("invalid uptime" in issue for issue in issues), issues)

        interleaved = valid.replace(
            "DBG_STACK_SAMPLE_BEGIN ",
            "<dbg> unrelated logger prefix DBG_STACK_SAMPLE_BEGIN ",
            1,
        )
        _, issues = verifier.parse_typed_transcript(interleaved, policy, build)
        self.assertTrue(
            any("outside its typed sample" in issue or "missing completed" in issue
                for issue in issues),
            issues,
        )

    def test_rejects_every_missing_sample_commit_record(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        valid_lines = self._typed_log(policy, build).read_text(
            encoding="utf-8"
        ).splitlines()
        sample_begin = next(
            index for index, line in enumerate(valid_lines)
            if line.startswith("DBG_STACK_SAMPLE_BEGIN ")
        )
        sample_end = next(
            index for index in range(sample_begin, len(valid_lines))
            if valid_lines[index].startswith("DBG_STACK_SAMPLE_END ")
        )

        for missing in range(sample_begin, sample_end + 1):
            with self.subTest(missing_record=valid_lines[missing].split(" ", 1)[0]):
                damaged = "\n".join(
                    valid_lines[:missing] + valid_lines[missing + 1:]
                ) + "\n"
                _, issues = verifier.parse_typed_transcript(
                    damaged, policy, build
                )
                self.assertTrue(issues)

    def test_every_deployable_preset_accepts_its_real_role_workloads(self) -> None:
        for preset in sorted(verifier.DEPLOYABLE_PRESETS):
            with self.subTest(preset=preset):
                policy = self.policies[preset]
                build = verifier.verify_build(
                    self._write_build(policy), self.policies, self.frame_limit
                )
                log = self._typed_log(policy, build)
                sample_count, issues = verifier.parse_typed_transcript(
                    log.read_text(encoding="utf-8"), policy, build
                )
                expected = sum(
                    item.minimum_successes
                    for item in verifier.load_workload_policy(POLICY_PATH)[preset]
                )
                self.assertEqual(expected, sample_count)
                self.assertEqual([], issues)

    def test_preset_workload_cannot_be_satisfied_by_wrong_owner_or_role(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        log = self._typed_log(policy, build)
        forged = log.read_text(encoding="utf-8").replace(
            "kind=gateway_report_ingress owner=mesh_route",
            "kind=gateway_report_ingress owner=bt_rx",
        )
        _, issues = verifier.parse_typed_transcript(forged, policy, build)
        self.assertTrue(any("owner differs" in issue for issue in issues), issues)

        other_role = self._typed_log(policy, build, ["anchor_survey_report"])
        _, issues = verifier.parse_typed_transcript(
            other_role.read_text(encoding="utf-8"), policy, build
        )
        self.assertTrue(any("gateway_report_ingress" in issue for issue in issues),
                        issues)

    def test_required_workloads_have_genuine_role_call_sites(self) -> None:
        clicker = (REPO_ROOT / "firmware" / "app" / "src" /
                   "app_clicker.c").read_text(encoding="utf-8")
        anchor = (REPO_ROOT / "firmware" / "app" / "src" /
                  "app_anchor_survey_discovery.c").read_text(encoding="utf-8")
        gateway = read_composed_source(
            REPO_ROOT / "firmware" / "app" / "src" / "app_anchor.c"
        )
        ble = (REPO_ROOT / "firmware" / "app" / "src" /
               "app_gateway_ble.c").read_text(encoding="utf-8")

        self.assertIn("dwm3000_driver_range_initiator", clicker)
        self.assertIn("app_stack_workload_diag_click_activity_sample", clicker)
        self.assertIn("app_mesh_local_delivery_stage", anchor)
        self.assertIn("app_stack_workload_diag_anchor_survey_admit", anchor)
        self.assertIn("app_stack_workload_diag_anchor_survey_sample", anchor)
        self.assertIn("app_stack_workload_diag_anchor_survey_release", anchor)
        self.assertIn("survey_gateway_note_reach_report_with_reverse_hint", gateway)
        self.assertIn("app_stack_workload_diag_gateway_report_cycle", gateway)
        self.assertIn("app_node_comm_submit_delivery", gateway)
        self.assertIn("gateway_survey_wait_for_discovery_collection", gateway)
        self.assertIn(
            "gateway_discovery_assignment_service_delivery_locked", gateway
        )
        self.assertIn("app_stack_workload_diag_gateway_control_sample", gateway)
        self.assertIn("app_stack_workload_diag_ble_admit_with_pressure", ble)
        self.assertIn("app_stack_workload_diag_ble_terminal_with_pressure", ble)

    def test_rejects_self_attestation_marker_fabrication_missing_samples_and_replay(self) -> None:
        policy = self.policies["mesh_clicker"]
        build = verifier.verify_build(self._write_build(policy), self.policies, self.frame_limit)
        manifest = self._manifest(policy, build)
        self._replace_manifest(manifest, lambda data: data.__setitem__("schema", 2))
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("trusted schema-3" in issue for issue in captures[0].issues))
        marker_log = self.root / "marker.typescript"
        marker_log.write_text("DBG_STACK_STRESS_BEGIN name=click_spam uptime=1\nDBG_STACK_BEGIN id=1 uptime=2\n", encoding="utf-8")
        manifest = self._manifest(policy, build, marker_log)
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("target-reported" in issue or "missing completed" in issue for issue in captures[0].issues))
        missing_log = self._typed_log(policy, build, ["relay_retry"])
        manifest = self._manifest(policy, build, missing_log)
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("click_activity" in issue for issue in captures[0].issues))
        valid = self._manifest(policy, build)
        data = json.loads(valid.read_text(encoding="utf-8"))
        captures, _ = verifier.verify_hardware([valid], [build], self.policies, True, {policy.preset}, {data["capture_id"]})
        self.assertTrue(any("capture replay" in issue for issue in captures[0].issues))

    def test_rejects_forged_retained_click_identity_and_run_reuse(self) -> None:
        policy = self.policies["mesh_clicker"]
        build = verifier.verify_build(self._write_build(policy), self.policies, self.frame_limit)
        log = self._typed_log(policy, build,
                              ["click_spam", "click_spam", "click_activity"])
        forged = log.read_text(encoding="utf-8").replace(
            "DBG_STACK_RUN_BEGIN epoch=1 run=2 kind=click_spam owner=clicker_action queue=2 custody=1 credit=1 retry=0 drain=2 src=102 dst=200 session=302 seq=402 type=34 sequence=2 previous=1",
            "DBG_STACK_RUN_BEGIN epoch=1 run=2 kind=click_spam owner=clicker_action queue=2 custody=1 credit=1 retry=0 drain=2 src=101 dst=200 session=301 seq=401 type=33 sequence=2 previous=1",
        )
        log.write_text(forged, encoding="utf-8")
        _, issues = verifier.parse_typed_transcript(log.read_text(encoding="utf-8"), policy, build)
        self.assertTrue(any("identity" in issue for issue in issues), issues)

        log = self._typed_log(policy, build,
                              ["click_spam", "click_spam", "click_activity"])
        reused = log.read_text(encoding="utf-8").replace(
            "DBG_STACK_RUN_BEGIN epoch=1 run=2", "DBG_STACK_RUN_BEGIN epoch=1 run=1", 1)
        log.write_text(reused, encoding="utf-8")
        _, issues = verifier.parse_typed_transcript(log.read_text(encoding="utf-8"), policy, build)
        self.assertTrue(any("invalid typed workload run" in issue for issue in issues), issues)

    def test_rejects_hash_identity_time_and_transcript_tampering(self) -> None:
        policy = self.policies["mesh_gateway"]
        build = verifier.verify_build(self._write_build(policy), self.policies, self.frame_limit)
        manifest = self._manifest(policy, build)
        self._replace_manifest(manifest, lambda data: data["artifact"].__setitem__("elf_sha256", "0" * 64), bind=True)
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("artifact hash" in issue for issue in captures[0].issues))
        manifest = self._manifest(policy, build)
        self._replace_manifest(manifest, lambda data: data["target"].__setitem__("build_identity", "imec-stack-v1:wrong:" + "b" * 64), bind=True)
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("target identity" in issue for issue in captures[0].issues))
        manifest = self._manifest(policy, build)
        self._replace_manifest(manifest, lambda data: data["provenance"].__setitem__("ended_at_utc", "2999-01-01T00:00:00Z"), bind=True)
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("future" in issue for issue in captures[0].issues))
        manifest = self._manifest(policy, build)
        log = self.root / json.loads(manifest.read_text(encoding="utf-8"))["transcript"]["path"]
        log.write_text(log.read_text(encoding="utf-8") + "fabricated\n", encoding="utf-8")
        captures, _ = verifier.verify_hardware([manifest], [build], self.policies, True, {policy.preset})
        self.assertTrue(any("SHA-256" in issue for issue in captures[0].issues))


if __name__ == "__main__":
    unittest.main()
