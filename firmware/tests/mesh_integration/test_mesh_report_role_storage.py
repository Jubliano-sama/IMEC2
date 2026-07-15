#!/usr/bin/env python3
"""Compile-role storage guards for the connected mesh report runtime."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

from source_text import read_composed_source


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
REPORT_SOURCE = FIRMWARE_ROOT / "app" / "src" / "app_mesh_report.c"
ANCHOR_SOURCE = FIRMWARE_ROOT / "app" / "src" / "app_anchor.c"
CAPACITY_HEADER = FIRMWARE_ROOT / "include" / "mesh_capacity.h"
PENDING_SYMBOL = re.compile(r"\bmesh_ch9_tx_pending\b")
ANCHOR_ROLE_GUARD = re.compile(r"DEVICE_ROLE\s*==\s*ROLE_ANCHOR")


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


def _unguarded_pending_symbol_lines(source: str) -> list[int]:
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

        if PENDING_SYMBOL.search(line) and not any(
            ANCHOR_ROLE_GUARD.search(condition) for condition in conditions
        ):
            failures.append(number)
    if conditions:
        raise AssertionError("unterminated preprocessor conditional")
    return failures


class MeshReportRoleStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.report_source = read_composed_source(REPORT_SOURCE)
        cls.anchor_source = read_composed_source(ANCHOR_SOURCE)
        cls.capacity_header = CAPACITY_HEADER.read_text(encoding="utf-8")

    def test_pending_batch_is_anchor_compile_role_only(self) -> None:
        failures = _unguarded_pending_symbol_lines(self.report_source)
        self.assertEqual(
            [],
            failures,
            "pending batch references compile outside ROLE_ANCHOR at lines "
            + ",".join(str(line) for line in failures),
        )
        self.assertIn("sizeof(mesh_ch9_tx_pending) == 4184u", self.report_source)

    def test_gateway_store_is_initialized_then_attached_after_relay_init(self) -> None:
        self.assertIn("sizeof(mesh_gateway_ack_store) == 4000u", self.report_source)
        self.assertIn("mesh_gateway_ack_store_init(&mesh_gateway_ack_store)",
                      self.report_source)
        self.assertNotIn("struct mesh_gateway_ack_store", self.anchor_source)

        start = self.anchor_source.index("int app_anchor_start_gateway_role(void)")
        body = self.anchor_source[start:self.anchor_source.index("\n}", start) + 2]
        relay_init = body.index("mesh_relay_init(&mesh_runtime")
        attach = body.index("app_mesh_report_attach_gateway_ack_store()")
        self.assertLess(relay_init, attach)
        self.assertIn("if (ret < 0)", body[attach:])

    def test_shared_capacity_names_nominal_and_recovery_storage(self) -> None:
        self.assertIn("MESH_CONNECTED_MAX_ANCHORS 50u", self.capacity_header)
        self.assertIn("MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH 4u",
                      self.capacity_header)
        self.assertIn("MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY 1u",
                      self.capacity_header)
        self.assertIn("MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY",
                      self.capacity_header)


if __name__ == "__main__":
    unittest.main()
