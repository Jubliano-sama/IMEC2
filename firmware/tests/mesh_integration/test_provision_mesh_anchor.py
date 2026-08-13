#!/usr/bin/env python3
"""Host-side survey qualification tests without Bluetooth hardware."""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
import importlib.util
import re
import struct
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
from tools.gateway_gui.protocol import (
    FLAG_COUNT_AS_CLICK,
    FLAG_DIAGNOSTIC,
    FLAG_GATEWAY_ACK_REQUIRED,
    MSG_ANCHOR_HEARTBEAT,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_RESULT_BUNDLE,
    MSG_SURVEY_DISCOVERY_REPORT,
    MSG_SURVEY_PAIR_RESULT,
    Packet,
    build_gateway_host_receipt,
    encode_cobs_packet,
    parse_cobs_packet,
)
from tools.gateway_gui import operation_policy as host_operation_policy
from tools.gateway_gui import protocol as host_protocol


GATEWAY_ID = 0xAABBCCDDEEFF0011


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


def gateway_command_event_payload(event: object) -> bytes:
    return struct.pack(
        "<BBBBBBBBHHIIIH2xIQQQQHHHHHHBB",
        1,
        78,
        event.command_kind,
        event.stage,
        event.flags,
        event.attempt,
        event.command_status,
        event.reason,
        event.command_id,
        event.route_epoch,
        event.correlation_id,
        event.gateway_sequence,
        event.host_session_id,
        event.host_sequence,
        event.event_sequence,
        event.anchor_id,
        event.pair_initiator_id,
        event.pair_responder_id,
        event.previous_hop_id,
        event.progress_count,
        event.total_count,
        event.success_count,
        event.failure_count,
        event.duplicate_count,
        event.lost_event_count,
        event.hop_count,
        event.discovery_slot,
    )


def pair_packet(
    initiator_id: int,
    responder_id: int,
    distance_mm: int,
    sequence: int,
    *,
    survey_id: int = SURVEY_ID,
    operation_generation: int | None = None,
    round_id: int = 1,
):
    def tlv(type_id: int, value: bytes) -> bytes:
        return bytes((type_id, len(value))) + value

    if operation_generation is None:
        operation_generation = (1 << 32) | survey_id
    round_commitment = round_id.to_bytes(2, "little") + bytes(30)
    payload = b"".join((
        tlv(0x15, survey_id.to_bytes(4, "little")),
        tlv(0xB6, operation_generation.to_bytes(8, "little")),
        tlv(0x1F, initiator_id.to_bytes(8, "little")),
        tlv(0x20, responder_id.to_bytes(8, "little")),
        tlv(0xAF, round_id.to_bytes(2, "little")),
        tlv(0xB7, round_commitment),
        tlv(0x0F, (1).to_bytes(2, "little")),
        tlv(0x0E, (0).to_bytes(2, "little")),
        tlv(0x0C, distance_mm.to_bytes(4, "little", signed=True)),
        tlv(0x21, b"\x00"),
    ))
    return parse_cobs_packet(encode_cobs_packet(
        msg_type=0x53,
        flags=0x24,
        src_id=responder_id,
        dst_id=0x9999888877776666,
        session_id=survey_id,
        seq=sequence,
        ttl=4,
        payload=payload,
    ))


def gateway_delivery_packet(
    sequence: int,
    *,
    transport: str = "gateway-stream-v1",
    msg_type: int = MSG_COMMAND_RESULT,
) -> Packet:
    return Packet(
        transport=transport,
        raw_transport=f"gateway-record-{sequence}".encode(),
        raw_packet=None,
        msg_type=msg_type,
        flags=FLAG_GATEWAY_ACK_REQUIRED,
        src_id=0x1020304050607080,
        dst_id=GATEWAY_ID,
        session_id=0x11223344,
        seq=sequence,
        ttl=None,
        age_ms=10,
        age_kind="gateway_queue_age_ms",
        payload=f"payload-{sequence}".encode(),
        tlvs=(),
    )


def gateway_stream_pair_packet(
    initiator_id: int,
    responder_id: int,
    distance_mm: int,
    sequence: int,
    *,
    survey_id: int = SURVEY_ID,
) -> Packet:
    packet = pair_packet(
        initiator_id,
        responder_id,
        distance_mm,
        sequence,
        survey_id=survey_id,
    )
    return dataclasses.replace(
        packet,
        transport="gateway-stream-v1",
        raw_transport=f"gateway-survey-pair-{sequence}".encode(),
    )


def gateway_stream_packet(
    sequence: int,
    *,
    msg_type: int,
    payload: bytes = b"\x05\x00",
    flags: int = FLAG_GATEWAY_ACK_REQUIRED,
    src_id: int = 0x1020304050607080,
    dst_id: int = GATEWAY_ID,
    session_id: int = 0x11223344,
) -> Packet:
    packet = parse_cobs_packet(encode_cobs_packet(
        msg_type=msg_type,
        flags=flags,
        src_id=src_id,
        dst_id=dst_id,
        session_id=session_id,
        seq=sequence,
        ttl=4,
        payload=payload,
    ))
    return dataclasses.replace(
        packet,
        transport="gateway-stream-v1",
        raw_transport=f"gateway-survey-record-{sequence}".encode(),
    )


