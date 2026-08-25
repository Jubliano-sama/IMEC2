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
RELAY_HEADER = FIRMWARE_ROOT / "include" / "mesh_relay.h"
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


def _function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


class MeshReportRoleStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.report_source = read_composed_source(REPORT_SOURCE)
        cls.anchor_source = read_composed_source(ANCHOR_SOURCE)
        cls.capacity_header = CAPACITY_HEADER.read_text(encoding="utf-8")
        cls.relay_header = RELAY_HEADER.read_text(encoding="utf-8")

    def test_pending_batch_is_anchor_compile_role_only(self) -> None:
        failures = _unguarded_pending_symbol_lines(self.report_source)
        self.assertEqual(
            [],
            failures,
            "pending batch references compile outside ROLE_ANCHOR at lines "
            + ",".join(str(line) for line in failures),
        )
        self.assertIn("sizeof(mesh_ch9_tx_pending) == 4184u", self.report_source)
        self.assertIn("mesh_ch9_tx_batch_storage.pending", self.report_source)
        self.assertIn("mesh_ch9_tx_batch_storage.candidates", self.report_source)
        self.assertIn(
            "sizeof(mesh_ch9_tx_batch_storage) == 4184u",
            self.report_source,
        )
        self.assertIn(
            "MESH_DIRECT_GATEWAY_BATCHING_ENABLED == 0",
            self.report_source,
        )
        self.assertNotIn(
            "mesh_ch9_batch_payload_scratch", self.report_source
        )
        self.assertNotIn("mesh_result_action_tx", self.report_source)

    def test_gateway_store_is_initialized_then_attached_after_relay_init(self) -> None:
        self.assertIn("sizeof(mesh_gateway_ack_store) == 9544u", self.report_source)
        self.assertIn(
            "sizeof(struct mesh_gateway_ack_store) == 9544u",
            self.relay_header,
        )
        self.assertIn(
            "MESH_RELAY_GATEWAY_ACK_CAPACITY == 200u",
            self.relay_header,
        )
        self.assertIn(
            "MESH_RELAY_GATEWAY_ACK_CLEANUP_RESERVE_CAPACITY 2u",
            self.relay_header,
        )
        self.assertIn(
            "MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY == 202u",
            self.relay_header,
        )
        self.assertRegex(
            self.relay_header,
            r"(?s)struct mesh_gateway_ack_store\s*\{.*?"
            r"candidate_identity_bits\s*\[\s*"
            r"MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES\s*\]\s*;.*?"
            r"confirmed_identity_bits\s*\[\s*"
            r"MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES\s*\]\s*;\s*\}",
        )
        self.assertIn("mesh_gateway_ack_store_init(&mesh_gateway_ack_store)",
                      self.report_source)
        self.assertNotIn("struct mesh_gateway_ack_store", self.anchor_source)

        start = self.anchor_source.index("int app_anchor_start_gateway_role(void)")
        body = self.anchor_source[start:self.anchor_source.index("\n}", start) + 2]
        relay_init = body.index("mesh_relay_init(&mesh_runtime")
        attach = body.index("app_mesh_report_attach_gateway_ack_store()")
        self.assertLess(relay_init, attach)
        self.assertIn("if (ret < 0)", body[attach:])

    def test_gateway_transport_has_no_redundant_full_payload_scratch(self) -> None:
        self.assertRegex(
            self.report_source,
            r"(?s)#if DEVICE_ROLE == ROLE_ANCHOR\s+"
            r"/\*.*?\*/\s+"
            r"static struct mesh_outbound mesh_send_scratch_tx;\s+#endif",
        )
        self.assertNotIn(
            "mesh_deferred_gateway_ack_scratch", self.report_source
        )

        encode = _function_body(
            self.report_source, "mesh_encode_outbound_tx_snapshot"
        )
        self.assertIn("out->payload", encode)
        self.assertIn("mesh_packet_age_at_air_arrival(", encode)
        self.assertIn("frame_airtime_us", encode)
        self.assertNotIn("mesh_outbound_refresh_age", encode)
        self.assertNotIn("memcpy(", encode)

        reliable = _function_body(
            self.report_source, "mesh_try_send_reliable_uplink_view"
        )
        self.assertIn(
            "view->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN",
            reliable,
        )
        self.assertIn("&mesh_route_waiting_tx_scratch", reliable)
        self.assertIn(
            "k_mutex_lock(&mesh_route_wait_scratch_lock, K_NO_WAIT)",
            reliable,
        )
        self.assertIn(
            "k_mutex_unlock(&mesh_route_wait_scratch_lock)", reliable
        )
        self.assertNotIn("static struct mesh_outbound out", reliable)
        lock_at = reliable.index(
            "k_mutex_lock(&mesh_route_wait_scratch_lock, K_NO_WAIT)"
        )
        unlock_at = reliable.index(
            "k_mutex_unlock(&mesh_route_wait_scratch_lock)"
        )
        ownership_at = reliable.index("memset(out, 0, sizeof(*out))")
        self.assertLess(
            reliable.index("mesh_start_tracked_tx_with_retry(", lock_at),
            unlock_at,
        )
        self.assertNotIn("return ", reliable[ownership_at:unlock_at])

        deferred_ack = _function_body(
            self.report_source,
            "mesh_try_deferred_gateway_ack_on_channel9",
        )
        self.assertIn(
            "mesh_send_outbound_with_release_on_channel(", deferred_ack
        )
        self.assertIn("MESH_EVENT_CHANNEL", deferred_ack)
        self.assertNotIn("*ack = *pending", deferred_ack)

        route_wait = _function_body(
            self.report_source, "mesh_try_route_waiting_tx"
        )
        route_lock_at = route_wait.index(
            "k_mutex_lock(&mesh_route_wait_scratch_lock, K_NO_WAIT)"
        )
        copy_at = route_wait.index("*pending = mesh_route_waiting_tx")
        deferred_ack_at = route_wait.index(
            "mesh_try_deferred_gateway_ack_on_channel9("
        )
        route_unlock_at = route_wait.index(
            "k_mutex_unlock(&mesh_route_wait_scratch_lock)"
        )
        self.assertLess(route_lock_at, copy_at)
        self.assertLess(copy_at, deferred_ack_at)
        self.assertLess(deferred_ack_at, route_unlock_at)

    def test_shared_capacity_names_nominal_and_recovery_storage(self) -> None:
        self.assertIn("MESH_CONNECTED_MAX_ANCHORS 50u", self.capacity_header)
        self.assertIn("MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH 9u",
                      self.capacity_header)
        self.assertIn("MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY 1u",
                      self.capacity_header)
        self.assertIn("MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY",
                      self.capacity_header)


if __name__ == "__main__":
    unittest.main()
