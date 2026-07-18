#!/usr/bin/env python3
"""Host-side survey qualification tests without Bluetooth hardware."""

from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import re
import sys
import types
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
SURVEY_ID = 0x778899AA


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


provision = _load(
    "provision_mesh_anchor_test",
    REPO_ROOT / "firmware" / "scripts" / "provision_mesh_anchor.py",
)
from tools.gateway_gui.protocol import encode_cobs_packet, parse_cobs_packet


def command_event(
    stage: int,
    *,
    event_sequence: int,
    command_kind: int = 2,
    command_id: int | None = None,
    host_session_id: int = 0x12345,
    host_sequence: int = 0x2345,
    correlation_id: int = 0x12345,
    anchor_id: int = 0,
    pair: tuple[int, int] = (0, 0),
    attempt: int = 1,
    previous_hop_id: int = 0,
    progress_count: int | None = None,
    total_count: int = 0,
    success_count: int = 0,
    failure_count: int = 0,
    duplicate_count: int = 0,
    lost_event_count: int = 0,
    status: int = 0,
    reason: int = 0,
    hop_count: int = 0,
    discovery_slot: int = 0,
    gateway_sequence: int | None = None,
):
    pair_stage = stage in (9, 10, 11)
    return provision.GatewayCommandEvent(
        command_kind=command_kind,
        stage=stage,
        flags=1 if stage == 12 else 0,
        attempt=attempt,
        command_status=status,
        reason=reason,
        command_id=(
            command_id
            if command_id is not None
            else 0x0102 if pair_stage else 0x0100
        ),
        route_epoch=0,
        correlation_id=correlation_id,
        gateway_sequence=(
            SURVEY_ID if command_kind == 2 else event_sequence
        ) if gateway_sequence is None else gateway_sequence,
        host_session_id=host_session_id,
        host_sequence=host_sequence,
        event_sequence=event_sequence,
        anchor_id=anchor_id,
        pair_initiator_id=pair[0],
        pair_responder_id=pair[1],
        previous_hop_id=previous_hop_id,
        progress_count=success_count if progress_count is None else progress_count,
        total_count=total_count,
        success_count=success_count,
        failure_count=failure_count,
        duplicate_count=duplicate_count,
        lost_event_count=lost_event_count,
        hop_count=hop_count,
        discovery_slot=discovery_slot,
    )


def pair_packet(
    initiator_id: int,
    responder_id: int,
    distance_mm: int,
    sequence: int,
    *,
    survey_id: int = SURVEY_ID,
):
    def tlv(type_id: int, value: bytes) -> bytes:
        return bytes((type_id, len(value))) + value

    payload = b"".join((
        tlv(0x15, survey_id.to_bytes(4, "little")),
        tlv(0x1F, initiator_id.to_bytes(8, "little")),
        tlv(0x20, responder_id.to_bytes(8, "little")),
        tlv(0x0F, (1).to_bytes(2, "little")),
        tlv(0x0E, (0).to_bytes(2, "little")),
        tlv(0x0C, distance_mm.to_bytes(4, "little", signed=True)),
        tlv(0x21, b"\x00"),
    ))
    return parse_cobs_packet(encode_cobs_packet(
        msg_type=0x53,
        flags=0x24,
        src_id=initiator_id,
        dst_id=0x9999888877776666,
        session_id=survey_id,
        seq=sequence,
        ttl=4,
        payload=payload,
    ))


def successful_events() -> list[object]:
    events: list[object] = []
    sequence = 1
    for anchor_id in (0x10, 0x20, 0x30):
        events.append(command_event(6, event_sequence=sequence, anchor_id=anchor_id))
        sequence += 1
    events.append(command_event(8, event_sequence=sequence, total_count=3))
    sequence += 1
    events.append(command_event(5, event_sequence=sequence, status=2, reason=2))
    sequence += 1
    for pair in ((0x10, 0x20), (0x10, 0x30), (0x20, 0x30)):
        events.append(command_event(9, event_sequence=sequence, pair=pair))
        sequence += 1
        events.append(command_event(10, event_sequence=sequence, pair=pair))
        sequence += 1
    events.append(
        command_event(
            12,
            event_sequence=sequence,
            total_count=3,
            success_count=3,
        )
    )
    return events