def gateway_stream_click_packet(
    sequence: int,
    *,
    anchor_id: int = 0x1020304050607080,
    clicker_id: int = 0x8877665544332211,
    event_seq: int = 0x10203040,
) -> Packet:
    payload = bytearray()
    for type_id, value in (
        (host_protocol.TLV_CLICKER_ID, clicker_id.to_bytes(8, "little")),
        (host_protocol.TLV_ANCHOR_ID, anchor_id.to_bytes(8, "little")),
        (host_protocol.TLV_EVENT_SEQ, event_seq.to_bytes(4, "little")),
        (host_protocol.TLV_TIMESTAMP_MS, (123456).to_bytes(8, "little")),
        (host_protocol.TLV_DISTANCE_MM, (2345).to_bytes(4, "little")),
        (host_protocol.TLV_QUALITY, bytes((100,))),
        (host_protocol.TLV_RANGE_STATUS, bytes((0,))),
        (host_protocol.TLV_SAMPLE_COUNT, (1).to_bytes(2, "little")),
        (host_protocol.TLV_DISTANCE_SAMPLES_MM, (2345).to_bytes(4, "little")),
        (host_protocol.TLV_RANGE_ROUND_INDICES, bytes((0,))),
        (
            host_protocol.TLV_SEQUENCE_START_TIMESTAMPS_MS,
            (123456).to_bytes(8, "little"),
        ),
        (host_protocol.TLV_ATTEMPT_INDEX, bytes((1,))),
        (
            host_protocol.TLV_DETECTION_SOURCE,
            bytes((host_protocol.DETECTION_SOURCE_UWB_WAKE_CLAIM,)),
        ),
        (host_protocol.TLV_BURST_ID, (1).to_bytes(4, "little")),
    ):
        host_protocol.append_tlv(payload, type_id, value)
    packet = gateway_stream_packet(
        sequence,
        msg_type=MSG_CLICK_REPORT,
        payload=bytes(payload),
        flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        src_id=anchor_id,
        session_id=host_protocol.click_report_session_id(clicker_id, event_seq),
    )
    host_protocol.validate_click_payload(packet)
    return packet


def gateway_stream_command_event_packet(
    event: object,
    *,
    flags: int,
) -> Packet:
    return Packet(
        transport="gateway-stream-v1",
        raw_transport=(
            f"gateway-command-event-{event.event_sequence}-{flags}".encode()
        ),
        raw_packet=None,
        msg_type=MSG_GATEWAY_COMMAND_EVENT,
        flags=flags,
        src_id=GATEWAY_ID,
        dst_id=GATEWAY_ID,
        session_id=event.event_sequence,
        seq=event.event_sequence & 0xFFFF,
        ttl=None,
        age_ms=10,
        age_kind="gateway_queue_age_ms",
        payload=gateway_command_event_payload(event),
        tlvs=(),
    )


