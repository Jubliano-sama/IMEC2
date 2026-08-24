#!/usr/bin/env python3
"""Regression tests for repository-owned typed stack evidence."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from source_text import read_composed_source


REPO_ROOT = Path(__file__).resolve().parents[3]
VERIFIER_PATH = REPO_ROOT / "firmware" / "scripts" / "verify_stack_evidence.py"
POLICY_PATH = REPO_ROOT / "firmware" / "include" / "stack_budget.h"
GATEWAY_CONFIG_PATH = REPO_ROOT / "firmware" / "app" / "prj-gateway.conf"
APP_CONFIG_PATH = REPO_ROOT / "firmware" / "app" / "src" / "app_config.h"
SPEC = importlib.util.spec_from_file_location("verify_stack_evidence", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
verifier = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verifier
SPEC.loader.exec_module(verifier)


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{", source
    )
    if match is None:
        raise AssertionError(f"function definition not found: {name}")
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


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
        if policy.preset == "mesh_gateway":
            config.update(verifier.GATEWAY_FIT_REQUIRED_CONFIG)
        if policy.deployable:
            config.update({
                "CONFIG_IMEC_STACK_DIAGNOSTICS": True,
                "CONFIG_THREAD_MONITOR": True,
                "CONFIG_THREAD_NAME": True,
                "CONFIG_USE_SEGGER_RTT": True,
            })
        if policy.preset in verifier.DURABLE_STATE_PRESETS:
            config.update({
                key: True for key in verifier.DURABLE_STATE_REQUIRED_CONFIG
            })
            config.update({
                "CONFIG_FLASH_LOAD_OFFSET": 0,
                "CONFIG_FLASH_LOAD_SIZE": verifier.DURABLE_STATE_FLASH_LIMIT,
            })
        (zephyr / ".config").write_text("\n".join(_line(key, value) for key, value in config.items()) + "\n", encoding="utf-8")
        if policy.preset in verifier.DURABLE_STATE_PRESETS:
            (zephyr / "zephyr.dts").write_text(
                "/dts-v1/;\n"
                "/ {\n"
                "  storage_partition: partition@7a000 {\n"
                "    reg = < 0x7a000 0x6000 >;\n"
                "    status = \"okay\";\n"
                "  };\n"
                "};\n",
                encoding="utf-8",
            )
        object_path = Path("CMakeFiles/app.dir/src") / f"{source_name}.obj"
        kernel_source = self.root / "kernel.c"
        kernel_source.write_text("void kernel_frame(void) {}\n", encoding="utf-8")
        (build / "build.ninja").write_text(
            "FLAGS = -fstack-usage -fdump-ipa-cgraph\n"
            f"build {object_path}: C_COMPILER__app {source}\n"
            f"build app/libapp.a: C_STATIC_LIBRARY_LINKER__app {object_path}\n"
            "build zephyr/zephyr.elf zephyr/zephyr.map: C_LINK "
            "zephyr/CMakeFiles/zephyr_final.dir/empty.c.obj | app/libapp.a\n"
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
            "Archive member included to satisfy reference by file (symbol)\n\n"
            f"app/libapp.a({object_path.name}) (--whole-archive)\n\n"
            "Memory Configuration\n\nName Origin Length Attributes\n"
            "FLASH 0x0000000000000000 0x0000000000080000 xr\n"
            f"RAM 0x{origin:016x} 0x{size:016x} xw\n\nLinker script and memory map\n"
            f" .text.{function}\n                0x0000000000010000 {function}\n"
            f"                0x{end:016x} _image_ram_end = .\n"
            "                0x0000000000070000 _flash_used = .\n",
            encoding="utf-8"
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
            "anchor_scan": "anchor_uwb_scan",
            "gateway_report_ingress": "mesh_route",
            "gateway_priority_control": "system_workqueue",
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
                    "-a", "0x20000410", "-s", "0x100",
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

    def test_every_production_work_and_timer_callback_has_an_exact_root(self) -> None:
        app_src = REPO_ROOT / "firmware" / "app" / "src"
        roots = verifier.load_thread_roots(POLICY_PATH)
        owners_by_function: dict[str, set[str]] = {}
        for (_source, function), owners in roots.items():
            owners_by_function.setdefault(function, set()).update(owners)

        initialized: set[str] = set()
        timer_handlers: set[str] = set()
        pattern = re.compile(
            r"\bk_(work_init|work_init_delayable|timer_init)\s*\("
            r"\s*[^,]+,\s*([A-Za-z_][A-Za-z0-9_]*)",
            re.DOTALL,
        )
        for path in (*app_src.glob("*.c"), *app_src.glob("*.inc")):
            if path.name == "app_ml.c":
                continue
            for kind, handler in pattern.findall(
                path.read_text(encoding="utf-8")
            ):
                initialized.add(handler)
                if kind == "timer_init":
                    timer_handlers.add(handler)

        self.assertEqual(
            set(),
            initialized - set(owners_by_function),
            "production kernel callbacks are missing stack execution roots",
        )
        for handler in timer_handlers:
            self.assertIn(
                "isr",
                owners_by_function[handler],
                f"k_timer expiry callback {handler} must be charged to ISR stack",
            )
        workload_policy = verifier.load_workload_policy(POLICY_PATH)
        self.assertEqual(verifier.DEPLOYABLE_PRESETS, set(workload_policy))
        self.assertEqual(
            ["click_activity"],
            [item.kind for item in workload_policy["mesh_clicker"]],
        )
        self.assertEqual(
            ["anchor_scan"],
            [item.kind for item in workload_policy["mesh_anchor"]],
        )
        self.assertEqual(
            ["gateway_report_ingress", "gateway_priority_control", "ble_backpressure"],
            [item.kind for item in workload_policy["mesh_gateway"]],
        )
        builds = [verifier.verify_build(self._write_build(policy), self.policies, self.frame_limit) for policy in self.policies.values()]
        self.assertEqual([], [(build.preset, build.issues) for build in builds if build.issues])

    def test_gateway_control_sequence_maintenance_is_charged_to_system_workqueue(self) -> None:
        roots = verifier.load_thread_roots(POLICY_PATH)
        self.assertEqual(
            {"system_workqueue"},
            roots[(
                "app_gateway_control_sequence.c",
                "gateway_control_sequence_maintenance_handler",
            )],
        )

        source = (
            REPO_ROOT / "firmware" / "app" / "src" /
            "app_gateway_control_sequence.c"
        ).read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"k_work_init_delayable\s*\(\s*"
            r"&gateway_control_sequence_maintenance_work\s*,\s*"
            r"gateway_control_sequence_maintenance_handler\s*\)",
        )

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

    def test_ninja_objects_decode_escaped_source_paths(self) -> None:
        ninja = self.root / "escaped-path-build.ninja"
        ninja.write_text(
            "build CMakeFiles/app.dir/src/local.c.obj: "
            "C_COMPILER /tmp/local.c\n"
            "build CMakeFiles/app.dir/src/driver.c.obj: "
            "C_COMPILER /tmp/vendor$ tree/driver.c "
            "| implicit_dep || order_dep |@ validation_dep\n"
            "build app/libapp.a: C_STATIC_LIBRARY_LINKER__app "
            "CMakeFiles/app.dir/src/local.c.obj "
            "CMakeFiles/app.dir/src/driver.c.obj || generated_headers\n",
            encoding="utf-8",
        )

        _text, objects = verifier._ninja_objects(ninja)

        self.assertEqual(
            [(Path("CMakeFiles/app.dir/src/local.c.obj"),
              Path("/tmp/local.c")),
             (Path("CMakeFiles/app.dir/src/driver.c.obj"),
              Path("/tmp/vendor tree/driver.c"))],
            objects,
        )

    def test_ninja_objects_fail_closed_on_unresolved_app_source(self) -> None:
        ninja = self.root / "unresolved-path-build.ninja"
        ninja.write_text(
            "build CMakeFiles/app.dir/src/driver.c.obj: "
            "C_COMPILER $generated_source || generated_headers\n"
            "build app/libapp.a: C_STATIC_LIBRARY_LINKER__app "
            "CMakeFiles/app.dir/src/driver.c.obj\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(
            verifier.EvidenceError, "unsupported Ninja path escape"
        ):
            verifier._ninja_objects(ninja)

    def test_ninja_objects_fail_closed_when_archive_member_is_unmapped(self) -> None:
        ninja = self.root / "hidden-output-build.ninja"
        ninja.write_text(
            "build CMakeFiles/app.dir/src/known.c.obj: "
            "C_COMPILER /tmp/known.c\n"
            "build $app_object: C_COMPILER /tmp/driver.c\n"
            "build app/libapp.a: C_STATIC_LIBRARY_LINKER__app "
            "CMakeFiles/app.dir/src/known.c.obj "
            "CMakeFiles/app.dir/src/driver.c.obj\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(
            verifier.EvidenceError,
            "compile edges differ",
        ):
            verifier._ninja_objects(ninja)

    def test_application_boundary_reconciles_exact_final_link_members(self) -> None:
        build = self.root / "build"
        (build / "zephyr").mkdir(parents=True)
        source = self.root / "application.c"
        source.write_text("void application(void) {}\n", encoding="utf-8")
        object_path = Path("CMakeFiles/app.dir/application.c.obj")
        ninja = (
            f"build {object_path}: C_COMPILER {source}\n"
            f"build app/libapp.a: C_STATIC_LIBRARY_LINKER {object_path}\n"
            "build zephyr/zephyr.elf zephyr/zephyr.map: C_LINK "
            "zephyr/CMakeFiles/zephyr_final.dir/empty.c.obj | app/libapp.a\n"
        )
        (build / "zephyr" / "zephyr.map").write_text(
            "Archive member included to satisfy reference by file (symbol)\n\n"
            "app/libapp.a(application.c.obj) (--whole-archive)\n\n"
            "Linker script and memory map\n",
            encoding="utf-8",
        )

        linked = verifier._linked_application_objects(
            build, ninja, [(object_path, source)]
        )

        self.assertEqual(
            [verifier.LinkedObject(object_path, source, Path("app/libapp.a"))],
            linked,
        )

    def test_application_boundary_rejects_unmapped_final_member(self) -> None:
        build = self.root / "build"
        (build / "zephyr").mkdir(parents=True)
        known_source = self.root / "known.c"
        known_source.write_text("void known(void) {}\n", encoding="utf-8")
        known_object = Path("CMakeFiles/app.dir/known.c.obj")
        ninja = (
            f"build {known_object}: C_COMPILER {known_source}\n"
            f"build app/libapp.a: C_STATIC_LIBRARY_LINKER {known_object}\n"
            "build zephyr/zephyr.elf zephyr/zephyr.map: C_LINK "
            "zephyr/CMakeFiles/zephyr_final.dir/empty.c.obj | "
            "app/libapp.a\n"
        )
        (build / "zephyr" / "zephyr.map").write_text(
            "Archive member included to satisfy reference by file (symbol)\n\n"
            "app/libapp.a(missing.c.obj) (--whole-archive)\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(
            verifier.EvidenceError, "members differ from Ninja archive"
        ):
            verifier._linked_application_objects(
                build, ninja, [(known_object, known_source)]
            )

    def test_live_inline_rows_keep_translation_unit_provenance(self) -> None:
        build = self.root / "build"
        build.mkdir()
        source = self.root / "app.c"
        inline = build / "generated.inc"
        source.write_text("void root(void) {}\n", encoding="utf-8")
        inline.write_text("static void helper(void) {}\n", encoding="utf-8")
        object_path = Path("CMakeFiles/app.dir/src/app.c.obj")
        tu = verifier._translation_unit_key(source, object_path)
        records = [
            verifier.StackUsage(source, 1, "root", 64, "static", tu,
                                str(object_path)),
            verifier.StackUsage(inline, 2, "helper", 96, "static", tu,
                                str(object_path)),
        ]

        selected = verifier._select_live_records(
            {tu: (source, records)},
            {"root"},
            {(tu, "root"): {"helper"}, (tu, "helper"): set()},
            build,
        )

        self.assertEqual(["root", "helper"], [row.function for row in selected])
        self.assertTrue(all(row.translation_unit == tu for row in selected))

    def test_live_unresolved_direct_call_is_a_verifier_issue(self) -> None:
        evidence = self._synchronous_evidence(
            {"root": 64}, {"root": {"missing_vendor_entry"}}
        )

        self.assertTrue(any(
            "unresolved live compiler call" in issue
            for issue in evidence.issues
        ), evidence.issues)

    def test_exact_linked_platform_symbol_ends_application_graph(self) -> None:
        policy = self.policies["mesh_gateway"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("app.c"), 1, "root", 64, "static")
        ]
        graph = {("app.c", "root"): {"k_work_submit"}}

        verifier._attribute_linked_functions(
            evidence,
            policy,
            linked,
            graph,
            {("app.c", "root"): {"main"}},
            self.frame_limit,
            platform_symbols={"k_work_submit"},
        )

        self.assertEqual([], evidence.issues)

    def test_targetless_dispatch_ends_at_runtime_capture_boundary(self) -> None:
        evidence = self._synchronous_evidence(
            {"root": 64}, {"root": {verifier._CGRAPH_INDIRECT_CALL}}
        )

        self.assertEqual([], evidence.issues)

    def test_live_unresolved_indirect_call_is_a_verifier_issue(self) -> None:
        evidence = self._synchronous_evidence(
            {"root": 64},
            {"root": {verifier._CGRAPH_REFERENCE_PREFIX + "missing_callback"}},
        )

        self.assertTrue(any(
            "unresolved live compiler indirect call" in issue
            for issue in evidence.issues
        ), evidence.issues)

    def test_vendor_prefixes_have_no_implicit_abi_contract(self) -> None:
        for symbol in (
            "sd_new_entry",
            "sdc_new_entry",
            "mpsl_new_entry",
            "ocrypto_new_entry",
        ):
            self.assertIsNone(verifier._abi_contract_reason(symbol))

    def test_empty_stack_usage_is_allowed_for_data_only_tu(self) -> None:
        usage = self.root / "flash_map_default.su"
        graph = self.root / "flash_map_default.c.000i.cgraph"
        usage.write_text("", encoding="utf-8")
        graph.write_text(
            "Initial Symbol table:\n"
            "default_flash_map/2 (default_flash_map)\n"
            "  Type: variable definition analyzed\n"
            "  References: \n"
            "Optimized Symbol table:\n"
            "default_flash_map/2 (default_flash_map)\n"
            "  Type: variable definition analyzed\n"
            "Final Symbol table:\n",
            encoding="utf-8",
        )

        records, nodes, synchronous = verifier._parse_tu_compiler_evidence(
            usage,
            graph,
            "flash_map_default.c [flash_map_default.c.obj]",
            Path("flash_map_default.c.obj"),
        )

        self.assertEqual([], records)
        self.assertEqual(
            {("flash_map_default.c [flash_map_default.c.obj]",
              verifier._CGRAPH_VARIABLE_PREFIX + "default_flash_map")},
            set(nodes),
        )
        self.assertEqual({}, synchronous)

    def test_empty_stack_usage_accepts_only_complete_empty_optimized_tu(self) -> None:
        usage = self.root / "empty.c.su"
        graph = self.root / "empty.c.000i.cgraph"
        linker_map = self.root / "zephyr.map"
        linked_object = verifier.LinkedObject(
            Path("zephyr/CMakeFiles/empty.dir/empty.c.obj"),
            self.root / "empty.c",
            Path("zephyr/libempty.a"),
        )
        usage.write_text("", encoding="utf-8")
        graph.write_text(
            "Initial Symbol table:\n\n"
            "unused_inline/1 (unused_inline)\n"
            "  Type: function definition\n"
            "  Calls: \n"
            "Optimized Symbol table:\n\n"
            "Trivially needed variables:\n"
            "Removing variables:\n\n"
            "Final Symbol table:\n",
            encoding="utf-8",
        )
        linker_map.write_text(
            "Archive member included to satisfy reference by file (symbol)\n\n"
            "zephyr/libempty.a(empty.c.obj) (--whole-archive)\n\n"
            "Discarded input sections\n"
            " .text 0x0000000000000000 0x0 "
            "zephyr/libempty.a(empty.c.obj)\n"
            "Linker script and memory map\n",
            encoding="utf-8",
        )

        records, nodes, synchronous = verifier._parse_tu_compiler_evidence(
            usage,
            graph,
            "empty.c [empty.c.obj]",
            Path("empty.c.obj"),
            final_map_path=linker_map,
            linked_object=linked_object,
        )

        self.assertEqual([], records)
        self.assertEqual({}, nodes)
        self.assertEqual({}, synchronous)

        graph.write_text(
            graph.read_text(encoding="utf-8").replace(
                "Trivially needed variables:\n",
                "unexpected truncated evidence\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            verifier.EvidenceError, "no analyzed definitions"
        ):
            verifier._parse_tu_compiler_evidence(
                usage,
                graph,
                "empty.c [empty.c.obj]",
                Path("empty.c.obj"),
                final_map_path=linker_map,
                linked_object=linked_object,
            )

        usage.write_text(
            f"{self.root / 'empty.c'}:1:1:lost_frame\t64\tstatic\n",
            encoding="utf-8",
        )
        graph.write_text(
            "Initial Symbol table:\n\n"
            "Optimized Symbol table:\n\n"
            "Final Symbol table:\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            verifier.EvidenceError, "no analyzed definitions"
        ):
            verifier._parse_tu_compiler_evidence(
                usage,
                graph,
                "empty.c [empty.c.obj]",
                Path("empty.c.obj"),
                final_map_path=linker_map,
                linked_object=linked_object,
            )

        usage.write_text("", encoding="utf-8")
        linker_map.write_text(
            linker_map.read_text(encoding="utf-8")
            + " .text.live_function\n"
            + "                0x0000000000010000 0x20 "
            + "zephyr/libempty.a(empty.c.obj)\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            verifier.EvidenceError, "no analyzed definitions"
        ):
            verifier._parse_tu_compiler_evidence(
                usage,
                graph,
                "empty.c [empty.c.obj]",
                Path("empty.c.obj"),
                final_map_path=linker_map,
                linked_object=linked_object,
            )

    def test_empty_stack_usage_is_rejected_when_cgraph_has_function(self) -> None:
        usage = self.root / "function.su"
        graph = self.root / "function.c.000i.cgraph"
        usage.write_text("", encoding="utf-8")
        graph.write_text(
            "Initial Symbol table:\n"
            "function/1 (function)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n"
            "Optimized Symbol table:\n"
            "function/1 (function)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n"
            "Final Symbol table:\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(
            verifier.EvidenceError,
            "empty compiler stack evidence",
        ):
            verifier._parse_tu_compiler_evidence(
                usage,
                graph,
                "function.c [function.c.obj]",
                Path("function.c.obj"),
            )

    def test_live_data_reference_is_resolved_as_a_variable_node(self) -> None:
        policy = self.policies["mesh_gateway"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [verifier.StackUsage(Path("root.c"), 1, "root", 64, "static")]
        graph = {
            ("root.c", "root"): {
                verifier._CGRAPH_DATA_REFERENCE_PREFIX + "shared_state"
            },
            ("state.c", verifier._CGRAPH_VARIABLE_PREFIX + "shared_state"): set(),
        }

        verifier._attribute_linked_functions(
            evidence,
            policy,
            linked,
            graph,
            {("root.c", "root"): {"main"}},
            self.frame_limit,
        )

        self.assertEqual([], evidence.issues)

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

    def test_exact_gateway_fit_booleans_are_generated_config_contract(self) -> None:
        policy = self.policies["mesh_gateway"]
        expected = {
            "CONFIG_ADC": False,
            "CONFIG_BT_CTLR_ECDH": False,
            "CONFIG_BT_CTLR_LE_ENC": False,
            "CONFIG_BT_ASSERT_VERBOSE": False,
            "CONFIG_BT_GATT_CACHING": False,
            "CONFIG_BT_GATT_READ_MULTIPLE": False,
            "CONFIG_BT_GATT_READ_MULT_VAR_LEN": False,
            "CONFIG_BT_GATT_SERVICE_CHANGED": True,
            "CONFIG_BT_CTLR_CRYPTO": True,
        }
        self.assertEqual(expected, verifier.GATEWAY_FIT_REQUIRED_CONFIG)

        accepted = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        self.assertEqual([], accepted.issues)

        for key, required in expected.items():
            with self.subTest(key=key):
                build = self._write_build(policy)
                config = build / "zephyr" / ".config"
                retained = [
                    line
                    for line in config.read_text(encoding="utf-8").splitlines()
                    if line != f"# {key} is not set"
                    and not line.startswith(f"{key}=")
                ]
                retained.append(_line(key, not required))
                config.write_text("\n".join(retained) + "\n", encoding="utf-8")

                rejected = verifier.verify_build(
                    build, self.policies, self.frame_limit
                )
                self.assertTrue(
                    any(key in issue for issue in rejected.issues),
                    rejected.issues,
                )

    def test_gateway_fit_booleans_do_not_constrain_non_gateway_presets(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = self._write_build(policy)
        config = build / "zephyr" / ".config"
        config.write_text(
            config.read_text(encoding="utf-8")
            + "\n".join(
                _line(key, not required)
                for key, required in verifier.GATEWAY_FIT_REQUIRED_CONFIG.items()
            )
            + "\n",
            encoding="utf-8",
        )

        evidence = verifier.verify_build(
            build, self.policies, self.frame_limit
        )
        self.assertEqual([], evidence.issues)

    def test_watchdog_bypass_is_bench_only_and_cannot_promote(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = self._write_build(policy)
        config = build / "zephyr" / ".config"
        config.write_text(
            config.read_text(encoding="utf-8")
            + "CONFIG_IMEC_WATCHDOG_BYPASS=y\n",
            encoding="utf-8",
        )

        production = verifier.verify_build(
            build, self.policies, self.frame_limit
        )
        self.assertTrue(
            any("bench-only" in issue for issue in production.issues),
            production.issues,
        )

        bench = verifier.verify_build(
            build,
            self.policies,
            self.frame_limit,
            allow_watchdog_bypass=True,
        )
        self.assertEqual([], bench.issues)

    def test_watchdog_bypass_is_limited_to_the_forcedhop_anchor_bench(self) -> None:
        forcedhop = self.policies["mesh_anchor_forcedhop"]
        forcedhop_build = self._write_build(forcedhop)
        forcedhop_config = forcedhop_build / "zephyr" / ".config"
        forcedhop_config.write_text(
            forcedhop_config.read_text(encoding="utf-8")
            + "CONFIG_IMEC_WATCHDOG_BYPASS=y\n",
            encoding="utf-8",
        )
        accepted = verifier.verify_build(
            forcedhop_build, self.policies, self.frame_limit
        )
        self.assertEqual([], accepted.issues)
        self.assertNotIn(
            "mesh_anchor_forcedhop", verifier.DEPLOYABLE_PRESETS
        )

        transmitter = self.policies["mesh_transmitter_forcedhop"]
        transmitter_build = self._write_build(transmitter)
        transmitter_config = transmitter_build / "zephyr" / ".config"
        transmitter_config.write_text(
            transmitter_config.read_text(encoding="utf-8")
            + "CONFIG_IMEC_WATCHDOG_BYPASS=y\n",
            encoding="utf-8",
        )
        rejected = verifier.verify_build(
            transmitter_build,
            self.policies,
            self.frame_limit,
            allow_watchdog_bypass=True,
        )
        self.assertTrue(
            any("not allowed" in issue for issue in rejected.issues),
            rejected.issues,
        )

    def test_durable_state_features_are_required_for_deployable_and_bench_images(self) -> None:
        for preset in sorted(verifier.DURABLE_STATE_PRESETS):
            with self.subTest(preset=preset):
                policy = self.policies[preset]
                for key in ("CONFIG_IMEC_DURABLE_STATE", "CONFIG_NVS"):
                    build = self._write_build(policy)
                    config = build / "zephyr" / ".config"
                    config.write_text(
                        "\n".join(
                            line
                            for line in config.read_text(encoding="utf-8").splitlines()
                            if not line.startswith(f"{key}=")
                        ) + "\n",
                        encoding="utf-8",
                    )
                    rejected = verifier.verify_build(
                        build, self.policies, self.frame_limit
                    )
                    self.assertTrue(
                        any(key in issue for issue in rejected.issues),
                        rejected.issues,
                    )

    def test_durable_state_storage_partition_must_be_present_enabled_and_usable(self) -> None:
        policy = self.policies["mesh_gateway"]

        build = self._write_build(policy)
        dts = build / "zephyr" / "zephyr.dts"
        dts.write_text("/dts-v1/; / {};\n", encoding="utf-8")
        missing = verifier.verify_build(build, self.policies, self.frame_limit)
        self.assertTrue(
            any("lacks storage_partition" in issue for issue in missing.issues),
            missing.issues,
        )

        build = self._write_build(policy)
        dts = build / "zephyr" / "zephyr.dts"
        dts.write_text(
            dts.read_text(encoding="utf-8").replace(
                'status = "okay";', 'status = "disabled";'
            ),
            encoding="utf-8",
        )
        disabled = verifier.verify_build(build, self.policies, self.frame_limit)
        self.assertTrue(
            any("status is 'disabled'" in issue for issue in disabled.issues),
            disabled.issues,
        )

        build = self._write_build(policy)
        dts = build / "zephyr" / "zephyr.dts"
        dts.write_text(
            dts.read_text(encoding="utf-8").replace(
                "< 0x7a000 0x6000 >", "< 0x7a001 0x1000 >"
            ),
            encoding="utf-8",
        )
        unusable = verifier.verify_build(build, self.policies, self.frame_limit)
        self.assertTrue(
            any("not 4096-byte aligned" in issue for issue in unusable.issues),
            unusable.issues,
        )
        self.assertTrue(
            any("size 4096 is below 8192" in issue for issue in unusable.issues),
            unusable.issues,
        )

        build = self._write_build(policy)
        linker_map = build / "zephyr" / "zephyr.map"
        linker_map.write_text(
            linker_map.read_text(encoding="utf-8").replace(
                "0x0000000000070000 _flash_used",
                "0x000000000007b000 _flash_used",
            ),
            encoding="utf-8",
        )
        overlapping = verifier.verify_build(
            build, self.policies, self.frame_limit
        )
        self.assertTrue(
            any("overlaps storage_partition" in issue
                for issue in overlapping.issues),
            overlapping.issues,
        )

    def test_durable_state_accepts_exact_partition_boundary_and_rejects_overlap(self) -> None:
        policy = self.policies["mesh_gateway"]

        boundary_build = self._write_build(policy)
        boundary_map = boundary_build / "zephyr" / "zephyr.map"
        boundary_map.write_text(
            boundary_map.read_text(encoding="utf-8").replace(
                "0x0000000000070000 _flash_used",
                "0x000000000007a000 _flash_used",
            ),
            encoding="utf-8",
        )
        boundary = verifier.verify_build(
            boundary_build, self.policies, self.frame_limit
        )
        self.assertEqual([], boundary.issues)

        overlap_build = self._write_build(policy)
        overlap_map = overlap_build / "zephyr" / "zephyr.map"
        overlap_map.write_text(
            overlap_map.read_text(encoding="utf-8").replace(
                "0x0000000000070000 _flash_used",
                "0x000000000007a001 _flash_used",
            ),
            encoding="utf-8",
        )
        overlap = verifier.verify_build(
            overlap_build, self.policies, self.frame_limit
        )
        self.assertTrue(
            any(
                "linked image [0x0,0x7a001) overlaps "
                "storage_partition [0x7a000,0x80000)" in issue
                for issue in overlap.issues
            ),
            overlap.issues,
        )

    def test_durable_state_gate_does_not_apply_to_synthetic_transmitter(self) -> None:
        policy = self.policies["mesh_transmitter_forcedhop"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        self.assertEqual([], build.issues)

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

    def test_compiler_cmake_source_path_resolves_to_worktree_application(self) -> None:
        application = self.root / "worktree" / "firmware" / "app"
        expected = application / "src" / "app_anchor.c"

        self.assertEqual(
            expected.resolve(),
            verifier._resolve_compiler_source(
                Path("CMAKE_SOURCE_DIR/src/app_anchor.c"),
                application_source_dir=application,
            ),
        )

        usage = self.root / "app_anchor.c.su"
        usage.write_text(
            "CMAKE_SOURCE_DIR/src/app_anchor.c:12:3:worker\t64\tstatic\n",
            encoding="utf-8",
        )
        records = verifier._parse_su(
            usage,
            application_source_dir=application,
        )
        self.assertEqual(expected.resolve(), records[0].source)

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

    def test_anchor_capture_requires_scan_owner_queue_with_exact_stack(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = verifier.BuildEvidence(
            self.root,
            preset="mesh_anchor",
            config={
                "CONFIG_STACK_ALIGN_DOUBLE_WORD": True,
                "CONFIG_MPU_STACK_GUARD": True,
                "CONFIG_ARM_MPU_REGION_MIN_ALIGN_AND_SIZE": "32",
            },
        )
        required = verifier._required_threads(build, policy)

        self.assertEqual(required["anchor_uwb_scan"], 8192)
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

    def test_anchor_scan_capacity_covers_measured_ddd_runtime_watermark(self) -> None:
        """Keep source, verifier, and the hardware margin gate synchronized."""
        source = APP_CONFIG_PATH.read_text(encoding="utf-8")
        match = re.search(
            r"^#define ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE (\d+)u$",
            source,
            re.MULTILINE,
        )
        self.assertIsNotNone(match)
        configured = int(match.group(1))
        observed_used = 6472

        for preset in ("mesh_anchor", "mesh_anchor_forcedhop"):
            with self.subTest(preset=preset):
                policy = self.policies[preset]
                build = verifier.BuildEvidence(
                    self.root,
                    preset=preset,
                    config={
                        "CONFIG_STACK_ALIGN_DOUBLE_WORD": True,
                        "CONFIG_MPU_STACK_GUARD": True,
                        "CONFIG_ARM_MPU_REGION_MIN_ALIGN_AND_SIZE": "32",
                    },
                )
                owner_capacity = verifier._owner_capacity(
                    policy, "anchor_uwb_scan"
                )
                runtime_size = verifier._required_threads(
                    build, policy
                )["anchor_uwb_scan"]
                observed_free = runtime_size - observed_used

                self.assertEqual(configured, owner_capacity)
                self.assertEqual(configured, runtime_size)
                self.assertGreaterEqual(
                    observed_free,
                    verifier._required_free(runtime_size),
                    "the configured anchor scan queue does not retain the "
                    "20 percent policy reserve above the 6472-byte DDD "
                    "hardware watermark",
                )

                rows = {
                    name: (
                        size - verifier._required_free(size),
                        verifier._required_free(size),
                        size,
                    )
                    for name, size in verifier._required_threads(
                        build, policy
                    ).items()
                }
                rows["anchor_uwb_scan"] = (
                    observed_used,
                    observed_free,
                    runtime_size,
                )
                self.assertEqual(
                    [], verifier._check_sample_rows(rows, policy, build)
                )

    def test_old_anchor_scan_capacity_rejects_measured_ddd_watermark(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = verifier.BuildEvidence(
            self.root,
            preset="mesh_anchor",
            config={
                "CONFIG_STACK_ALIGN_DOUBLE_WORD": True,
                "CONFIG_MPU_STACK_GUARD": True,
                "CONFIG_ARM_MPU_REGION_MIN_ALIGN_AND_SIZE": "32",
            },
        )
        required = verifier._required_threads(build, policy)
        rows = {
            name: (
                size - verifier._required_free(size),
                verifier._required_free(size),
                size,
            )
            for name, size in required.items()
        }
        rows["anchor_uwb_scan"] = (6472, 760, 7232)

        issues = verifier._check_sample_rows(rows, policy, build)

        self.assertTrue(any(
            "anchor_uwb_scan differs from generated config" in issue
            for issue in issues
        ), issues)
        self.assertTrue(any(
            "free space below policy for anchor_uwb_scan" in issue
            for issue in issues
        ), issues)

    def test_gateway_runtime_model_uses_only_live_exact_workqueues(self) -> None:
        policy = self.policies["mesh_gateway"]
        gateway_config = verifier.parse_kconfig(GATEWAY_CONFIG_PATH)

        self.assertGreaterEqual(policy.bt_rx_bytes, 1536)
        self.assertEqual(
            policy.bt_rx_bytes,
            gateway_config["CONFIG_BT_RX_STACK_SIZE"],
        )
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )
        self.assertEqual([], build.issues)
        required = verifier._required_threads(build, policy)

        self.assertNotIn("main", required)
        self.assertNotIn("BT HCI TX", required)
        self.assertNotIn("BT RX", required)
        self.assertEqual(4480, required["sysworkq"])
        self.assertEqual(8192, required["mesh_route"])
        self.assertEqual(policy.bt_rx_bytes, required["BT RX WQ"])
        self.assertGreaterEqual(required["BT RX WQ"], 1536)
        self.assertEqual(1344, required["BT LW WQ"])

        rows = {
            name: (size - verifier._required_free(size, name in {
                "logging", "BT RX WQ", "BT LW WQ", "MPSL Work", "idle",
            }), verifier._required_free(size, name in {
                "logging", "BT RX WQ", "BT LW WQ", "MPSL Work", "idle",
            }), size)
            for name, size in required.items()
        }
        observed_used = 1064
        corrected_size = required["BT RX WQ"]
        corrected_free = corrected_size - observed_used
        self.assertGreaterEqual(
            corrected_free,
            verifier._required_free(corrected_size, service=True),
        )
        rows["BT RX WQ"] = (
            observed_used,
            corrected_free,
            corrected_size,
        )
        self.assertEqual([], verifier._check_sample_rows(rows, policy, build))

        old_size = 1088
        old_free = old_size - observed_used
        self.assertLess(
            old_free,
            verifier._required_free(old_size, service=True),
        )
        rows["BT RX WQ"] = (observed_used, old_free, old_size)
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
            "Optimized Symbol table:\n"
            "root/1 (root)\n"
            "  Type: function definition analyzed\n"
            "  References: callback/2 (addr)\n"
            "  Calls: \n"
            "callback/2 (callback)\n"
            "  Type: function definition analyzed\n"
            "  Calls: \n"
            "Final Symbol table:\n",
            encoding="utf-8",
        )
        parsed = verifier._parse_cgraph(graph, "callback.c")
        self.assertEqual(
            {verifier._CGRAPH_REFERENCE_PREFIX + "callback"},
            parsed[("callback.c", "root")],
        )

    def test_data_address_is_not_misclassified_as_a_callback(self) -> None:
        graph = self.root / "data-ref.c.c.000i.cgraph"
        graph.write_text(
            "Optimized Symbol table:\n"
            "root/1 (root)\n"
            "  Type: function definition analyzed\n"
            "  References: __region_start/2 (addr)\n"
            "  Calls: \n"
            "__region_start/2 (__region_start)\n"
            "  Type: variable\n"
            "  References: \n"
            "Final Symbol table:\n",
            encoding="utf-8",
        )

        parsed = verifier._parse_cgraph(graph, "data-ref.c")

        self.assertEqual(
            {verifier._CGRAPH_DATA_REFERENCE_PREFIX + "__region_start"},
            parsed[("data-ref.c", "root")],
        )

    def test_tu_qualified_callback_root_stops_inherited_owner(self) -> None:
        policy = self.policies["mesh_gateway"]
        evidence = verifier.BuildEvidence(self.root)
        main_tu = "firmware/app/src/main.c [CMakeFiles/app.dir/main.c.obj]"
        worker_tu = (
            "firmware/app/src/worker.c [CMakeFiles/app.dir/worker.c.obj]"
        )
        linked = [
            verifier.StackUsage(Path("main.c"), 1, "main", 64, "static",
                                main_tu),
            verifier.StackUsage(Path("worker.c"), 1, "work_handler", 64,
                                "static", worker_tu),
            verifier.StackUsage(Path("worker.c"), 2, "worker_helper", 4097,
                                "static", worker_tu),
        ]
        graph = {
            (main_tu, "main"): {
                verifier._CGRAPH_REFERENCE_PREFIX + "work_handler"
            },
            (worker_tu, "work_handler"): {"worker_helper"},
            (worker_tu, "worker_helper"): set(),
        }
        roots = {
            ("main.c", "main"): {"main"},
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

    def test_compiler_func_strings_do_not_join_unrelated_functions(self) -> None:
        graph = self.root / "func-string.c.c.000i.cgraph"
        graph.write_text(
            "Optimized Symbol table:\n"
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
            "  Varpool flags: initialized read-only const-value-known\n"
            "Final Symbol table:\n",
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
            "Optimized Symbol table:\n"
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
            "  Calls: \n"
            "Final Symbol table:\n",
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

    def test_unique_inlined_cross_file_intermediary_preserves_ownership(self) -> None:
        policy = self.policies["mesh_anchor"]
        evidence = verifier.BuildEvidence(self.root)
        linked = [
            verifier.StackUsage(Path("main.c"), 1, "main", 64, "static"),
            verifier.StackUsage(Path("service.c"), 1, "service_leaf", 64,
                                "static"),
        ]
        graph = {
            ("main.c", "main"): {"service_init"},
            # GCC may inline this public cross-file initializer completely, so
            # it remains in the IPA graph but has no linked stack-usage row.
            ("service.c", "service_init"): {"service_leaf"},
            ("service.c", "service_leaf"): set(),
        }

        verifier._attribute_linked_functions(
            evidence,
            policy,
            linked,
            graph,
            {("main.c", "main"): {"main"}},
            self.frame_limit,
        )

        self.assertEqual([], evidence.issues)
        self.assertEqual(2, evidence.attributed_usage_count)

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

        other_role = self._typed_log(policy, build, ["anchor_scan"])
        _, issues = verifier.parse_typed_transcript(
            other_role.read_text(encoding="utf-8"), policy, build
        )
        self.assertTrue(any("gateway_report_ingress" in issue for issue in issues),
                        issues)

    def test_required_workloads_have_genuine_role_call_sites(self) -> None:
        clicker = (REPO_ROOT / "firmware" / "app" / "src" /
                   "app_clicker.c").read_text(encoding="utf-8")
        anchor = read_composed_source(
            REPO_ROOT / "firmware" / "app" / "src" / "app_anchor.c"
        )
        gateway = read_composed_source(
            REPO_ROOT / "firmware" / "app" / "src" / "app_anchor.c"
        )
        mesh_report = read_composed_source(
            REPO_ROOT / "firmware" / "app" / "src" / "app_mesh_report.c"
        )
        ble = read_composed_source(
            REPO_ROOT / "firmware" / "app" / "src" / "app_gateway_ble.c"
        )

        self.assertIn("dwm3000_driver_range_initiator", clicker)
        self.assertIn("app_stack_workload_diag_click_activity_sample", clicker)
        self.assertIn("anchor_uwb_scan_work_handler", anchor)
        self.assertIn(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity",
            anchor,
        )
        self.assertIn(
            "app_stack_workload_diag_gateway_report_cycle", mesh_report
        )
        self.assertIn("app_node_comm_submit_delivery", gateway)
        self.assertIn("gateway_discovery_assignment_service_delivery", gateway)
        self.assertIn("app_stack_workload_diag_gateway_control_sample", gateway)
        self.assertIn("app_stack_workload_diag_ble_admit_with_pressure", ble)
        self.assertIn("app_stack_workload_diag_ble_terminal_with_pressure", ble)

    def test_gateway_fifo_handoff_crosses_an_async_stack_boundary(self) -> None:
        gateway = read_composed_source(
            REPO_ROOT / "firmware" / "app" / "src" / "app_anchor.c"
        )
        body = function_body(
            gateway,
            "gateway_host_command_submit_next_queued",
        )

        self.assertEqual(0, body.count("gateway_host_command_submit_next_queued"))
        self.assertNotIn("gateway_host_command_submit_priority(", body)
        self.assertIn(
            "k_work_reschedule(&gateway_host_command_retry_work, K_NO_WAIT)",
            body,
        )

    def test_spi_crc_register_read_does_not_reenter_transfer_engine(self) -> None:
        driver = (
            REPO_ROOT /
            "dwm3000 examples and sdk" /
            "decadriver" /
            "deca_device.c"
        ).read_text(encoding="utf-8")
        start = driver.index("void dwt_xfer3000\n(")
        end = driver.index("} // end dwt_xfer3000()", start)
        body = driver[start:end]

        self.assertNotIn("dwcrc8 = dwt_read8bitoffsetreg(", body)
        self.assertIn(
            "readfromspi(sizeof(crc_header), crc_header,",
            body,
        )

    def test_every_dwm3000_build_uses_the_pinned_worktree_resolver(self) -> None:
        helper = (
            REPO_ROOT / "firmware" / "cmake" / "dwm3000_sdk.cmake"
        ).read_text(encoding="utf-8")
        self.assertIn("70231425cbadc83e1d1a8b526868e3461391dd9b", helper)
        self.assertIn("${ZEPHYR_BASE}/..", helper)
        self.assertIn("rev-parse HEAD", helper)

        for application in (
            "app", "uwb_smoke_test", "twr_range_test", "power_profile_test",
        ):
            with self.subTest(application=application):
                cmake = (
                    REPO_ROOT / "firmware" / application / "CMakeLists.txt"
                ).read_text(encoding="utf-8")
                self.assertIn(
                    "include(\"${CMAKE_CURRENT_LIST_DIR}/../cmake/dwm3000_sdk.cmake\")",
                    cmake,
                )
                self.assertIn(
                    "imec_resolve_dwm3000_sdk(DWM3000_SDK_DIR)", cmake,
                )

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

    def test_capture_wall_clock_allows_only_bounded_process_teardown(self) -> None:
        policy = self.policies["mesh_anchor"]
        build = verifier.verify_build(
            self._write_build(policy), self.policies, self.frame_limit
        )

        manifest = self._manifest(policy, build)
        data = json.loads(manifest.read_text(encoding="utf-8"))
        ended = datetime.fromisoformat(
            data["provenance"]["ended_at_utc"].replace("Z", "+00:00")
        )
        data["provenance"]["started_at_utc"] = (
            ended - verifier.MAX_CAPTURE_DURATION - timedelta(seconds=1)
        ).isoformat().replace("+00:00", "Z")
        data["capture_id"] = verifier._capture_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        captures, _ = verifier.verify_hardware(
            [manifest], [build], self.policies, True, {policy.preset}
        )
        self.assertFalse(
            any("wall-clock bounds" in issue for issue in captures[0].issues),
            captures[0].issues,
        )

        data["provenance"]["started_at_utc"] = (
            ended
            - verifier.MAX_CAPTURE_DURATION
            - verifier.MAX_CAPTURE_PROCESS_OVERHEAD
            - timedelta(seconds=1)
        ).isoformat().replace("+00:00", "Z")
        data["capture_id"] = verifier._capture_id(data)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        captures, _ = verifier.verify_hardware(
            [manifest], [build], self.policies, True, {policy.preset}
        )
        self.assertTrue(
            any("wall-clock bounds" in issue for issue in captures[0].issues),
            captures[0].issues,
        )


if __name__ == "__main__":
    unittest.main()