def successful_route_events(
    *,
    session_id: int = 0x12345,
    retries: int = 2,
    first_sequence: int = 1,
) -> list[object]:
    events: list[object] = []
    sequence = first_sequence
    for attempt in range(1, retries + 1):
        events.append(
            command_event(
                5,
                event_sequence=sequence,
                command_kind=3,
                command_id=0x000C,
                host_session_id=session_id,
                host_sequence=session_id & 0xFFFF,
                correlation_id=session_id,
                attempt=attempt,
                status=2,
                reason=2,
            )
        )
        sequence += 1
    events.append(
        command_event(
            4,
            event_sequence=sequence,
            command_kind=3,
            command_id=0x000C,
            host_session_id=session_id,
            host_sequence=session_id & 0xFFFF,
            correlation_id=session_id,
            attempt=retries + 1,
        )
    )
    sequence += 1
    events.append(
        command_event(
            12,
            event_sequence=sequence,
            command_kind=3,
            command_id=0x000C,
            host_session_id=session_id,
            host_sequence=session_id & 0xFFFF,
            correlation_id=session_id,
        )
    )
    return events


def successful_assignment_events(
    anchor_count: int,
    *,
    session_id: int = 0x12345,
    direct_count: int = 1,
    first_sequence: int = 1,
) -> list[object]:
    events: list[object] = []
    sequence = first_sequence
    events.append(
        command_event(
            4,
            event_sequence=sequence,
            command_kind=1,
            command_id=0x0104,
            host_session_id=session_id,
            host_sequence=session_id & 0xFFFF,
            correlation_id=session_id,
            attempt=1,
        )
    )
    sequence += 1
    for index in range(anchor_count):
        anchor_id = 0x1000 + index
        hop_count = 1 if index < direct_count else 2 + (index % 3)
        previous_hop = anchor_id if hop_count == 1 else 0x2000 + (index % 5)
        events.append(
            command_event(
                6,
                event_sequence=sequence,
                command_kind=1,
                command_id=0x0104,
                host_session_id=session_id,
                host_sequence=session_id & 0xFFFF,
                correlation_id=session_id,
                anchor_id=anchor_id,
                previous_hop_id=previous_hop,
                progress_count=index + 1,
                hop_count=hop_count,
                discovery_slot=0xFF,
            )
        )
        sequence += 1
    for index in range(anchor_count):
        events.append(
            command_event(
                6,
                event_sequence=sequence,
                command_kind=1,
                command_id=0x0104,
                host_session_id=session_id,
                host_sequence=session_id & 0xFFFF,
                correlation_id=session_id,
                anchor_id=0x1000 + index,
                progress_count=index + 1,
                discovery_slot=index,
            )
        )
        sequence += 1
    events.append(
        command_event(
            7,
            event_sequence=sequence,
            command_kind=1,
            command_id=0x0104,
            host_session_id=session_id,
            host_sequence=session_id & 0xFFFF,
            correlation_id=session_id,
            progress_count=anchor_count,
        )
    )
    sequence += 1
    events.append(
        command_event(
            8,
            event_sequence=sequence,
            command_kind=1,
            command_id=0x0104,
            host_session_id=session_id,
            host_sequence=session_id & 0xFFFF,
            correlation_id=session_id,
            attempt=1,
            progress_count=anchor_count,
            total_count=anchor_count,
        )
    )
    sequence += 1
    events.append(
        command_event(
            12,
            event_sequence=sequence,
            command_kind=1,
            command_id=0x0104,
            host_session_id=session_id,
            host_sequence=session_id & 0xFFFF,
            correlation_id=session_id,
            progress_count=anchor_count,
            total_count=anchor_count,
            success_count=anchor_count,
        )
    )
    return events