def gateway_stream_discovery_packet(
    sequence: int,
    *,
    anchor_id: int = 0x1020304050607080,
    survey_id: int = SURVEY_ID,
    operation_generation: int = 0x1234567887654321,
    boot_incarnation: int = 0x55667788,
) -> Packet:
    payload = bytearray()
    host_protocol.append_tlv(
        payload, host_protocol.TLV_SURVEY_ID, survey_id.to_bytes(4, "little")
    )
    host_protocol.append_tlv(
        payload, host_protocol.TLV_ANCHOR_ID, anchor_id.to_bytes(8, "little")
    )
    host_protocol.append_tlv(
        payload,
        host_protocol.TLV_REACHABILITY_ENTRY,
        0x1111222233334444.to_bytes(8, "little") + b"\xc3\x52",
    )
    host_protocol.append_tlv(
        payload,
        host_protocol.TLV_SURVEY_OPERATION_GENERATION,
        operation_generation.to_bytes(8, "little"),
    )
    host_protocol.append_tlv(
        payload,
        host_protocol.TLV_NODE_BOOT_COUNTER,
        boot_incarnation.to_bytes(4, "little"),
    )
    host_protocol.append_tlv(
        payload, host_protocol.TLV_COMMAND_STATUS, (0).to_bytes(2, "little")
    )
    return gateway_stream_packet(
        sequence,
        msg_type=MSG_SURVEY_DISCOVERY_REPORT,
        payload=bytes(payload),
        flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        src_id=anchor_id,
        session_id=boot_incarnation,
    )


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
        previous_hop = (
            anchor_id
            if hop_count == 1
            else 0x1000 + (index % max(1, direct_count))
        )
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
        hop_count = 1 if index < direct_count else 2 + (index % 3)
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
                hop_count=hop_count,
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
    @staticmethod
    def _literal_macro(source: str, name: str) -> int:
        match = re.search(
            rf"#define\s+{re.escape(name)}\s+(\d+)u\b", source
        )
        if match is None:
            raise AssertionError(f"missing literal macro {name}")
        return int(match.group(1))

    def test_every_host_command_budget_mirror_matches_firmware(self) -> None:
        gateway_header = (
            REPO_ROOT / "firmware" / "include" / "gateway_command.h"
        ).read_text(encoding="utf-8")
        operation_header = (
            REPO_ROOT / "firmware" / "include" / "operation_policy.h"
        ).read_text(encoding="utf-8")
        survey_header = (
            REPO_ROOT / "firmware" / "include" / "survey.h"
        ).read_text(encoding="utf-8")
        node_comm_header = (
            REPO_ROOT / "firmware" / "app" / "src" / "app_node_comm.h"
        ).read_text(encoding="utf-8")

        firmware_min = self._literal_macro(
            gateway_header, "GATEWAY_COMMAND_BUDGET_MIN_MS"
        )
        firmware_max = self._literal_macro(
            gateway_header, "GATEWAY_COMMAND_BUDGET_MAX_MS"
        )
        assignment_default = self._literal_macro(
            operation_header,
            "OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS",
        )
        operation_max = self._literal_macro(
            operation_header, "OPERATION_POLICY_COMMAND_BUDGET_MAX_MS"
        )
        discovery_default = self._literal_macro(
            operation_header,
            "OPERATION_POLICY_DISCOVERY_DEFAULT_BUDGET_MS",
        )
        survey_default = self._literal_macro(
            survey_header, "SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS"
        )
        route_refresh_default = self._literal_macro(
            node_comm_header,
            "APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS",
        )
        sample_count = self._literal_macro(
            survey_header, "SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT"
        )

        self.assertEqual(firmware_min, host_protocol.GATEWAY_COMMAND_BUDGET_MIN_MS)
        self.assertEqual(firmware_max, host_protocol.GATEWAY_COMMAND_BUDGET_MAX_MS)
        self.assertEqual(firmware_min, host_operation_policy.COMMAND_BUDGET_MIN_MS)
        self.assertEqual(
            operation_max, host_operation_policy.COMMAND_BUDGET_MAX_MS
        )
        self.assertEqual(
            assignment_default,
            host_protocol.DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
        )
        self.assertEqual(
            assignment_default, host_operation_policy.ASSIGNMENT_DEFAULT_BUDGET_MS
        )
        self.assertEqual(
            survey_default,
            host_protocol.SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS,
        )
        self.assertEqual(
            discovery_default,
            host_operation_policy.DISCOVERY_DEFAULT_BUDGET_MS,
        )
        self.assertEqual(3_600_000, survey_default)
        self.assertEqual(900_000, discovery_default)
        self.assertGreater(survey_default, discovery_default)
        self.assertEqual(
            route_refresh_default,
            host_protocol.ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS,
        )
        self.assertEqual(sample_count, host_protocol.SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT)
        self.assertEqual(
            assignment_default,
            provision.DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
        )
        self.assertEqual(survey_default, provision.SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS)
        self.assertEqual(
            route_refresh_default,
            provision.ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS,
        )

    def test_discovery_budget_mirror_covers_maximum_firmware_route_depth(self) -> None:
        survey_header = (
            REPO_ROOT / "firmware" / "include" / "survey.h"
        ).read_text(encoding="utf-8")
        report_capacity = self._literal_macro(
            survey_header, "SURVEY_GATEWAY_MAX_REPORTS"
        )
        service_ms = self._literal_macro(
            survey_header, "SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS"
        )
        flow_guard_ms = self._literal_macro(
            survey_header, "SURVEY_DISCOVERY_REPORT_FLOW_CONTROL_GUARD_MS"
        )
        per_hop_ms = self._literal_macro(
            survey_header,
            "SURVEY_DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS",
        )
        max_hops = self._literal_macro(survey_header, "SURVEY_DEFAULT_TTL")
        direct_ms = report_capacity * service_ms + flow_guard_ms
        firmware_max_ms = direct_ms + (max_hops - 1) * per_hop_ms

        self.assertEqual(42_000, firmware_max_ms)
        self.assertEqual(
            firmware_max_ms,
            host_operation_policy.DISCOVERY_REPORT_CUSTODY_MAX_MS,
        )

    def test_cli_rejects_survey_budget_below_selected_discovery_horizon(
        self,
    ) -> None:
        required_ms = host_operation_policy.discovery_required_budget_ms(
            host_operation_policy.DISCOVERY_DEFAULT_START_DELAY_MS,
            host_operation_policy.DISCOVERY_DEFAULT_SLOT_MS,
            6,
            host_operation_policy.DISCOVERY_DEFAULT_ROUND_COUNT,
            1_000,
        )
        self.assertEqual(210_743, required_ms)

        argv = [
            "provision_mesh_anchor.py",
            "--gateway",
            "test-gateway",
            "--command",
            "survey",
            "--command-budget-ms",
            str(required_ms - 1),
        ]
        with (
            mock.patch.object(sys, "argv", argv),
            mock.patch.object(sys, "stderr", mock.MagicMock()),
            mock.patch.object(provision.asyncio, "run") as async_run,
            self.assertRaises(SystemExit),
        ):
            provision.main()
        async_run.assert_not_called()

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
        self.assertEqual(3_600_000, provision.GATEWAY_COMMAND_BUDGET_MAX_MS)
        self.assertRegex(
            anchor_source,
            r"BUILD_ASSERT\s*\(\s*"
            r"DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS\s*<=\s*"
            r"GATEWAY_COMMAND_BUDGET_MAX_MS",
        )

    def test_cli_accepts_declared_three_anchor_assignment_budget(self) -> None:
        argv = [
            "provision_mesh_anchor.py",
            "--gateway",
            "test-gateway",
            "--command",
            "assign-slots",
            "--expected-anchors",
            "3",
            "--command-budget-ms",
            "751204",
        ]
        with (
            mock.patch.object(sys, "argv", argv),
            mock.patch.object(provision.asyncio, "run") as async_run,
        ):
            provision.main()
        async_run.assert_called_once()

    def test_qualification_timeout_covers_firmware_budget_and_delivery_guard(self) -> None:
        self.assertEqual(
            5.0,
            provision.GATEWAY_COMMAND_COMPLETION_GUARD_S,
        )
        self.assertEqual(
            125.0,
            provision._qualification_timeout_s(30.0, 120000),
        )
        self.assertEqual(
            300.0,
            provision._qualification_timeout_s(300.0, 120000),
        )

    def test_survey_terminal_drain_bounds_match_firmware_custody_bounds(self) -> None:
        survey_header = (
            REPO_ROOT / "firmware" / "include" / "survey.h"
        ).read_text(encoding="utf-8")
        gateway_header = (
            REPO_ROOT / "firmware" / "include" / "gateway_command.h"
        ).read_text(encoding="utf-8")
        control_timeout = re.search(
            r"#define\s+SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS\s+(\d+)u\b",
            survey_header,
        )
        validation_hold = re.search(
            r"#define\s+GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS\s+"
            r"(\d+)u\b",
            gateway_header,
        )

        self.assertIsNotNone(control_timeout)
        self.assertIsNotNone(validation_hold)
        assert control_timeout is not None and validation_hold is not None
        self.assertEqual(
            int(control_timeout.group(1)) / 1000.0,
            provision.SURVEY_TERMINAL_DRAIN_MAX_S,
        )
        self.assertEqual(
            int(validation_hold.group(1)) / 1000.0,
            provision.SURVEY_TERMINAL_DRAIN_QUIET_DEFAULT_S,
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

    def test_preexisting_cumulative_loss_is_baselined_but_new_loss_fails(self) -> None:
        events = successful_route_events(retries=0)
        stable = provision.RouteRefreshQualification(
            0x12345, 0x2345, 0x12345
        )
        for event in events:
            stable.observe(dataclasses.replace(event, lost_event_count=17))
        stable.validate()

        changed = provision.RouteRefreshQualification(
            0x12345, 0x2345, 0x12345
        )
        changed.observe(dataclasses.replace(events[0], lost_event_count=17))
        changed.observe(dataclasses.replace(events[-1], lost_event_count=18))
        with self.assertRaisesRegex(RuntimeError, "lost events counter changed"):
            changed.validate()


class AssignmentQualificationTests(unittest.TestCase):
    def test_exact_per_anchor_hop_map_passes_and_mismatch_fails(self) -> None:
        events = successful_assignment_events(3, direct_count=1)
        expected = {0x1000: 1, 0x1001: 3, 0x1002: 4}
        qualification = provision.AssignmentQualification(
            0x12345,
            0x2345,
            0x12345,
            3,
            require_hop_evidence=True,
            expected_anchor_hops=expected,
        )
        for event in events:
            qualification.observe(event)
        qualification.validate()

        mismatched = provision.AssignmentQualification(
            0x12345,
            0x2345,
            0x12345,
            3,
            require_hop_evidence=True,
            expected_anchor_hops={**expected, 0x1001: 2},
        )
        for event in events:
            mismatched.observe(event)
        with self.assertRaisesRegex(RuntimeError, "expected hop count 2, got 3"):
            mismatched.validate()

    def test_expected_anchor_hop_parser_uses_gui_protocol_bound(self) -> None:
        self.assertEqual(
            (0x36E3C2FE6CAC46B2, 3),
            provision._parse_expected_anchor_hop("0x36e3c2fe6cac46b2=3"),
        )
        for invalid in ("broken", "0=1", "1=0", "1=9"):
            with self.subTest(invalid=invalid), self.assertRaises(
                argparse.ArgumentTypeError
            ):
                provision._parse_expected_anchor_hop(invalid)

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
        events[4] = dataclasses.replace(events[4], hop_count=0)
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

    def test_reliable_mapping_depth_qualifies_when_live_claim_events_drop(self) -> None:
        events = successful_assignment_events(3, direct_count=1)
        qualification = provision.AssignmentQualification(
            0x12345,
            0x2345,
            0x12345,
            3,
            require_hop_evidence=True,
            expected_direct_anchors=1,
            expected_multihop_anchors=2,
            expected_anchor_hops={0x1000: 1, 0x1001: 3, 0x1002: 4},
        )
        for event in (events[0], events[1], *events[4:]):
            qualification.observe(event)

        qualification.validate()
        self.assertEqual((1, 0x1000), qualification.hop_paths[0x1000])
        self.assertEqual((3, 0), qualification.hop_paths[0x1001])
        self.assertEqual((4, 0), qualification.hop_paths[0x1002])

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

    def test_contradictory_or_changed_hop_evidence_cannot_qualify(self) -> None:
        events = successful_assignment_events(3, direct_count=1)
        contradictory = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3, require_hop_evidence=True
        )
        for index, event in enumerate(events):
            if index == 1:
                event = dataclasses.replace(event, previous_hop_id=0x1001)
            contradictory.observe(event)
        with self.assertRaisesRegex(RuntimeError, "contradictory previous-hop"):
            contradictory.validate()

        changed = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        for event in events:
            changed.observe(event)
            if (
                event.stage == 6
                and event.anchor_id == 0x1001
                and event.hop_count != 0
            ):
                changed.observe(
                    dataclasses.replace(
                        event,
                        event_sequence=4000,
                        hop_count=4,
                        previous_hop_id=0x1000,
                    )
                )
        with self.assertRaisesRegex(RuntimeError, "changed hop evidence"):
            changed.validate()


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
        return GATEWAY_ID.to_bytes(8, "little")

    async def write_gatt_char(
        self, _characteristic: object, data: bytes, *, response: bool
    ) -> None:
        assert response is False
        self.operations.append(
            ("write", (bytes(data), self.notify_enabled, response))
        )
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
        "survey_id": 0,
        "survey_duration_ms": 1000,
        "discovery_slots": 6,
        "samples": 5,
        "notification_hold_s": 0.25,
        "require_survey_success": True,
        "require_assignment_success": False,
        "expected_anchors": 3,
        "expected_pairs": 3,
        "expected_direct_anchors": None,
        "expected_multihop_anchors": None,
        "expected_anchor_hops": {},
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

    async def test_friendly_gateway_name_resolves_to_matching_service_device(self) -> None:
        device = types.SimpleNamespace(
            address="EF:BD:42:B8:83:0C",
            name="cached gateway name",
        )
        advertisement = types.SimpleNamespace(
            local_name="IMEC Mesh Test Gateway",
            service_uuids=[host_protocol.SERVICE_UUID.upper()],
        )
        scanner = mock.AsyncMock(
            return_value={device.address: (device, advertisement)}
        )
        with mock.patch.object(provision.BleakScanner, "discover", scanner):
            resolved = await provision._resolve_gateway_target(
                "IMEC Mesh Test Gateway",
                1.5,
            )
        self.assertIs(device, resolved)
        scanner.assert_awaited_once_with(timeout=1.5, return_adv=True)

    async def test_gateway_address_skips_scan(self) -> None:
        scanner = mock.AsyncMock()
        with mock.patch.object(provision.BleakScanner, "discover", scanner):
            resolved = await provision._resolve_gateway_target(
                "EF:BD:42:B8:83:0C",
                1.5,
            )
        self.assertEqual("EF:BD:42:B8:83:0C", resolved)
        scanner.assert_not_awaited()

    async def test_completed_monitor_tolerates_bluez_disconnect_eof_only(self) -> None:
        class TeardownEofClient(FakeBleakClient):
            async def __aexit__(self, *_args: object) -> None:
                self.operations.append(("disconnect", self.gateway))
                raise EOFError

        with (
            mock.patch.object(provision, "BleakClient", TeardownEofClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch("builtins.print") as printed,
        ):
            await provision.run(
                args(
                    command="monitor",
                    duration=0.001,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )

        printed.assert_any_call("BLE_COMPLETE packets=0", flush=True)
        printed.assert_any_call(
            "BLE_DISCONNECT_COMPLETE peer already closed D-Bus transport",
            flush=True,
        )

        class ActiveEofClient(FakeBleakClient):
            async def read_gatt_char(self, _uuid: object) -> bytes:
                raise EOFError

        with (
            mock.patch.object(provision, "BleakClient", ActiveEofClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch("builtins.print"),
            self.assertRaises(EOFError),
        ):
            await provision.run(
                args(
                    command="monitor",
                    duration=0.001,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )

    async def _run_unqualified_command_with_packets(
        self,
        packets: list[Packet],
        *,
        command: str = "survey",
    ) -> list[bytes]:
        FakeDecoder.packets = packets
        FakeBleakClient.notification_count = len(packets)
        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(
                    command=command,
                    duration=0.001,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )
        return [
            value[0]
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]

    async def _run_unqualified_survey_with_packets(
        self, packets: list[Packet]
    ) -> list[bytes]:
        return await self._run_unqualified_command_with_packets(packets)

    async def _run_monitor_with_packets(self, packets: list[Packet]) -> list[bytes]:
        FakeDecoder.packets = packets
        FakeBleakClient.notification_count = len(packets)
        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(
                    command="monitor",
                    duration=0.001,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )
        return [
            value[0]
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]

    def _expected_receipt(self, packet: Packet) -> bytes:
        return build_gateway_host_receipt(
            packet,
            host_id=1,
            gateway_id=GATEWAY_ID,
        ).frame

    async def test_qualification_writes_then_holds_then_enables_and_drains(self) -> None:
        events = [
            dataclasses.replace(event, gateway_sequence=0x12345)
            for event in successful_events()
        ]
        FakeDecoder.events = events
        FakeDecoder.packets = [
            pair_packet(0x10, 0x20, 1000, 1, survey_id=0x12345),
            pair_packet(0x10, 0x30, 1200, 2, survey_id=0x12345),
            pair_packet(0x20, 0x30, 1500, 3, survey_id=0x12345),
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
        self.assertEqual(0x12345, qualification.survey_id)
        self.assertEqual(0x12345, command_args["survey_id"])
        self.assertEqual(0x12345, command_args["session_id"])
        self.assertEqual(3, command_args["expected_anchor_count"])
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

    async def test_unretained_gateway_stream_packets_are_not_receipted(self) -> None:
        unretained = dataclasses.replace(gateway_delivery_packet(7), flags=0)
        command_event_packet = gateway_stream_command_event_packet(
            command_event(
                1,
                event_sequence=8,
                command_kind=1,
                command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
            ),
            flags=0,
        )
        non_stream = gateway_delivery_packet(
            9, transport="cobs-shared-packet"
        )
        FakeDecoder.packets = [unretained, command_event_packet, non_stream]
        FakeBleakClient.notification_count = len(FakeDecoder.packets)
        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(
                    command="monitor",
                    duration=0.001,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )

        writes = [
            value[0]
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]
        self.assertEqual([], writes)

    async def test_direct_discovery_report_is_receipted_without_qualification(
        self,
    ) -> None:
        discovery = gateway_stream_discovery_packet(31)
        operation_generation = discovery.value(
            host_protocol.TLV_SURVEY_OPERATION_GENERATION
        )

        self.assertIsInstance(operation_generation, int)
        assert isinstance(operation_generation, int)
        self.assertNotEqual(
            discovery.session_id, operation_generation & 0xFFFFFFFF
        )
        self.assertTrue(provision._survey_custody_record_retained(discovery))

        writes = await self._run_unqualified_survey_with_packets([discovery])

        self.assertEqual(2, len(writes), "command plus exact 0x55 receipt")
        self.assertEqual(self._expected_receipt(discovery), writes[-1])

    async def test_malformed_discovery_report_is_never_receipted(self) -> None:
        valid = gateway_stream_discovery_packet(32)
        operation_generation = valid.value(
            host_protocol.TLV_SURVEY_OPERATION_GENERATION
        )
        assert isinstance(operation_generation, int)
        old_header_identity = dataclasses.replace(
            valid, session_id=operation_generation & 0xFFFFFFFF
        )
        payload_without_generation = b"".join(
            bytes((value.type_id, len(value.raw))) + value.raw
            for value in valid.tlvs
            if value.type_id != host_protocol.TLV_SURVEY_OPERATION_GENERATION
        )
        missing_generation = gateway_stream_packet(
            33,
            msg_type=MSG_SURVEY_DISCOVERY_REPORT,
            payload=payload_without_generation,
            flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            src_id=valid.src_id,
            session_id=valid.session_id,
        )

        for packet in (old_header_identity, missing_generation):
            with self.subTest(sequence=packet.seq):
                with self.assertRaisesRegex(
                    host_protocol.DecodeError,
                    "malformed survey discovery report",
                ):
                    provision._survey_custody_record_retained(packet)

        with self.assertRaisesRegex(
            RuntimeError, "malformed survey discovery report"
        ):
            await self._run_unqualified_survey_with_packets(
                [old_header_identity]
            )
        writes = [
            value[0]
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]
        self.assertNotIn(self._expected_receipt(old_header_identity), writes)

    async def test_all_survey_phase_results_are_receipted(self) -> None:
        reachability = gateway_stream_packet(
            31,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x00, 0x01)),
        )
        prepare = gateway_stream_packet(
            32,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x01, 0x01)),
        )
        start = gateway_stream_packet(
            33,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x02, 0x01)),
        )
        abort = gateway_stream_packet(
            34,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x03, 0x01)),
        )
        unrelated = gateway_stream_packet(
            35,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x02, 0x00)),
        )

        writes = await self._run_unqualified_survey_with_packets(
            [reachability, prepare, start, abort, unrelated]
        )

        self.assertEqual(
            6,
            len(writes),
            "command plus every ACK-required command-result receipt",
        )
        self.assertEqual(
            [
                self._expected_receipt(reachability),
                self._expected_receipt(prepare),
                self._expected_receipt(start),
                self._expected_receipt(abort),
                self._expected_receipt(unrelated),
            ],
            writes[-5:],
        )

    async def test_local_route_and_assignment_results_are_receipted(self) -> None:
        def local_result(sequence: int, command_id: int) -> Packet:
            return gateway_stream_packet(
                sequence,
                msg_type=MSG_COMMAND_RESULT,
                payload=bytes((
                    provision.TLV_COMMAND_ID,
                    2,
                    command_id & 0xFF,
                    command_id >> 8,
                    host_protocol.TLV_COMMAND_STATUS,
                    2,
                    0,
                    0,
                    host_protocol.TLV_REASON,
                    1,
                    0,
                )),
                src_id=GATEWAY_ID,
                dst_id=GATEWAY_ID,
            )

        route_refresh = local_result(48, provision.CMD_FORCE_REDISCOVERY)
        assignment = local_result(49, provision.CMD_ASSIGN_DISCOVERY_SLOTS)
        unrelated = local_result(50, 0x0002)  # CMD_GET_STATUS

        writes = await self._run_monitor_with_packets(
            [route_refresh, assignment, unrelated]
        )

        self.assertEqual(
            [
                self._expected_receipt(route_refresh),
                self._expected_receipt(assignment),
                self._expected_receipt(unrelated),
            ],
            writes,
        )

    async def test_only_ack_required_assignment_publisher_event_is_receipted(
        self,
    ) -> None:
        publisher_event = dataclasses.replace(
            successful_assignment_events(1)[-1],
            route_epoch=7,
            discovery_slot=0xFF,
        )
        generic_event = command_event(
            1,
            event_sequence=51,
            command_kind=1,
            command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
            host_session_id=0x12345,
            host_sequence=0x2345,
            correlation_id=0x12345,
        )
        publisher = gateway_stream_command_event_packet(
            publisher_event,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
        )
        generic = gateway_stream_command_event_packet(
            generic_event,
            flags=0,
        )

        writes = await self._run_monitor_with_packets([publisher, generic])

        self.assertEqual([self._expected_receipt(publisher)], writes)

    async def test_abort_result_is_receipted_during_unrelated_command(self) -> None:
        abort = gateway_stream_packet(
            35,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x03, 0x01)),
        )

        writes = await self._run_unqualified_command_with_packets(
            [abort],
            command="here-i-am",
        )

        self.assertEqual(2, len(writes), "command plus retained ABORT receipt")
        self.assertEqual(self._expected_receipt(abort), writes[-1])

    async def test_failed_survey_terminal_drains_delayed_abort_before_disconnect(
        self,
    ) -> None:
        terminal = command_event(
            12,
            event_sequence=7,
            status=6,
            reason=9,
            total_count=1,
            failure_count=1,
        )
        terminal_packet = types.SimpleNamespace(
            msg_type=provision.MSG_GATEWAY_COMMAND_EVENT,
            src_id=GATEWAY_ID,
            dst_id=1,
            session_id=0x12345,
            seq=7,
            payload=b"terminal",
        )
        abort = gateway_stream_packet(
            8,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x03, 0x01)),
        )
        unrelated_click = gateway_stream_click_packet(80)
        retained_heartbeat = gateway_stream_packet(
            81,
            msg_type=MSG_ANCHOR_HEARTBEAT,
        )

        class OrderedDecoder:
            def feed(self, raw: bytes) -> object:
                packet = {
                    b"terminal": terminal_packet,
                    b"click": unrelated_click,
                    b"heartbeat": retained_heartbeat,
                    b"abort": abort,
                }[raw]
                return types.SimpleNamespace(errors=[], packets=[packet])

        class DelayedAbortClient(FakeBleakClient):
            late_notification: asyncio.Task[None] | None = None

            def __init__(self, *args: object, **kwargs: object) -> None:
                super().__init__(*args, **kwargs)
                self.connected = False

            async def __aenter__(self):
                self.connected = True
                return await super().__aenter__()

            async def __aexit__(self, *_args: object) -> None:
                self.connected = False
                await super().__aexit__(*_args)
                if self.late_notification is not None:
                    await self.late_notification

            async def start_notify(self, _uuid: object, callback: object) -> None:
                self.notify_enabled = True
                self.notify_callback = callback
                self.operations.append(("start_notify", None))
                callback(None, bytearray(b"terminal"))

                async def deliver_abort() -> None:
                    await asyncio.sleep(0.001)
                    if self.connected:
                        callback(None, bytearray(b"click"))
                    await asyncio.sleep(0.001)
                    if self.connected:
                        callback(None, bytearray(b"heartbeat"))
                    await asyncio.sleep(0.005)
                    if self.connected:
                        callback(None, bytearray(b"abort"))

                self.late_notification = asyncio.create_task(deliver_abort())

            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                if not self.connected:
                    raise RuntimeError("write attempted after disconnect")
                await super().write_gatt_char(
                    characteristic,
                    data,
                    response=response,
                )

        with (
            mock.patch.object(provision, "BleakClient", DelayedAbortClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", OrderedDecoder),
            mock.patch.object(
                provision,
                "decode_gateway_command_event",
                return_value=terminal,
            ),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "survey qualification failed"):
                await asyncio.wait_for(
                    provision.run(
                        args(
                            expected_anchors=2,
                            expected_pairs=1,
                            notification_hold_s=0.02,
                        )
                    ),
                    timeout=0.5,
                )

        receipt = self._expected_receipt(abort)
        operations = FakeBleakClient.operations
        writes = [
            value[0]
            for name, value in operations
            if name == "write"
        ]
        self.assertIn(
            receipt,
            writes,
            "the delayed exact ABORT must be receipted before the host leaves",
        )
        self.assertIn(self._expected_receipt(unrelated_click), writes)
        self.assertIn(self._expected_receipt(retained_heartbeat), writes)
        receipt_index = next(
            index
            for index, (name, value) in enumerate(operations)
            if name == "write" and value[0] == receipt
        )
        disconnect_index = next(
            index for index, (name, _value) in enumerate(operations)
            if name == "disconnect"
        )
        self.assertLess(receipt_index, disconnect_index)

    async def test_failed_survey_terminal_drain_has_a_bounded_quiet_timeout(
        self,
    ) -> None:
        terminal = command_event(
            12,
            event_sequence=7,
            status=6,
            reason=9,
            total_count=1,
            failure_count=1,
        )
        terminal_packet = types.SimpleNamespace(
            msg_type=provision.MSG_GATEWAY_COMMAND_EVENT,
            src_id=GATEWAY_ID,
            dst_id=1,
            session_id=0x12345,
            seq=7,
            payload=b"terminal",
        )

        class TerminalDecoder:
            def feed(self, _raw: bytes) -> object:
                return types.SimpleNamespace(errors=[], packets=[terminal_packet])

        class TerminalOnlyClient(FakeBleakClient):
            async def start_notify(self, _uuid: object, callback: object) -> None:
                self.notify_enabled = True
                self.notify_callback = callback
                self.operations.append(("start_notify", None))
                callback(None, bytearray(b"terminal"))

        with (
            mock.patch.object(provision, "BleakClient", TerminalOnlyClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", TerminalDecoder),
            mock.patch.object(
                provision,
                "decode_gateway_command_event",
                return_value=terminal,
            ),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "survey qualification failed"):
                await asyncio.wait_for(
                    provision.run(
                        args(
                            expected_anchors=2,
                            expected_pairs=1,
                            notification_hold_s=0.01,
                        )
                    ),
                    timeout=0.5,
                )

        self.assertEqual("disconnect", FakeBleakClient.operations[-1][0])

    async def test_retained_survey_records_are_receipted_during_monitor(self) -> None:
        discovery = gateway_stream_discovery_packet(34)
        pair_result = gateway_stream_packet(
            35,
            msg_type=MSG_SURVEY_PAIR_RESULT,
        )
        prepare = gateway_stream_packet(
            36,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x01, 0x01)),
        )
        start = gateway_stream_packet(
            37,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x02, 0x01)),
        )
        abort = gateway_stream_packet(
            38,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x03, 0x01)),
        )

        writes = await self._run_monitor_with_packets(
            [discovery, pair_result, prepare, start, abort]
        )

        self.assertEqual(
            [
                self._expected_receipt(discovery),
                self._expected_receipt(pair_result),
                self._expected_receipt(prepare),
                self._expected_receipt(start),
                self._expected_receipt(abort),
            ],
            writes,
        )

    async def test_nonretained_records_remain_unreceipted_during_monitor(self) -> None:
        duplicate_command_ids = gateway_stream_packet(
            39,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes(
                (
                    provision.TLV_COMMAND_ID,
                    2,
                    0x01,
                    0x01,
                    provision.TLV_COMMAND_ID,
                    2,
                    0x02,
                    0x01,
                )
            ),
            flags=0,
        )
        unrelated_result = gateway_stream_packet(
            40,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x02, 0x00)),
            flags=0,
        )
        gateway_local_reachability_result = dataclasses.replace(
            gateway_stream_packet(
                41,
                msg_type=MSG_COMMAND_RESULT,
                payload=bytes((provision.TLV_COMMAND_ID, 2, 0x00, 0x01)),
                flags=0,
            ),
            src_id=GATEWAY_ID,
            dst_id=GATEWAY_ID,
        )
        duplicate_abort_ids = gateway_stream_packet(
            42,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes(
                (
                    provision.TLV_COMMAND_ID,
                    2,
                    0x03,
                    0x01,
                    provision.TLV_COMMAND_ID,
                    2,
                    0x03,
                    0x01,
                )
            ),
            flags=0,
        )
        malformed_abort_id = gateway_stream_packet(
            43,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 1, 0x03)),
            flags=0,
        )
        missing_command_id = gateway_stream_packet(
            44,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((0x05, 1, 0x00)),
            flags=0,
        )
        result_bundle = gateway_stream_packet(
            45, msg_type=MSG_RESULT_BUNDLE, flags=0
        )
        click = gateway_stream_packet(
            46,
            msg_type=MSG_CLICK_REPORT,
            flags=0,
        )
        writes = await self._run_monitor_with_packets(
            [
                duplicate_command_ids,
                unrelated_result,
                gateway_local_reachability_result,
                duplicate_abort_ids,
                malformed_abort_id,
                missing_command_id,
                result_bundle,
                click,
            ]
        )

        self.assertEqual([], writes)

    async def test_all_retained_host_records_release_in_stream_order(
        self,
    ) -> None:
        click = gateway_stream_click_packet(35)
        unrelated_result = gateway_stream_packet(
            36,
            msg_type=MSG_COMMAND_RESULT,
            payload=bytes((provision.TLV_COMMAND_ID, 2, 0x02, 0x00)),
        )
        heartbeat = gateway_stream_packet(
            37,
            msg_type=MSG_ANCHOR_HEARTBEAT,
        )

        writes = await self._run_monitor_with_packets(
            [click, unrelated_result, heartbeat]
        )

        self.assertEqual(
            [
                self._expected_receipt(click),
                self._expected_receipt(unrelated_result),
                self._expected_receipt(heartbeat),
            ],
            writes,
        )

    async def test_concurrent_notifications_serialize_exact_receipt_writes(self) -> None:
        packets = [
            gateway_stream_pair_packet(
                0x10, 0x20, 1000, 21, survey_id=0x12345
            ),
            gateway_stream_pair_packet(
                0x10, 0x30, 1200, 22, survey_id=0x12345
            ),
            gateway_stream_pair_packet(
                0x20, 0x30, 1500, 23, survey_id=0x12345
            ),
        ]
        events = successful_events()
        FakeDecoder.events = events
        FakeDecoder.packets = packets + [
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

        class ConcurrentNotificationClient(FakeBleakClient):
            notification_count = len(FakeDecoder.packets)
            active_writes = 0
            max_active_writes = 0

            def __init__(self, *args: object, **kwargs: object) -> None:
                super().__init__(*args, **kwargs)
                self.services = types.SimpleNamespace(
                    get_characteristic=lambda _uuid: types.SimpleNamespace(
                        max_write_without_response_size=20
                    )
                )

            async def start_notify(self, _uuid: object, callback: object) -> None:
                self.notify_enabled = True
                self.notify_callback = callback
                self.operations.append(("start_notify", None))

                async def invoke_notification() -> None:
                    callback(None, bytearray(b"notification"))
                    await asyncio.sleep(0)

                await asyncio.gather(*(
                    invoke_notification()
                    for _ in range(self.notification_count)
                ))

            async def write_gatt_char(
                self,
                _characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                assert response is False
                type(self).active_writes += 1
                type(self).max_active_writes = max(
                    type(self).max_active_writes,
                    type(self).active_writes,
                )
                self.operations.append(
                    ("write", (bytes(data), self.notify_enabled, response))
                )
                await asyncio.sleep(0)
                type(self).active_writes -= 1

        expected_frames = [
            build_gateway_host_receipt(
                packet,
                host_id=1,
                gateway_id=GATEWAY_ID,
            ).frame
            for packet in packets
        ]
        expected_chunks = [
            frame[offset : offset + 20]
            for frame in expected_frames
            for offset in range(0, len(frame), 20)
        ]
        with (
            mock.patch.object(
                provision, "BleakClient", ConcurrentNotificationClient
            ),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(
                provision,
                "decode_gateway_command_event",
                side_effect=lambda payload, **_kwargs: events[payload[0]],
            ),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(
                    command="survey",
                    duration=1.0,
                    require_survey_success=True,
                    notification_hold_s=0.0,
                )
            )

        write_records = [
            value
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]
        writes = [value[0] for value in write_records]
        self.assertGreater(len(writes), len(expected_chunks))
        self.assertEqual(expected_chunks, writes[-len(expected_chunks):])
        self.assertTrue(
            all(record[2] is False for record in write_records),
            "every command and host-receipt chunk must use WWR",
        )
        self.assertEqual(1, ConcurrentNotificationClient.max_active_writes)

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
            mock.patch.object(
                provision,
                "_qualification_timeout_s",
                side_effect=lambda requested, _budget: requested,
            ),
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

    async def test_combined_reachability_accepts_durable_mapping_without_optional_hops(
        self,
    ) -> None:
        route_events = successful_route_events(
            session_id=0x11111, retries=0, first_sequence=1
        )
        assignment_events = [
            dataclasses.replace(event, hop_count=0, previous_hop_id=0)
            if event.stage == provision.GATEWAY_COMMAND_STAGE_ANCHOR_REPORT
            else event
            for event in successful_assignment_events(
                3,
                session_id=0x22222,
                direct_count=1,
                first_sequence=100,
            )
        ]
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
                    expected_direct_anchors=None,
                    expected_multihop_anchors=None,
                )
            )

        self.assertIsInstance(qualification, provision.AssignmentQualification)
        assert isinstance(qualification, provision.AssignmentQualification)
        self.assertFalse(qualification.require_hop_evidence)
        self.assertEqual(3, len(qualification.anchors))
        self.assertEqual(3, len(qualification.assigned_slots))
        self.assertEqual(0, len(qualification.hop_paths))

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

    async def test_non_strict_monitor_disconnect_is_also_fatal(self) -> None:
        class DisconnectingClient(FakeBleakClient):
            async def start_notify(self, uuid: object, callback: object) -> None:
                await super().start_notify(uuid, callback)
                assert self.disconnected_callback is not None
                self.disconnected_callback(self)

        with (
            mock.patch.object(provision, "BleakClient", DisconnectingClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "disconnected"):
                await provision.run(
                    args(
                        command="monitor",
                        require_survey_success=False,
                        notification_hold_s=0.0,
                    )
                )

    async def test_strict_survey_rejects_reusable_manual_survey_identity(self) -> None:
        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "auto-generated survey ID"):
                await provision.run(args(survey_id=0x778899AA))


if __name__ == "__main__":
    unittest.main()
