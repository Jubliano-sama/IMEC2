#!/usr/bin/env python3
"""Role-specific RAM and stack-owner guards for forced-hop firmware."""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
APP_ROOT = FIRMWARE_ROOT / "app"
ANCHOR_SOURCE = APP_ROOT / "src" / "app_anchor.c"
APP_CONFIG = APP_ROOT / "src" / "app_config.h"
MESH_TEST_SOURCE = APP_ROOT / "src" / "app_mesh_test.c"
APP_CMAKE = APP_ROOT / "CMakeLists.txt"
POLICY_HEADER = FIRMWARE_ROOT / "include" / "stack_budget.h"
VERIFIER_PATH = FIRMWARE_ROOT / "scripts" / "verify_stack_evidence.py"

SPEC = importlib.util.spec_from_file_location(
    "forcedhop_verify_stack_evidence", VERIFIER_PATH
)
assert SPEC is not None and SPEC.loader is not None
verifier = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verifier
SPEC.loader.exec_module(verifier)

TRANSMITTER_GUARD = re.compile(
    r"(?:!\s*defined\s*(?:\(\s*)?CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER|"
    r"!\s*\(.*defined\s*\(\s*CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)"
)
ANCHOR_SCAN_QUEUE_SYMBOL = re.compile(
    r"\banchor_uwb_scan_work_q(?:_stack|_config)?\b"
)


def _logical_lines(source: str) -> list[tuple[int, str]]:
    result: list[tuple[int, str]] = []
    pending = ""
    start = 0
    for number, raw in enumerate(source.splitlines(), start=1):
        stripped = raw.rstrip()
        if not pending:
            start = number
        pending += stripped[:-1] + " " if stripped.endswith("\\") else stripped
        if not stripped.endswith("\\"):
            result.append((start, pending))
            pending = ""
    if pending:
        result.append((start, pending))
    return result


def _unguarded_queue_symbol_lines(source: str) -> list[int]:
    conditions: list[str] = []
    failures: list[int] = []
    for number, line in _logical_lines(source):
        directive = re.match(
            r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line
        )
        if directive is not None:
            kind, argument = directive.group(1), directive.group(2).strip()
            if kind == "if":
                conditions.append(argument)
            elif kind == "ifdef":
                conditions.append(f"defined({argument})")
            elif kind == "ifndef":
                conditions.append(f"!defined({argument})")
            elif kind == "elif":
                if not conditions:
                    raise AssertionError(f"orphan #elif at line {number}")
                conditions[-1] = argument
            elif kind == "else":
                if not conditions:
                    raise AssertionError(f"orphan #else at line {number}")
                conditions[-1] = f"!({conditions[-1]})"
            else:
                if not conditions:
                    raise AssertionError(f"orphan #endif at line {number}")
                conditions.pop()
            continue

        if ANCHOR_SCAN_QUEUE_SYMBOL.search(line) and not any(
            TRANSMITTER_GUARD.search(condition) for condition in conditions
        ):
            failures.append(number)
    if conditions:
        raise AssertionError("unterminated preprocessor conditional")
    return failures


class ForcedHopRamPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policies, _ = verifier.load_policy(POLICY_HEADER)
        cls.forcedhop = cls.policies["mesh_transmitter_forcedhop"]

    def test_forcedhop_preset_keeps_split_stack_budget(self) -> None:
        cmake = APP_CMAKE.read_text(encoding="utf-8")
        transmitter = cmake.split(
            "if(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD)", 1
        )[1].split("find_package(Zephyr", 1)[0]

        self.assertIn('"CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=8192\\n"', transmitter)
        self.assertNotIn("CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=16384", transmitter)
        self.assertEqual(8192, self.forcedhop.system_workqueue_bytes)
        self.assertEqual(9216, self.forcedhop.mesh_route_bytes)
        self.assertGreaterEqual(
            self.forcedhop.minimum_static_ram_headroom_bytes, 18432
        )

    def test_forcedhop_cannot_allocate_or_use_anchor_scan_queue(self) -> None:
        failures = _unguarded_queue_symbol_lines(
            ANCHOR_SOURCE.read_text(encoding="utf-8")
        )
        self.assertEqual(
            [],
            failures,
            "anchor scan queue references compile into forced-hop at lines "
            + ",".join(str(line) for line in failures),
        )

    def test_forcedhop_owns_mesh_test_not_anchor_scan(self) -> None:
        roots = verifier.load_thread_roots(POLICY_HEADER)

        self.assertEqual(
            {"mesh_test"},
            roots[("app_mesh_test.c", "mesh_test_tx_thread_entry")],
        )
        self.assertEqual(8192, verifier._owner_capacity(self.forcedhop, "mesh_test"))
        self.assertEqual(
            0, verifier._owner_capacity(self.forcedhop, "anchor_uwb_scan")
        )

    def test_mesh_test_stack_is_explicitly_eight_kib(self) -> None:
        config = APP_CONFIG.read_text(encoding="utf-8")
        mesh_test = MESH_TEST_SOURCE.read_text(encoding="utf-8")

        self.assertIn("#define MESH_TEST_WORKQUEUE_STACK_SIZE 8192u", config)
        self.assertIn(
            "K_THREAD_STACK_DEFINE(mesh_test_thread_stack, "
            "MESH_TEST_WORKQUEUE_STACK_SIZE)",
            mesh_test,
        )
        self.assertIn("mesh_test_tx_thread_entry", mesh_test)


if __name__ == "__main__":
    unittest.main()
