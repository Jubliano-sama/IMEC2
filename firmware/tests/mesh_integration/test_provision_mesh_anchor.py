#!/usr/bin/env python3
"""Host-side enumeration and route qualification tests without BLE hardware."""

from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "firmware" / "scripts" / "provision_mesh_anchor.py"
SPEC = importlib.util.spec_from_file_location(
    "provision_mesh_anchor_test", MODULE_PATH
)
assert SPEC is not None and SPEC.loader is not None
provision = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = provision
SPEC.loader.exec_module(provision)


HOST_SESSION = 0x12345
HOST_SEQUENCE = 0x2345
GATEWAY_ID = 0xAABBCCDDEEFF0011
ANCHOR_A = 0x1000
ANCHOR_B = 0x1001
ANCHOR_C = 0x1002


def command_event(
    stage: int,
    sequence: int,
    *,
    kind: int = provision.GATEWAY_COMMAND_KIND_ANCHOR_ENUMERATION,
    command_id: int = provision.CMD_ASSIGN_DISCOVERY_SLOTS,
    flags: int = 0,
    attempt: int = 1,
    anchor_id: int = 0,
    previous_hop_id: int = 0,
    progress_count: int = 0,
    total_count: int = 0,
    success_count: int = 0,
    failure_count: int = 0,
    hop_count: int = 0,
    discovery_slot: int = provision.DISCOVERY_SLOT_UNAVAILABLE,
    status: int = 0,
    reason: int = 0,
    lost_event_count: int = 0,
) -> object:
    return provision.GatewayCommandEvent(
        command_kind=kind,
        stage=stage,
        flags=flags,
        attempt=attempt,
        command_status=status,
        reason=reason,
        command_id=command_id,
        route_epoch=0,
        correlation_id=HOST_SESSION,
        gateway_sequence=sequence,
        host_session_id=HOST_SESSION,
        host_sequence=HOST_SEQUENCE,
        event_sequence=sequence,
        anchor_id=anchor_id,
        pair_initiator_id=0,
        pair_responder_id=0,
        previous_hop_id=previous_hop_id,
        progress_count=progress_count,
        total_count=total_count,
        success_count=success_count,
        failure_count=failure_count,
        duplicate_count=0,
        lost_event_count=lost_event_count,
        hop_count=hop_count,
        discovery_slot=discovery_slot,
    )


def successful_assignment_events() -> list[object]:
    events = [
        command_event(
            provision.GATEWAY_COMMAND_STAGE_FLOOD_ATTEMPT,
            1,
        )
    ]
    paths = (
        (ANCHOR_A, GATEWAY_ID, 1, 0),
        (ANCHOR_B, GATEWAY_ID, 1, 1),
        (ANCHOR_C, ANCHOR_A, 2, 2),
    )
    for index, (anchor, previous, hops, slot) in enumerate(paths, start=2):
        events.append(
            command_event(
                provision.GATEWAY_COMMAND_STAGE_ANCHOR_REPORT,
                index,
                anchor_id=anchor,
                previous_hop_id=previous,
                hop_count=hops,
                discovery_slot=slot,
            )
        )
    events.append(
        command_event(
            provision.GATEWAY_COMMAND_STAGE_ENUMERATION_COMPLETE,
            5,
            progress_count=3,
            total_count=3,
        )
    )
    events.append(
        command_event(
            provision.GATEWAY_COMMAND_STAGE_SCHEDULE_READY,
            6,
            progress_count=3,
            total_count=3,
        )
    )
    for index, (anchor, _previous, hops, slot) in enumerate(paths, start=7):
        events.append(
            command_event(
                provision.GATEWAY_COMMAND_STAGE_SCHEDULE_READY,
                index,
                anchor_id=anchor,
                progress_count=index - 6,
                total_count=3,
                hop_count=hops,
                discovery_slot=slot,
            )
        )
    events.append(
        command_event(
            provision.GATEWAY_COMMAND_STAGE_TERMINAL,
            10,
            flags=1,
            progress_count=3,
            total_count=3,
            success_count=3,
        )
    )
    return events


class AssignmentQualificationTests(unittest.TestCase):
    def test_f1dd_topology_is_losslessly_qualified(self) -> None:
        qualification = provision.AssignmentQualification(
            HOST_SESSION,
            HOST_SEQUENCE,
            HOST_SESSION,
            3,
            require_hop_evidence=True,
            expected_direct_anchors=2,
            expected_multihop_anchors=1,
            expected_anchor_hops={ANCHOR_A: 1, ANCHOR_B: 1, ANCHOR_C: 2},
        )
        for event in successful_assignment_events():
            qualification.observe(event)
        qualification.validate()

        self.assertEqual(2, qualification.direct_count)
        self.assertEqual(1, qualification.multihop_count)
        self.assertEqual(
            {ANCHOR_A: 0, ANCHOR_B: 1, ANCHOR_C: 2},
            qualification.assigned_slots,
        )

    def test_wrong_forced_anchor_depth_fails_closed(self) -> None:
        qualification = provision.AssignmentQualification(
            HOST_SESSION,
            HOST_SEQUENCE,
            HOST_SESSION,
            3,
            require_hop_evidence=True,
            expected_anchor_hops={ANCHOR_A: 1, ANCHOR_B: 1, ANCHOR_C: 1},
        )
        for event in successful_assignment_events():
            qualification.observe(event)
        with self.assertRaisesRegex(RuntimeError, "expected hop count 1, got 2"):
            qualification.validate()

    def test_changed_replay_and_new_loss_are_hard_failures(self) -> None:
        events = successful_assignment_events()
        qualification = provision.AssignmentQualification(
            HOST_SESSION, HOST_SEQUENCE, HOST_SESSION, 3
        )
        qualification.observe(events[0])
        qualification.observe(dataclasses.replace(events[0], attempt=2))
        for event in events[1:]:
            qualification.observe(
                dataclasses.replace(event, lost_event_count=1)
            )
        with self.assertRaisesRegex(RuntimeError, "event sequence|lost events"):
            qualification.validate()