class SurveyQualificationTests(unittest.TestCase):
    def test_exact_correlated_success_accepts_natural_retry(self) -> None:
        qualification = provision.SurveyQualification(
            0x12345, 0x2345, 0x12345, 0x778899AA, 3, 3
        )
        unrelated = dataclasses.replace(
            command_event(6, event_sequence=1, anchor_id=0x99),
            correlation_id=0xDEADBEEF,
        )
        self.assertFalse(qualification.observe(unrelated))
        for sequence, (pair, distance_mm) in enumerate((
            ((0x10, 0x20), 1000),
            ((0x10, 0x30), 1200),
            ((0x20, 0x30), 1500),
        ), 1):
            qualification.observe_packet(pair_packet(*pair, distance_mm, sequence))
        for event in successful_events():
            qualification.observe(event)
        qualification.validate()
        self.assertEqual(1, qualification.retries)
        self.assertEqual({0x10, 0x20, 0x30}, qualification.anchors)
        self.assertEqual(3, len(qualification.pair_successes))
        self.assertEqual(3, len(qualification.geometry_model.pairs))
        self.assertIsNotNone(qualification.geometry_rmse_m)

    def test_loss_pair_order_and_terminal_counters_are_hard_failures(self) -> None:
        qualification = provision.SurveyQualification(
            0x12345, 0x2345, 0x12345, 0x778899AA, 2, 1
        )
        qualification.observe(
            command_event(
                6, event_sequence=1, anchor_id=0x10, lost_event_count=1
            )
        )
        qualification.observe(command_event(6, event_sequence=2, anchor_id=0x20))
        qualification.observe(command_event(8, event_sequence=3, total_count=1))
        qualification.observe(
            command_event(10, event_sequence=4, pair=(0x10, 0x20))
        )
        qualification.observe(
            command_event(
                12,
                event_sequence=5,
                total_count=1,
                success_count=0,
                failure_count=1,
            )
        )
        with self.assertRaisesRegex(RuntimeError, "lost events"):
            qualification.validate()


class IdentityAllocationTests(unittest.TestCase):
    def test_process_state_survives_wrap_without_zero_or_reuse(self) -> None:
        with mock.patch.object(provision, "_identity_state", 0xFFFFFFFE):
            identities = [provision._new_identity() for _ in range(3)]

        self.assertEqual([0xFFFFFFFF, 1, 2], identities)
        self.assertEqual(len(identities), len(set(identities)))
        self.assertTrue(all(identity & 0xFFFF for identity in identities))

    def test_next_identity_skips_the_previous_command_identity(self) -> None:
        with mock.patch.object(provision, "_identity_state", 0x12344):
            identity = provision._next_identity(previous=0x12345)

        self.assertEqual(0x12346, identity)


class CommandBudgetContractTests(unittest.TestCase):
    def test_cli_max_matches_firmware_and_covers_assignment_default(self) -> None:
        gateway_header = (
            REPO_ROOT / "firmware" / "include" / "gateway_command.h"
        ).read_text(encoding="utf-8")
        anchor_source = (
            REPO_ROOT / "firmware" / "app" / "src" / "app_anchor.c"
        ).read_text(encoding="utf-8")
        maximum = re.search(
            r"#define\s+GATEWAY_COMMAND_BUDGET_MAX_MS\s+(\d+)u\b",
            gateway_header,
        )

        self.assertIsNotNone(maximum)
        assert maximum is not None
        self.assertEqual(
            int(maximum.group(1)), provision.GATEWAY_COMMAND_BUDGET_MAX_MS
        )
        self.assertEqual(600000, provision.GATEWAY_COMMAND_BUDGET_MAX_MS)
        self.assertRegex(
            anchor_source,
            r"BUILD_ASSERT\s*\(\s*"
            r"DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS\s*<=\s*"
            r"GATEWAY_COMMAND_BUDGET_MAX_MS",
        )


class RouteRefreshQualificationTests(unittest.TestCase):
    def test_retries_replay_and_stale_correlation_are_accepted(self) -> None:
        qualification = provision.RouteRefreshQualification(
            0x12345, 0x2345, 0x12345
        )
        stale = dataclasses.replace(
            successful_route_events()[0], correlation_id=0xDEADBEEF
        )
        self.assertFalse(qualification.observe(stale))
        events = successful_route_events(retries=3)
        for event in events:
            qualification.observe(event)
        replay = dataclasses.replace(events[-1], flags=events[-1].flags | 0x06)
        self.assertTrue(qualification.observe(replay))
        qualification.validate()
        self.assertEqual({4}, qualification.flood_attempts)
        self.assertEqual(3, qualification.retries)

    def test_terminal_without_real_local_flood_fails_closed(self) -> None:
        qualification = provision.RouteRefreshQualification(
            0x12345, 0x2345, 0x12345
        )
        qualification.observe(successful_route_events(retries=0)[-1])
        with self.assertRaisesRegex(RuntimeError, "no completed local"):
            qualification.validate()

    def test_loss_and_out_of_budget_attempt_are_hard_failures(self) -> None:
        qualification = provision.RouteRefreshQualification(
            0x12345, 0x2345, 0x12345
        )
        events = successful_route_events(retries=0)
        qualification.observe(
            dataclasses.replace(
                events[0],
                attempt=provision.ROUTE_REFRESH_MAX_LOCAL_ATTEMPTS + 1,
                lost_event_count=1,
            )
        )
        qualification.observe(events[-1])
        with self.assertRaisesRegex(RuntimeError, "lost events"):
            qualification.validate()