class RouteRefreshQualificationTests(unittest.TestCase):
    def test_local_flood_and_terminal_are_required(self) -> None:
        qualification = provision.RouteRefreshQualification(
            HOST_SESSION, HOST_SEQUENCE, HOST_SESSION
        )
        qualification.observe(
            command_event(
                provision.GATEWAY_COMMAND_STAGE_FLOOD_ATTEMPT,
                1,
                kind=provision.GATEWAY_COMMAND_KIND_ROUTE_REFRESH,
                command_id=provision.CMD_FORCE_REDISCOVERY,
            )
        )
        self.assertTrue(
            qualification.observe(
                command_event(
                    provision.GATEWAY_COMMAND_STAGE_TERMINAL,
                    2,
                    kind=provision.GATEWAY_COMMAND_KIND_ROUTE_REFRESH,
                    command_id=provision.CMD_FORCE_REDISCOVERY,
                    flags=1,
                )
            )
        )
        qualification.validate()

        missing = provision.RouteRefreshQualification(
            HOST_SESSION, HOST_SEQUENCE, HOST_SESSION
        )
        missing.observe(
            command_event(
                provision.GATEWAY_COMMAND_STAGE_TERMINAL,
                3,
                kind=provision.GATEWAY_COMMAND_KIND_ROUTE_REFRESH,
                command_id=provision.CMD_FORCE_REDISCOVERY,
                flags=1,
            )
        )
        with self.assertRaisesRegex(RuntimeError, "no completed local"):
            missing.validate()


class ProvisioningHelpersTests(unittest.TestCase):
    def test_production_assignment_is_durable_unless_explicitly_ram_only(self) -> None:
        captured: list[argparse.Namespace] = []

        async def capture(args: argparse.Namespace) -> None:
            captured.append(args)

        base_args = [
            "provision_mesh_anchor.py",
            "--gateway",
            "test",
            "--command",
            "assign-slots",
        ]
        with mock.patch.object(provision, "run", side_effect=capture):
            with mock.patch.object(sys, "argv", base_args):
                provision.main()
            with mock.patch.object(
                sys, "argv", [*base_args, "--ram-only-assignment"]
            ):
                provision.main()

        self.assertFalse(captured[0].ram_only_assignment)
        self.assertTrue(captured[1].ram_only_assignment)

    def test_monitor_sigint_stops_cleanly_without_hiding_other_commands(self) -> None:
        def interrupt(coroutine: object) -> None:
            coroutine.close()
            raise KeyboardInterrupt

        for command, should_raise in (("monitor", False), ("here-i-am", True)):
            with self.subTest(command=command), mock.patch.object(
                sys, "argv", ["provision_mesh_anchor.py", "--gateway", "test", "--command", command]
            ), mock.patch.object(
                provision.asyncio, "run", side_effect=interrupt
            ), mock.patch("builtins.print") as print_mock:
                if should_raise:
                    with self.assertRaises(KeyboardInterrupt):
                        provision.main()
                    print_mock.assert_not_called()
                else:
                    provision.main()
                    print_mock.assert_called_once_with("BLE_MONITOR_STOPPED")

    def test_assignment_policy_uses_the_exact_depth_aware_budget(self) -> None:
        policy = provision._assignment_operation_policy(3, None, deepest_hop=2)
        expected = provision.assignment_required_budget_ms(
            provision.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            3,
            deepest_hop=2,
        )
        self.assertEqual(expected, policy.assignment.operation_budget_ms)
        self.assertEqual(3, policy.assignment.expected_anchor_count)
        self.assertEqual(2, policy.assignment.deepest_hop)

    def test_identity_allocator_never_returns_zero_or_reuses_previous(self) -> None:
        previous = provision._identity_state
        for _ in range(1000):
            identity = provision._next_identity(previous)
            self.assertNotEqual(0, identity)
            self.assertNotEqual(previous, identity)
            self.assertNotEqual(0, identity & 0xFFFF)
            previous = identity

    def test_expected_anchor_hop_parser_enforces_protocol_bounds(self) -> None:
        self.assertEqual(
            (ANCHOR_C, 2), provision._parse_expected_anchor_hop("0x1002=2")
        )
        for invalid in ("broken", "0=1", "1=0", "1=9"):
            with self.subTest(invalid=invalid), self.assertRaises(
                argparse.ArgumentTypeError
            ):
                provision._parse_expected_anchor_hop(invalid)

    def test_host_timeout_covers_firmware_budget_and_delivery_guard(self) -> None:
        timeout = provision._qualification_timeout_s(1.0, 120000)
        self.assertGreaterEqual(
            timeout,
            120.0 + provision.GATEWAY_COMMAND_COMPLETION_GUARD_S,
        )


if __name__ == "__main__":
    unittest.main()