class AssignmentQualificationTests(unittest.TestCase):
    def test_exact_success_sweeps_3_20_and_50_with_mixed_hops(self) -> None:
        for anchor_count in (3, 20, 50):
            with self.subTest(anchor_count=anchor_count):
                direct_count = max(1, anchor_count // 3)
                qualification = provision.AssignmentQualification(
                    0x12345,
                    0x2345,
                    0x12345,
                    anchor_count,
                    require_hop_evidence=True,
                    expected_direct_anchors=direct_count,
                    expected_multihop_anchors=anchor_count - direct_count,
                )
                events = successful_assignment_events(
                    anchor_count, direct_count=direct_count
                )
                stale = dataclasses.replace(events[1], correlation_id=0xDEADBEEF)
                self.assertFalse(qualification.observe(stale))
                for event in events:
                    qualification.observe(event)
                replay = dataclasses.replace(events[-1], flags=events[-1].flags | 0x06)
                self.assertTrue(qualification.observe(replay))
                qualification.validate()
                self.assertEqual(anchor_count, len(qualification.anchors))
                self.assertEqual(anchor_count, len(qualification.assigned_slots))
                self.assertEqual(direct_count, qualification.direct_count)
                self.assertEqual(
                    anchor_count - direct_count, qualification.multihop_count
                )

    def test_zero_hop_is_absent_route_evidence_and_cannot_count_as_direct(self) -> None:
        events = successful_assignment_events(3, direct_count=1)
        events[1] = dataclasses.replace(events[1], hop_count=0)
        qualification = provision.AssignmentQualification(
            0x12345,
            0x2345,
            0x12345,
            3,
            require_hop_evidence=True,
            expected_direct_anchors=1,
            expected_multihop_anchors=2,
        )
        for event in events:
            qualification.observe(event)

        with self.assertRaisesRegex(RuntimeError, "expected hop-path evidence"):
            qualification.validate()
        self.assertEqual(0, qualification.direct_count)
        self.assertEqual(2, qualification.multihop_count)

    def test_duplicate_claims_are_deduplicated_but_conflicting_slots_fail(self) -> None:
        events = successful_assignment_events(3, direct_count=1)
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        qualification.observe(events[0])
        qualification.observe(events[1])
        qualification.observe(dataclasses.replace(events[1], event_sequence=1000))
        for event in events[2:]:
            qualification.observe(event)
        qualification.validate()

        conflicting = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        for event in events[:-1]:
            conflicting.observe(event)
        conflicting.observe(
            dataclasses.replace(
                events[4], event_sequence=1001, discovery_slot=2
            )
        )
        conflicting.observe(events[-1])
        with self.assertRaisesRegex(RuntimeError, "changed assigned slot"):
            conflicting.validate()

    def test_missing_table_ack_terminal_counters_fail(self) -> None:
        events = successful_assignment_events(3)
        events[-1] = dataclasses.replace(
            events[-1], success_count=2, failure_count=1
        )
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        for event in events:
            qualification.observe(event)
        with self.assertRaisesRegex(RuntimeError, "every table ACK"):
            qualification.validate()

    def test_assignment_retry_progress_does_not_weaken_exact_success(self) -> None:
        events = successful_assignment_events(3)
        retry = dataclasses.replace(
            events[0],
            stage=5,
            event_sequence=500,
            attempt=2,
            command_status=2,
            reason=2,
        )
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        qualification.observe(events[0])
        qualification.observe(retry)
        for event in events[1:]:
            qualification.observe(event)
        qualification.validate()
        self.assertEqual(1, qualification.retries)

    def test_loss_missing_mapping_and_stale_terminal_cannot_pass(self) -> None:
        events = successful_assignment_events(3)
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        for event in events:
            if event.stage == 6 and event.anchor_id == 0x1002 and event.discovery_slot != 0xFF:
                continue
            qualification.observe(event)
        qualification.observe(
            dataclasses.replace(
                events[-1],
                event_sequence=2000,
                correlation_id=0xDEADBEEF,
                lost_event_count=1,
            )
        )
        with self.assertRaisesRegex(RuntimeError, "published slot mappings"):
            qualification.validate()

        lost = provision.AssignmentQualification(0x12345, 0x2345, 0x12345, 3)
        for index, event in enumerate(events):
            lost.observe(
                dataclasses.replace(event, lost_event_count=1)
                if index == 1
                else event
            )
        with self.assertRaisesRegex(RuntimeError, "lost events"):
            lost.validate()


class FakeCharacteristic:
    max_write_without_response_size = 244


class FakeServices:
    def get_characteristic(self, _uuid: object) -> FakeCharacteristic:
        return FakeCharacteristic()


class FakeDecoder:
    events: list[object] = []
    packets: list[object] = []
    errors: list[str] = []

    def __init__(self) -> None:
        self.index = 0

    def feed(self, _raw: bytes) -> object:
        errors = list(self.errors) if self.index == 0 else []
        packets = []
        if self.index < len(self.packets):
            packets.append(self.packets[self.index])
        elif self.index < len(self.events):
            packets.append(
                types.SimpleNamespace(
                    msg_type=provision.MSG_GATEWAY_COMMAND_EVENT,
                    src_id=1,
                    dst_id=1,
                    session_id=self.index + 1,
                    seq=self.index + 1,
                    payload=bytes([self.index]),
                )
            )
        self.index += 1
        return types.SimpleNamespace(errors=errors, packets=packets)


class FakeBleakClient:
    operations: list[tuple[str, object]] = []
    notification_count = 0
    write_notification_counts: list[int] = []

    def __init__(
        self,
        gateway: str,
        timeout: float,
        disconnected_callback: object | None = None,
    ) -> None:
        self.gateway = gateway
        self.timeout = timeout
        self.disconnected_callback = disconnected_callback
        self.services = FakeServices()
        self.notify_enabled = False
        self.notify_callback: object | None = None

    async def __aenter__(self):
        self.operations.append(("connect", self.gateway))
        return self

    async def __aexit__(self, *_args: object) -> None:
        self.operations.append(("disconnect", self.gateway))

    async def read_gatt_char(self, _uuid: object) -> bytes:
        self.operations.append(("read_identity", None))
        return (0xAABBCCDDEEFF0011).to_bytes(8, "little")

    async def write_gatt_char(
        self, _characteristic: object, data: bytes, *, response: bool
    ) -> None:
        assert not response
        self.operations.append(("write", (bytes(data), self.notify_enabled)))
        if self.notify_enabled and self.write_notification_counts:
            count = self.write_notification_counts.pop(0)
            assert self.notify_callback is not None
            for _ in range(count):
                self.notify_callback(None, bytearray(b"notification"))

    async def start_notify(self, _uuid: object, callback: object) -> None:
        self.notify_enabled = True
        self.notify_callback = callback
        self.operations.append(("start_notify", None))
        for _ in range(self.notification_count):
            callback(None, bytearray(b"notification"))


def args(**overrides: object) -> argparse.Namespace:
    values = {
        "gateway": "AA:BB:CC:DD:EE:FF",
        "command": "survey",
        "host_id": 1,
        "duration": 1.0,
        "connect_timeout": 12.0,
        "repeat": 1,
        "interval": 0.05,
        "survey_id": 0x778899AA,
        "survey_duration_ms": 1000,
        "discovery_slots": 6,
        "samples": 3,
        "notification_hold_s": 0.25,
        "require_survey_success": True,
        "require_assignment_success": False,
        "expected_anchors": 3,
        "expected_pairs": 3,
        "expected_direct_anchors": None,
        "expected_multihop_anchors": None,
        "route_refresh_timeout": 1.0,
        "assignment_timeout": 1.0,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class ProvisionRunTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        FakeBleakClient.operations = []
        FakeDecoder.events = []
        FakeDecoder.packets = []
        FakeDecoder.errors = []
        FakeBleakClient.notification_count = 0
        FakeBleakClient.write_notification_counts = []

    async def test_qualification_writes_then_holds_then_enables_and_drains(self) -> None:
        events = successful_events()
        FakeDecoder.events = events
        FakeDecoder.packets = [
            pair_packet(0x10, 0x20, 1000, 1),
            pair_packet(0x10, 0x30, 1200, 2),
            pair_packet(0x20, 0x30, 1500, 3),
        ] + [
            types.SimpleNamespace(
                msg_type=provision.MSG_GATEWAY_COMMAND_EVENT,
                src_id=1,
                dst_id=1,
                session_id=index + 1,
                seq=index + 1,
                payload=bytes([index]),
            )
            for index in range(len(events))
        ]
        FakeBleakClient.notification_count = len(FakeDecoder.packets)
        sleeps: list[float] = []
        command_args: dict[str, object] = {}
        real_build_anchor_discovery_command = (
            provision.build_anchor_discovery_command
        )

        async def fake_sleep(delay: float) -> None:
            sleeps.append(delay)
            FakeBleakClient.operations.append(("sleep", delay))

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        def capture_command(**kwargs: object) -> object:
            command_args.update(kwargs)
            return real_build_anchor_discovery_command(**kwargs)

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "decode_gateway_command_event", fake_decode),
            mock.patch.object(
                provision,
                "build_anchor_discovery_command",
                side_effect=capture_command,
            ),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch.object(provision.asyncio, "sleep", side_effect=fake_sleep),
            mock.patch("builtins.print"),
        ):
            qualification = await provision.run(args())

        operation_names = [name for name, _value in FakeBleakClient.operations]
        write_index = operation_names.index("write")
        sleep_index = operation_names.index("sleep")
        notify_index = operation_names.index("start_notify")
        self.assertLess(write_index, sleep_index)
        self.assertLess(sleep_index, notify_index)
        self.assertEqual([0.25], sleeps)
        self.assertFalse(FakeBleakClient.operations[write_index][1][1])
        self.assertIsNotNone(qualification)
        assert qualification is not None
        self.assertEqual(0x12345, qualification.correlation_id)
        self.assertEqual(0x778899AA, qualification.survey_id)
        self.assertEqual(0x778899AA, command_args["survey_id"])
        self.assertEqual(0x12345, command_args["session_id"])
        self.assertEqual(3, len(qualification.pair_successes))

    async def test_default_mode_keeps_notify_before_command_write(self) -> None:
        async def fake_sleep(_delay: float) -> None:
            return None

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch.object(provision.asyncio, "sleep", side_effect=fake_sleep),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(require_survey_success=False, notification_hold_s=0.0)
            )

        operation_names = [name for name, _value in FakeBleakClient.operations]
        self.assertLess(
            operation_names.index("start_notify"), operation_names.index("write")
        )
        write = next(value for name, value in FakeBleakClient.operations if name == "write")
        self.assertTrue(write[1])

    async def test_decode_error_fails_immediately_after_notifications_enable(self) -> None:
        FakeDecoder.errors = ["bad stream CRC"]
        FakeBleakClient.notification_count = 1

        async def fake_sleep(_delay: float) -> None:
            return None

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch.object(provision.asyncio, "sleep", side_effect=fake_sleep),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "BLE decode errors"):
                await provision.run(args())

    async def test_missing_terminal_times_out(self) -> None:
        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                await provision.run(args(duration=0.001, notification_hold_s=0.0))

    async def test_combined_reachability_waits_for_local_flood_then_assignment(self) -> None:
        route_events = successful_route_events(
            session_id=0x11111, retries=1, first_sequence=1
        )
        assignment_events = successful_assignment_events(
            3,
            session_id=0x22222,
            direct_count=1,
            first_sequence=100,
        )
        events = route_events + assignment_events
        FakeDecoder.events = events
        FakeBleakClient.notification_count = len(route_events)
        FakeBleakClient.write_notification_counts = [len(assignment_events)]

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "decode_gateway_command_event", fake_decode),
            mock.patch.object(
                provision, "_new_identity", side_effect=[0x11111, 0x22222]
            ),
            mock.patch("builtins.print"),
        ):
            qualification = await provision.run(
                args(
                    command="qualify-reachability",
                    require_survey_success=False,
                    notification_hold_s=0.0,
                    expected_direct_anchors=1,
                    expected_multihop_anchors=2,
                )
            )

        writes = [value for name, value in FakeBleakClient.operations if name == "write"]
        self.assertEqual(2, len(writes))
        self.assertFalse(writes[0][1])
        self.assertTrue(writes[1][1])
        self.assertIsInstance(qualification, provision.AssignmentQualification)
        assert isinstance(qualification, provision.AssignmentQualification)
        self.assertEqual(3, len(qualification.anchors))

    async def test_strict_assignment_disconnect_fails_immediately(self) -> None:
        class DisconnectingClient(FakeBleakClient):
            async def start_notify(self, uuid: object, callback: object) -> None:
                await super().start_notify(uuid, callback)
                assert self.disconnected_callback is not None
                self.disconnected_callback(self)

        with (
            mock.patch.object(provision, "BleakClient", DisconnectingClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "disconnected"):
                await provision.run(
                    args(
                        command="assign-slots",
                        require_survey_success=False,
                        require_assignment_success=True,
                        notification_hold_s=0.0,
                    )
                )


if __name__ == "__main__":
    unittest.main()
