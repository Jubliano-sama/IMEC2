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
        self.assertEqual(1_800_000, survey_default)
        self.assertEqual(260_277, discovery_default)
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
        mesh_header = (
            REPO_ROOT / "firmware" / "include" / "mesh.h"
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
        max_hops = self._literal_macro(mesh_header, "MESH_NETWORK_MAX_HOPS")
        direct_ms = report_capacity * service_ms + flow_guard_ms
        firmware_max_ms = direct_ms + (max_hops - 1) * per_hop_ms

        self.assertEqual(58_000, firmware_max_ms)
        self.assertEqual(
            firmware_max_ms,
            host_operation_policy.DISCOVERY_REPORT_CUSTODY_MAX_MS,
        )

    def test_headless_survey_policy_uses_depth_aware_start_and_exact_budget(
        self,
    ) -> None:
        expected_start_by_depth = {
            1: 20_000,
            5: 20_000,
            6: 21_104,
            7: 23_104,
            8: 25_104,
            0: 25_104,
        }
        for deepest_hop, expected_start_ms in expected_start_by_depth.items():
            with self.subTest(deepest_hop=deepest_hop):
                policy = provision._survey_operation_policy(
                    expected_anchor_count=6,
                    discovery_slot_count=6,
                    report_grace_ms=250,
                    deepest_hop=deepest_hop,
                )
                self.assertEqual(
                    expected_start_ms,
                    policy.discovery.start_delay_ms,
                )
                self.assertEqual(
                    host_operation_policy.discovery_required_budget_ms(
                        expected_start_ms,
                        host_operation_policy.DISCOVERY_DEFAULT_SLOT_MS,
                        6,
                        host_operation_policy.DISCOVERY_DEFAULT_ROUND_COUNT,
                        250,
                        deepest_hop,
                    ),
                    policy.discovery.operation_budget_ms,
                )

        with self.assertRaisesRegex(ValueError, "deepest hop"):
            provision._survey_operation_policy(
                expected_anchor_count=6,
                discovery_slot_count=6,
                report_grace_ms=250,
                deepest_hop=9,
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
        self.assertEqual(261_027, required_ms)

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
        def close_cli_coroutine(coroutine) -> None:
            coroutine.close()

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
            mock.patch.object(
                provision.asyncio,
                "run",
                side_effect=close_cli_coroutine,
            ) as async_run,
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
        mesh_header = (
            REPO_ROOT / "firmware" / "include" / "mesh.h"
        ).read_text(encoding="utf-8")
        control_base_ms = self._literal_macro(
            survey_header, "SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS"
        )
        control_per_hop_ms = self._literal_macro(
            survey_header, "SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS"
        )
        max_hops = self._literal_macro(mesh_header, "MESH_NETWORK_MAX_HOPS")
        control_timeout_ms = (
            control_base_ms + (max_hops - 1) * control_per_hop_ms
        )
        validation_hold = re.search(
            r"#define\s+GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS\s+"
            r"(\d+)u\b",
            gateway_header,
        )

        self.assertIsNotNone(validation_hold)
        assert validation_hold is not None
        self.assertEqual(
            control_timeout_ms / 1000.0,
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
    def test_negative_command_result_fails_matching_qualification_early(self) -> None:
        qualification = provision.AssignmentQualification(
            0x11223344,
            0x3344,
            0x11223344,
            3,
        )
        packet = gateway_stream_packet(
            0x3344,
            msg_type=MSG_COMMAND_RESULT,
            flags=FLAG_GATEWAY_ACK_REQUIRED | host_protocol.FLAG_ERROR,
            src_id=GATEWAY_ID,
            dst_id=GATEWAY_ID,
            session_id=0x11223344,
            payload=bytes((
                provision.TLV_COMMAND_ID, 2, 0x04, 0x01,
                host_protocol.TLV_COMMAND_STATUS, 2, 5, 0,
                host_protocol.TLV_REASON, 1, 9,
            )),
        )

        self.assertEqual(
            "gateway command failed before lifecycle terminal "
            "(command=260 status=5 reason=9)",
            provision._failed_qualification_command_result(
                packet, qualification
            ),
        )
        self.assertIsNone(
            provision._failed_qualification_command_result(
                dataclasses.replace(packet, session_id=0x11223345),
                qualification,
            )
        )

    def test_table_ack_depth_wins_over_stale_claim_route_sidecar(self) -> None:
        direct_d = 0x1000
        direct_c = 0x1001
        forced_b = 0x1002
        events = [
            command_event(
                4,
                event_sequence=1,
                command_kind=1,
                command_id=0x0104,
            ),
            command_event(
                6,
                event_sequence=2,
                command_kind=1,
                command_id=0x0104,
                anchor_id=direct_d,
                previous_hop_id=direct_d,
                progress_count=1,
                hop_count=1,
                discovery_slot=0xFF,
            ),
            # The anchor still estimates itself at depth two, even though this
            # CLAIM reached the gateway directly (previous hop is itself).
            command_event(
                6,
                event_sequence=3,
                command_kind=1,
                command_id=0x0104,
                anchor_id=direct_c,
                previous_hop_id=direct_c,
                progress_count=2,
                hop_count=2,
                discovery_slot=0xFF,
            ),
            command_event(
                6,
                event_sequence=4,
                command_kind=1,
                command_id=0x0104,
                anchor_id=forced_b,
                previous_hop_id=direct_d,
                progress_count=3,
                hop_count=2,
                discovery_slot=0xFF,
            ),
            command_event(
                7,
                event_sequence=5,
                command_kind=1,
                command_id=0x0104,
                progress_count=3,
                total_count=3,
            ),
            # Per-anchor TABLE ACK confirmations carry the current, reliable
            # depth proof and arrive before the durable publisher drains.
            command_event(
                8,
                event_sequence=6,
                command_kind=1,
                command_id=0x0104,
                anchor_id=direct_c,
                attempt=0,
                progress_count=1,
                total_count=3,
                hop_count=1,
                discovery_slot=2,
            ),
            command_event(
                8,
                event_sequence=7,
                command_kind=1,
                command_id=0x0104,
                anchor_id=direct_d,
                attempt=0,
                progress_count=2,
                total_count=3,
                hop_count=1,
                discovery_slot=1,
            ),
            command_event(
                8,
                event_sequence=8,
                command_kind=1,
                command_id=0x0104,
                anchor_id=forced_b,
                attempt=0,
                progress_count=3,
                total_count=3,
                hop_count=2,
                discovery_slot=0,
            ),
            # The reliable mapping batch was snapshotted before those ACKs,
            # so C's slot is authoritative while its hop sidecar is stale.
            command_event(
                6,
                event_sequence=9,
                command_kind=1,
                command_id=0x0104,
                anchor_id=forced_b,
                progress_count=1,
                total_count=3,
                success_count=1,
                hop_count=2,
                discovery_slot=0,
            ),
            command_event(
                6,
                event_sequence=10,
                command_kind=1,
                command_id=0x0104,
                anchor_id=direct_d,
                progress_count=2,
                total_count=3,
                success_count=1,
                hop_count=1,
                discovery_slot=1,
            ),
            command_event(
                6,
                event_sequence=11,
                command_kind=1,
                command_id=0x0104,
                anchor_id=direct_c,
                progress_count=3,
                total_count=3,
                success_count=1,
                hop_count=2,
                discovery_slot=2,
            ),
            command_event(
                7,
                event_sequence=12,
                command_kind=1,
                command_id=0x0104,
                progress_count=3,
                total_count=3,
                success_count=3,
            ),
            command_event(
                8,
                event_sequence=13,
                command_kind=1,
                command_id=0x0104,
                progress_count=3,
                total_count=3,
            ),
            command_event(
                12,
                event_sequence=14,
                command_kind=1,
                command_id=0x0104,
                progress_count=3,
                total_count=3,
                success_count=3,
            ),
        ]

        qualification = provision.AssignmentQualification(
            0x12345,
            0x2345,
            0x12345,
            3,
            require_hop_evidence=True,
            expected_direct_anchors=2,
            expected_multihop_anchors=1,
            expected_anchor_hops={direct_d: 1, direct_c: 1, forced_b: 2},
        )
        for event in events:
            qualification.observe(event)

        qualification.validate()
        self.assertEqual(2, qualification.direct_count)
        self.assertEqual(1, qualification.multihop_count)

        outside_roster = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3, require_hop_evidence=True
        )
        for event in events:
            if (
                event.stage == 6
                and event.anchor_id == forced_b
                and event.discovery_slot == 0xFF
            ):
                event = dataclasses.replace(event, previous_hop_id=0xDEADBEEF)
            outside_roster.observe(event)
        with self.assertRaisesRegex(RuntimeError, "outside the qualified roster"):
            outside_roster.validate()

    def test_live_ack_confirm_progress_is_distinct_from_table_publication(self) -> None:
        events = successful_assignment_events(3, direct_count=3)
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        for event in events[:4]:
            qualification.observe(event)
        for index in range(3):
            qualification.observe(
                command_event(
                    8,
                    event_sequence=100 + index,
                    command_kind=1,
                    command_id=0x0104,
                    host_session_id=0x12345,
                    host_sequence=0x2345,
                    correlation_id=0x12345,
                    anchor_id=0x1000 + index,
                    progress_count=index + 1,
                    total_count=3,
                    hop_count=1,
                    discovery_slot=index,
                )
            )
        for event in events[4:]:
            qualification.observe(event)

        qualification.validate()
        self.assertEqual(
            {0x1000: 0, 0x1001: 1, 0x1002: 2},
            qualification.confirmed_slots,
        )

    def test_ack_confirm_slot_must_match_reliable_publication(self) -> None:
        events = successful_assignment_events(3, direct_count=3)
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        qualification.observe(
            command_event(
                8,
                event_sequence=100,
                command_kind=1,
                command_id=0x0104,
                host_session_id=0x12345,
                host_sequence=0x2345,
                correlation_id=0x12345,
                anchor_id=0x1000,
                progress_count=1,
                total_count=3,
                hop_count=1,
                discovery_slot=2,
            )
        )
        for event in events:
            qualification.observe(event)

        with self.assertRaisesRegex(RuntimeError, "confirmed slot 2, published slot 0"):
            qualification.validate()

    def test_sparse_retained_slots_are_valid(self) -> None:
        events = successful_assignment_events(3, direct_count=3)
        sparse_slots = {0x1000: 1, 0x1001: 4, 0x1002: 9}
        qualification = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3
        )
        for event in events:
            if event.stage == 6 and event.discovery_slot != 0xFF:
                event = dataclasses.replace(
                    event, discovery_slot=sparse_slots[event.anchor_id]
                )
            qualification.observe(event)

        qualification.validate()

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

    def test_outside_roster_or_changed_hop_evidence_cannot_qualify(self) -> None:
        events = successful_assignment_events(3, direct_count=1)
        outside_roster = provision.AssignmentQualification(
            0x12345, 0x2345, 0x12345, 3, require_hop_evidence=True
        )
        for index, event in enumerate(events):
            if index == 2:
                event = dataclasses.replace(event, previous_hop_id=0xDEADBEEF)
            outside_roster.observe(event)
        with self.assertRaisesRegex(RuntimeError, "outside the qualified roster"):
            outside_roster.validate()

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
        self.is_connected = False

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *_args: object) -> None:
        await self.disconnect()

    async def connect(self) -> None:
        self.is_connected = True
        self.operations.append(("connect", self.gateway))

    async def disconnect(self) -> None:
        self.is_connected = False
        self.operations.append(("disconnect", self.gateway))

    async def read_gatt_char(self, _uuid: object) -> bytes:
        self.operations.append(("read_identity", None))
        return GATEWAY_ID.to_bytes(8, "little")

    async def write_gatt_char(
        self, _characteristic: object, data: bytes, *, response: bool
    ) -> None:
        assert response is True
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
        "reconnect_attempts": 3,
        "reconnect_delay_s": 1.5,
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
        "command_budget_ms": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class QualificationTimeoutProbe(float):
    """Record when a resolved timeout is turned into an absolute deadline."""

    effective_budget_ms: int
    deadline_origins: list[float]

    def __new__(
        cls,
        timeout_s: float,
        effective_budget_ms: int,
    ) -> "QualificationTimeoutProbe":
        instance = super().__new__(cls, timeout_s)
        instance.effective_budget_ms = effective_budget_ms
        instance.deadline_origins = []
        return instance

    def __radd__(self, started_at: float) -> float:
        self.deadline_origins.append(float(started_at))
        return float(started_at) + float(self)


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

    async def test_unqualified_survey_preserves_optional_expected_anchor_tlv(
        self,
    ) -> None:
        built_commands: list[tuple[dict[str, object], object]] = []
        real_build_anchor_discovery_command = (
            provision.build_anchor_discovery_command
        )

        def capture_command(**kwargs: object) -> object:
            command = real_build_anchor_discovery_command(**kwargs)
            built_commands.append((dict(kwargs), command))
            return command

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(
                provision,
                "build_anchor_discovery_command",
                side_effect=capture_command,
            ),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(
                    command="survey",
                    duration=0.001,
                    expected_anchors=3,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )

        self.assertEqual(1, len(built_commands))
        command_args, command = built_commands[0]
        self.assertEqual(3, command_args["expected_anchor_count"])
        self.assertEqual(
            3,
            command.packet.value(host_protocol.TLV_EXPECTED_NODE_COUNT),
        )

        omitted_args = dict(command_args)
        omitted_args["expected_anchor_count"] = None
        omitted = real_build_anchor_discovery_command(**omitted_args)
        self.assertIsNone(
            omitted.packet.value(host_protocol.TLV_EXPECTED_NODE_COUNT)
        )

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
        policy = command_args["operation_policy"]
        self.assertIsInstance(policy, host_operation_policy.OperationPolicyProfile)
        self.assertEqual(3, policy.assignment.expected_anchor_count)
        self.assertEqual(6, policy.discovery.slot_count)
        self.assertEqual(1000, policy.discovery.report_grace_ms)
        self.assertEqual(
            host_operation_policy.discovery_required_budget_ms(
                host_operation_policy.DISCOVERY_DEFAULT_START_DELAY_MS,
                host_operation_policy.DISCOVERY_DEFAULT_SLOT_MS,
                6,
                host_operation_policy.DISCOVERY_DEFAULT_ROUND_COUNT,
                1000,
            ),
            policy.discovery.operation_budget_ms,
        )
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

    async def test_zero_hold_strict_survey_drains_assignment_before_target(
        self,
    ) -> None:
        old_session = 0x33333
        live_publication_events = [
            dataclasses.replace(
                command_event(
                    6,
                    event_sequence=200,
                    command_kind=1,
                    command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
                    host_session_id=old_session,
                    host_sequence=old_session & 0xFFFF,
                    correlation_id=old_session,
                    anchor_id=0x4444,
                    progress_count=1,
                    total_count=3,
                    success_count=1,
                    discovery_slot=0,
                ),
                flags=0,
                route_epoch=7,
            ),
            dataclasses.replace(
                command_event(
                    12,
                    event_sequence=203,
                    command_kind=1,
                    command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
                    host_session_id=old_session,
                    host_sequence=old_session & 0xFFFF,
                    correlation_id=old_session,
                    progress_count=3,
                    total_count=3,
                    success_count=3,
                    discovery_slot=0xFF,
                ),
                flags=host_protocol.GATEWAY_COMMAND_EVENT_FLAG_TERMINAL,
                route_epoch=7,
            ),
        ]
        live_publication_packets = [
            gateway_stream_command_event_packet(
                event, flags=FLAG_GATEWAY_ACK_REQUIRED
            )
            for event in live_publication_events
        ]
        survey_events = [
            dataclasses.replace(event, gateway_sequence=0x12345)
            for event in successful_events()
        ]
        survey_packets = [
            pair_packet(0x10, 0x20, 1000, 1, survey_id=0x12345),
            pair_packet(0x10, 0x30, 1200, 2, survey_id=0x12345),
            pair_packet(0x20, 0x30, 1500, 3, survey_id=0x12345),
        ] + [
            gateway_stream_command_event_packet(event, flags=0)
            for event in survey_events
        ]
        FakeDecoder.packets = live_publication_packets + survey_packets
        FakeBleakClient.notification_count = 1
        FakeBleakClient.write_notification_counts = [
            1,
            0,
            len(survey_packets),
        ]

        class LivePublicationBleakClient(FakeBleakClient):
            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                await super().write_gatt_char(
                    characteristic, data, response=response
                )
                await asyncio.sleep(0)

        with (
            mock.patch.object(
                provision, "BleakClient", LivePublicationBleakClient
            ),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            qualification = await asyncio.wait_for(
                provision.run(
                    args(
                        command="survey",
                        duration=1.0,
                        require_survey_success=True,
                        notification_hold_s=0.0,
                    )
                ),
                timeout=1.0,
            )

        operation_names = [
            name for name, _value in FakeBleakClient.operations
        ]
        writes = [
            value
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]
        expected_receipts = [
            self._expected_receipt(packet)
            for packet in live_publication_packets
        ]
        self.assertLess(
            operation_names.index("start_notify"),
            operation_names.index("write"),
        )
        self.assertEqual(expected_receipts, [value[0] for value in writes[:2]])
        self.assertEqual(
            provision.CMD_SURVEY_REACHABILITY,
            parse_cobs_packet(writes[2][0]).value(provision.TLV_COMMAND_ID),
        )
        self.assertTrue(
            all(value[1] for value in writes),
            "zero-hold strict survey wrote before notifications were active",
        )
        self.assertIsInstance(qualification, provision.SurveyQualification)

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
        self.assertEqual(self._expected_receipt(discovery), writes[0])
        self.assertEqual(
            provision.CMD_SURVEY_REACHABILITY,
            parse_cobs_packet(writes[1]).value(provision.TLV_COMMAND_ID),
        )

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
            [
                write
                for write in writes
                if write
                in {
                    self._expected_receipt(reachability),
                    self._expected_receipt(prepare),
                    self._expected_receipt(start),
                    self._expected_receipt(abort),
                    self._expected_receipt(unrelated),
                }
            ],
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

    async def test_retained_generic_event_is_receipted_without_publisher_barrier(
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
            flags=FLAG_GATEWAY_ACK_REQUIRED,
        )

        writes = await self._run_monitor_with_packets([publisher, generic])

        self.assertEqual(
            [self._expected_receipt(publisher), self._expected_receipt(generic)],
            writes,
        )

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

    async def test_best_effort_anchor_heartbeat_needs_no_host_receipt(self) -> None:
        heartbeat = gateway_stream_packet(
            38,
            msg_type=MSG_ANCHOR_HEARTBEAT,
            flags=0,
        )

        writes = await self._run_monitor_with_packets([heartbeat])

        self.assertEqual([], writes)

    async def test_no_response_write_cannot_drop_final_delimiter_silently(
        self,
    ) -> None:
        class DropFinalChunkForNoResponseClient(FakeBleakClient):
            attempted_writes: list[tuple[bytes, bool]] = []
            delivered_writes: list[tuple[bytes, bool]] = []

            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                chunk = bytes(data)
                type(self).attempted_writes.append((chunk, response))
                if not response and chunk.endswith(b"\x00"):
                    # BlueZ may report a locally queued write command as done
                    # even though the gateway never admits the frame delimiter.
                    return
                type(self).delivered_writes.append((chunk, response))
                await super().write_gatt_char(
                    characteristic,
                    chunk,
                    response=response,
                )

        with (
            mock.patch.object(
                provision, "BleakClient", DropFinalChunkForNoResponseClient
            ),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            await provision.run(
                args(
                    command="survey",
                    duration=0.001,
                    require_survey_success=False,
                    notification_hold_s=0.0,
                )
            )

        attempted = DropFinalChunkForNoResponseClient.attempted_writes
        delivered = DropFinalChunkForNoResponseClient.delivered_writes
        self.assertTrue(attempted)
        self.assertTrue(all(response for _chunk, response in attempted))
        self.assertEqual(attempted, delivered)
        self.assertTrue(
            b"".join(chunk for chunk, _response in delivered).endswith(b"\x00")
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
            responses_emitted = False

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

            async def write_gatt_char(
                self,
                _characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                assert response is True
                type(self).active_writes += 1
                type(self).max_active_writes = max(
                    type(self).max_active_writes,
                    type(self).active_writes,
                )
                self.operations.append(
                    ("write", (bytes(data), self.notify_enabled, response))
                )
                await asyncio.sleep(0)
                if (
                    not type(self).responses_emitted
                    and data.endswith(b"\x00")
                ):
                    type(self).responses_emitted = True
                    assert self.notify_callback is not None

                    async def invoke_notification() -> None:
                        assert self.notify_callback is not None
                        self.notify_callback(None, bytearray(b"notification"))
                        await asyncio.sleep(0)

                    await asyncio.gather(*(
                        invoke_notification()
                        for _ in range(self.notification_count)
                    ))
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
            all(record[2] is True for record in write_records),
            "every command and host-receipt chunk must use ATT write requests",
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
        FakeBleakClient.notification_count = 0
        FakeBleakClient.write_notification_counts = [
            len(route_events),
            len(assignment_events),
        ]

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
            qualification = await asyncio.wait_for(
                provision.run(
                    args(
                        command="qualify-reachability",
                        require_survey_success=False,
                        notification_hold_s=0.0,
                        expected_direct_anchors=1,
                        expected_multihop_anchors=2,
                    )
                ),
                timeout=1.0,
            )

        writes = [value for name, value in FakeBleakClient.operations if name == "write"]
        self.assertEqual(2, len(writes))
        operation_names = [name for name, _value in FakeBleakClient.operations]
        self.assertLess(
            operation_names.index("start_notify"), operation_names.index("write")
        )
        self.assertTrue(writes[0][1])
        self.assertTrue(writes[1][1])
        self.assertIsInstance(qualification, provision.AssignmentQualification)
        assert isinstance(qualification, provision.AssignmentQualification)
        self.assertEqual(3, len(qualification.anchors))

    async def test_combined_reachability_drains_live_assignment_before_dispatch(
        self,
    ) -> None:
        route_events = successful_route_events(
            session_id=0x11111, retries=0, first_sequence=1
        )
        assignment_events = successful_assignment_events(
            3,
            session_id=0x22222,
            direct_count=1,
            first_sequence=100,
        )
        old_session = 0x33333
        live_publication_events = [
            dataclasses.replace(
                command_event(
                    6,
                    event_sequence=200,
                    command_kind=1,
                    command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
                    host_session_id=old_session,
                    host_sequence=old_session & 0xFFFF,
                    correlation_id=old_session,
                    anchor_id=0x4444,
                    progress_count=1,
                    total_count=3,
                    success_count=1,
                    discovery_slot=0,
                ),
                flags=0,
                route_epoch=7,
            ),
            dataclasses.replace(
                command_event(
                    7,
                    event_sequence=201,
                    command_kind=1,
                    command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
                    host_session_id=old_session,
                    host_sequence=old_session & 0xFFFF,
                    correlation_id=old_session,
                    progress_count=3,
                    total_count=3,
                    discovery_slot=0xFF,
                ),
                flags=0,
                route_epoch=7,
            ),
            dataclasses.replace(
                command_event(
                    8,
                    event_sequence=202,
                    command_kind=1,
                    command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
                    host_session_id=old_session,
                    host_sequence=old_session & 0xFFFF,
                    correlation_id=old_session,
                    progress_count=3,
                    total_count=3,
                    discovery_slot=0xFF,
                ),
                flags=0,
                route_epoch=7,
            ),
            dataclasses.replace(
                command_event(
                    12,
                    event_sequence=203,
                    command_kind=1,
                    command_id=provision.CMD_ASSIGN_DISCOVERY_SLOTS,
                    host_session_id=old_session,
                    host_sequence=old_session & 0xFFFF,
                    correlation_id=old_session,
                    progress_count=3,
                    total_count=3,
                    success_count=3,
                    discovery_slot=0xFF,
                ),
                flags=host_protocol.GATEWAY_COMMAND_EVENT_FLAG_TERMINAL,
                route_epoch=7,
            ),
        ]
        live_publication_packets = [
            gateway_stream_command_event_packet(
                event, flags=FLAG_GATEWAY_ACK_REQUIRED
            )
            for event in live_publication_events
        ]
        route_packets = [
            gateway_stream_command_event_packet(event, flags=0)
            for event in route_events
        ]
        assignment_packets = [
            gateway_stream_command_event_packet(event, flags=0)
            for event in assignment_events
        ]
        FakeDecoder.packets = (
            [live_publication_packets[0]]
            + route_packets
            + live_publication_packets[1:]
            + assignment_packets
        )
        # Subscription may expose only already-retained publisher custody.
        # The Here-I-Am result itself cannot exist until its command write;
        # release those route events from that write just like the real link.
        FakeBleakClient.notification_count = 1
        # Each receipt releases exactly the next retained publisher event.
        # Only the successor assignment command releases its own events.
        FakeBleakClient.write_notification_counts = [
            len(route_packets),
            1,
            1,
            1,
            0,
            len(assignment_packets),
        ]

        class LivePublicationBleakClient(FakeBleakClient):
            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                await super().write_gatt_char(
                    characteristic, data, response=response
                )
                # Real Bleak writes always yield to completion callbacks.  The
                # fixture must do the same as each receipt releases the next
                # retained publisher record.
                await asyncio.sleep(0)

        with (
            mock.patch.object(provision, "BleakClient", LivePublicationBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(
                provision, "_new_identity", side_effect=[0x11111, 0x22222]
            ),
            mock.patch("builtins.print"),
        ):
            qualification = await asyncio.wait_for(
                provision.run(
                    args(
                        command="qualify-reachability",
                        require_survey_success=False,
                        notification_hold_s=0.0,
                        expected_direct_anchors=1,
                        expected_multihop_anchors=2,
                    )
                ),
                timeout=1.0,
            )

        writes = [
            value[0]
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]
        expected_receipts = [
            self._expected_receipt(packet) for packet in live_publication_packets
        ]
        self.assertEqual(expected_receipts, writes[1:5])
        self.assertEqual(6, len(writes), "expected two commands and four receipts")
        self.assertEqual(
            provision.CMD_FORCE_REDISCOVERY,
            parse_cobs_packet(writes[0]).value(provision.TLV_COMMAND_ID),
        )
        self.assertEqual(
            provision.CMD_ASSIGN_DISCOVERY_SLOTS,
            parse_cobs_packet(writes[5]).value(provision.TLV_COMMAND_ID),
        )
        self.assertIsInstance(qualification, provision.AssignmentQualification)

    async def test_combined_reachability_binds_short_budget_to_three_anchor_policy(
        self,
    ) -> None:
        route_events = successful_route_events(
            session_id=0x11111, retries=0, first_sequence=1
        )
        assignment_events = successful_assignment_events(
            3,
            session_id=0x22222,
            direct_count=1,
            first_sequence=100,
        )
        events = route_events + assignment_events
        FakeDecoder.events = events
        FakeBleakClient.notification_count = 0
        FakeBleakClient.write_notification_counts = [
            len(route_events),
            len(assignment_events),
        ]
        command_args: dict[str, object] = {}
        route_args: dict[str, object] = {}
        commands: list[object] = []
        timeout_probes: list[QualificationTimeoutProbe] = []
        real_build_assignment = provision.build_assign_discovery_slots_command
        real_build_here_i_am = provision.build_here_i_am_command
        real_required_budget = provision.assignment_required_budget_ms
        real_qualification_timeout = provision._qualification_timeout_s
        budget_ms = (
            host_operation_policy.assignment_required_budget_ms(
                host_operation_policy.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
                3,
            )
            + 1
        )

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        def capture_assignment(**kwargs: object) -> object:
            command_args.update(kwargs)
            command = real_build_assignment(**kwargs)
            commands.append(command)
            return command

        def capture_here_i_am(**kwargs: object) -> object:
            route_args.update(kwargs)
            command = real_build_here_i_am(**kwargs)
            commands.append(command)
            return command

        def capture_timeout(
            requested_timeout_s: float,
            effective_budget_ms: int,
        ) -> QualificationTimeoutProbe:
            probe = QualificationTimeoutProbe(
                real_qualification_timeout(
                    requested_timeout_s,
                    effective_budget_ms,
                ),
                effective_budget_ms,
            )
            timeout_probes.append(probe)
            return probe

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "decode_gateway_command_event", fake_decode),
            mock.patch.object(
                provision,
                "assignment_required_budget_ms",
                wraps=real_required_budget,
            ) as required_budget,
            mock.patch.object(
                provision,
                "_qualification_timeout_s",
                side_effect=capture_timeout,
            ),
            mock.patch.object(
                provision,
                "build_assign_discovery_slots_command",
                side_effect=capture_assignment,
            ),
            mock.patch.object(
                provision,
                "build_here_i_am_command",
                side_effect=capture_here_i_am,
            ),
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
                    command_budget_ms=budget_ms,
                )
            )

        self.assertIsInstance(qualification, provision.AssignmentQualification)
        self.assertEqual(budget_ms, command_args["command_budget_ms"])
        self.assertEqual(3, command_args["expected_anchor_count"])
        policy = command_args["operation_policy"]
        self.assertIsInstance(policy, host_operation_policy.OperationPolicyProfile)
        self.assertEqual(3, policy.assignment.expected_anchor_count)
        self.assertEqual(budget_ms, policy.assignment.operation_budget_ms)
        self.assertEqual(
            host_operation_policy.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            policy.assignment.response_spread_ms,
        )
        self.assertTrue(policy.assignment.ram_only_iteration)
        self.assertEqual(budget_ms, route_args["command_budget_ms"])
        self.assertEqual(policy, route_args["operation_policy"])
        required_budget.assert_not_called()
        self.assertEqual(2, len(commands))
        self.assertEqual(
            [budget_ms, budget_ms],
            [
                command.packet.value(host_protocol.TLV_COMMAND_BUDGET_MS)
                for command in commands
            ],
        )
        self.assertEqual(
            [budget_ms, budget_ms],
            [
                probe.effective_budget_ms
                for probe in timeout_probes
                if probe.deadline_origins
            ],
        )

    async def test_assign_slots_omitted_budget_binds_one_topology_budget_everywhere(
        self,
    ) -> None:
        expected_anchors = 3
        deepest_hop = 2
        identity = 0x12345
        events = successful_assignment_events(
            expected_anchors,
            session_id=identity,
            direct_count=1,
        )
        FakeDecoder.events = events
        FakeBleakClient.write_notification_counts = [len(events)]
        built_commands: list[tuple[dict[str, object], object]] = []
        timeout_probes: list[QualificationTimeoutProbe] = []
        real_build_assignment = provision.build_assign_discovery_slots_command
        real_required_budget = provision.assignment_required_budget_ms
        real_qualification_timeout = provision._qualification_timeout_s
        expected_budget_ms = host_operation_policy.assignment_required_budget_ms(
            host_operation_policy.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            expected_anchors,
            deepest_hop,
        )

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        def capture_assignment(**kwargs: object) -> object:
            command = real_build_assignment(**kwargs)
            built_commands.append((dict(kwargs), command))
            return command

        def capture_timeout(
            requested_timeout_s: float,
            effective_budget_ms: int,
        ) -> QualificationTimeoutProbe:
            probe = QualificationTimeoutProbe(
                real_qualification_timeout(
                    requested_timeout_s,
                    effective_budget_ms,
                ),
                effective_budget_ms,
            )
            timeout_probes.append(probe)
            return probe

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "decode_gateway_command_event", fake_decode),
            mock.patch.object(
                provision,
                "assignment_required_budget_ms",
                wraps=real_required_budget,
            ) as required_budget,
            mock.patch.object(
                provision,
                "build_assign_discovery_slots_command",
                side_effect=capture_assignment,
            ),
            mock.patch.object(
                provision,
                "_qualification_timeout_s",
                side_effect=capture_timeout,
            ),
            mock.patch.object(provision, "_new_identity", return_value=identity),
            mock.patch("builtins.print"),
        ):
            qualification = await provision.run(
                args(
                    command="assign-slots",
                    require_survey_success=False,
                    require_assignment_success=True,
                    notification_hold_s=0.0,
                    expected_anchors=expected_anchors,
                    expected_direct_anchors=1,
                    expected_multihop_anchors=2,
                    deepest_hop=deepest_hop,
                )
            )

        self.assertIsInstance(qualification, provision.AssignmentQualification)
        required_budget.assert_called_once_with(
            host_operation_policy.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            expected_anchors,
            deepest_hop=deepest_hop,
        )
        self.assertEqual(1, len(built_commands))
        command_args, command = built_commands[0]
        policy = command_args["operation_policy"]
        self.assertIsInstance(policy, host_operation_policy.OperationPolicyProfile)
        self.assertEqual(
            expected_budget_ms,
            policy.assignment.operation_budget_ms,
        )
        self.assertEqual(deepest_hop, policy.assignment.deepest_hop)
        self.assertTrue(policy.assignment.ram_only_iteration)
        assignment_policy_tlvs = [
            tlv
            for tlv in command.packet.tlvs
            if tlv.type_id == host_protocol.TLV_OPERATION_POLICY
            and tlv.decoded["family"] == "assignment"
        ]
        self.assertEqual(1, len(assignment_policy_tlvs))
        self.assertEqual(
            policy.assignment.encode_value(),
            assignment_policy_tlvs[0].raw,
        )
        self.assertTrue(
            assignment_policy_tlvs[0].decoded["ram_only_iteration"]
        )
        self.assertEqual(expected_budget_ms, command_args["command_budget_ms"])
        self.assertEqual(
            expected_budget_ms,
            command.packet.value(host_protocol.TLV_COMMAND_BUDGET_MS),
        )
        self.assertEqual(
            [expected_budget_ms],
            [
                probe.effective_budget_ms
                for probe in timeout_probes
                if probe.deadline_origins
            ],
        )

    async def test_combined_reachability_omitted_budget_binds_one_topology_budget_everywhere(
        self,
    ) -> None:
        expected_anchors = 3
        deepest_hop = 2
        route_events = successful_route_events(
            session_id=0x11111,
            retries=0,
            first_sequence=1,
        )
        assignment_events = successful_assignment_events(
            expected_anchors,
            session_id=0x22222,
            direct_count=1,
            first_sequence=100,
        )
        events = route_events + assignment_events
        FakeDecoder.events = events
        FakeBleakClient.write_notification_counts = [
            len(route_events),
            len(assignment_events),
        ]
        built_commands: list[tuple[dict[str, object], object]] = []
        timeout_probes: list[QualificationTimeoutProbe] = []
        real_build_assignment = provision.build_assign_discovery_slots_command
        real_build_here_i_am = provision.build_here_i_am_command
        real_required_budget = provision.assignment_required_budget_ms
        real_qualification_timeout = provision._qualification_timeout_s
        expected_budget_ms = host_operation_policy.assignment_required_budget_ms(
            host_operation_policy.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            expected_anchors,
            deepest_hop,
        )

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        def capture_command(builder: object, kwargs: dict[str, object]) -> object:
            command = builder(**kwargs)
            built_commands.append((dict(kwargs), command))
            return command

        def capture_assignment(**kwargs: object) -> object:
            return capture_command(real_build_assignment, kwargs)

        def capture_here_i_am(**kwargs: object) -> object:
            return capture_command(real_build_here_i_am, kwargs)

        def capture_timeout(
            requested_timeout_s: float,
            effective_budget_ms: int,
        ) -> QualificationTimeoutProbe:
            probe = QualificationTimeoutProbe(
                real_qualification_timeout(
                    requested_timeout_s,
                    effective_budget_ms,
                ),
                effective_budget_ms,
            )
            timeout_probes.append(probe)
            return probe

        with (
            mock.patch.object(provision, "BleakClient", FakeBleakClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "decode_gateway_command_event", fake_decode),
            mock.patch.object(
                provision,
                "assignment_required_budget_ms",
                wraps=real_required_budget,
            ) as required_budget,
            mock.patch.object(
                provision,
                "build_assign_discovery_slots_command",
                side_effect=capture_assignment,
            ),
            mock.patch.object(
                provision,
                "build_here_i_am_command",
                side_effect=capture_here_i_am,
            ),
            mock.patch.object(
                provision,
                "_qualification_timeout_s",
                side_effect=capture_timeout,
            ),
            mock.patch.object(
                provision,
                "_new_identity",
                side_effect=[0x11111, 0x22222],
            ),
            mock.patch("builtins.print"),
        ):
            qualification = await provision.run(
                args(
                    command="qualify-reachability",
                    require_survey_success=False,
                    notification_hold_s=0.0,
                    expected_anchors=expected_anchors,
                    expected_direct_anchors=1,
                    expected_multihop_anchors=2,
                    deepest_hop=deepest_hop,
                )
            )

        self.assertIsInstance(qualification, provision.AssignmentQualification)
        required_budget.assert_called_once_with(
            host_operation_policy.ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            expected_anchors,
            deepest_hop=deepest_hop,
        )
        self.assertEqual(2, len(built_commands))
        policies = []
        for command_args, command in built_commands:
            policies.append(command_args["operation_policy"])
            self.assertEqual(expected_budget_ms, command_args["command_budget_ms"])
            self.assertEqual(
                expected_budget_ms,
                command.packet.value(host_protocol.TLV_COMMAND_BUDGET_MS),
            )
        self.assertEqual(policies[0], policies[1])
        self.assertTrue(policies[0].assignment.ram_only_iteration)
        self.assertEqual(
            expected_budget_ms,
            policies[0].assignment.operation_budget_ms,
        )
        self.assertEqual(deepest_hop, policies[0].assignment.deepest_hop)
        self.assertEqual(
            [expected_budget_ms, expected_budget_ms],
            [
                probe.effective_budget_ms
                for probe in timeout_probes
                if probe.deadline_origins
            ],
        )

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
        FakeBleakClient.notification_count = 0
        FakeBleakClient.write_notification_counts = [
            len(route_events),
            len(assignment_events),
        ]

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

    async def test_completed_command_reconnects_same_gateway_without_resend(
        self,
    ) -> None:
        events = successful_assignment_events(
            3,
            session_id=0x12345,
            direct_count=1,
        )

        class ReconnectDecoder:
            def feed(self, raw: bytes) -> object:
                packet = types.SimpleNamespace(
                    msg_type=provision.MSG_GATEWAY_COMMAND_EVENT,
                    src_id=1,
                    dst_id=GATEWAY_ID,
                    session_id=0x12345,
                    seq=raw[0] + 1,
                    payload=raw,
                )
                return types.SimpleNamespace(errors=[], packets=[packet])

        class ReconnectingClient(FakeBleakClient):
            notify_starts = 0
            command_written = False

            async def start_notify(
                self, _uuid: object, callback: object
            ) -> None:
                type(self).notify_starts += 1
                self.notify_enabled = True
                self.notify_callback = callback
                self.operations.append(("start_notify", None))
                if type(self).notify_starts == 2:
                    for index in range(len(events)):
                        callback(None, bytearray((index,)))

            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                await super().write_gatt_char(
                    characteristic,
                    data,
                    response=response,
                )
                if not type(self).command_written:
                    type(self).command_written = True
                    self.is_connected = False
                    assert self.disconnected_callback is not None
                    asyncio.get_running_loop().call_soon(
                        self.disconnected_callback,
                        self,
                    )

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        identity = mock.Mock(return_value=0x12345)
        with (
            mock.patch.object(provision, "BleakClient", ReconnectingClient),
            mock.patch.object(
                provision, "GatewayReceiveBuffer", ReconnectDecoder
            ),
            mock.patch.object(
                provision, "decode_gateway_command_event", fake_decode
            ),
            mock.patch.object(provision, "_new_identity", identity),
            mock.patch("builtins.print"),
        ):
            qualification = await provision.run(
                args(
                    command="assign-slots",
                    require_survey_success=False,
                    require_assignment_success=True,
                    notification_hold_s=0.0,
                    expected_direct_anchors=1,
                    expected_multihop_anchors=2,
                    reconnect_attempts=2,
                    reconnect_delay_s=0.0,
                )
            )

        writes = [
            value[0]
            for name, value in FakeBleakClient.operations
            if name == "write"
        ]
        assignment_commands = [
            frame
            for frame in writes
            if parse_cobs_packet(frame).value(provision.TLV_COMMAND_ID)
            == provision.CMD_ASSIGN_DISCOVERY_SLOTS
        ]
        self.assertEqual(1, len(assignment_commands))
        self.assertEqual(
            2,
            sum(name == "connect" for name, _value in FakeBleakClient.operations),
        )
        self.assertEqual(
            2,
            sum(
                name == "read_identity"
                for name, _value in FakeBleakClient.operations
            ),
        )
        identity.assert_called_once_with()
        self.assertIsInstance(qualification, provision.AssignmentQualification)
        assert isinstance(qualification, provision.AssignmentQualification)
        self.assertEqual(0x12345, qualification.correlation_id)
        self.assertEqual(0x12345, qualification.host_session_id)
        self.assertEqual(0x2345, qualification.host_sequence)

    async def test_reconnect_ignores_stale_link_cleanup_error(self) -> None:
        events = successful_assignment_events(
            3,
            session_id=0x12345,
            direct_count=1,
        )

        class CleanupErrorClient(FakeBleakClient):
            notify_starts = 0
            disconnect_calls = 0

            async def start_notify(
                self, uuid: object, callback: object
            ) -> None:
                type(self).notify_starts += 1
                await super().start_notify(uuid, callback)
                if type(self).notify_starts == 1:
                    assert self.disconnected_callback is not None
                    self.disconnected_callback(self)

            async def disconnect(self) -> None:
                type(self).disconnect_calls += 1
                if type(self).disconnect_calls == 1:
                    raise EOFError("stale BlueZ link already closed")
                await super().disconnect()

        FakeDecoder.events = events
        FakeBleakClient.write_notification_counts = [len(events)]

        def fake_decode(payload: bytes, **_kwargs: object) -> object:
            return events[payload[0]]

        with (
            mock.patch.object(provision, "BleakClient", CleanupErrorClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(
                provision, "decode_gateway_command_event", fake_decode
            ),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            qualification = await provision.run(
                args(
                    command="assign-slots",
                    require_survey_success=False,
                    require_assignment_success=True,
                    notification_hold_s=0.0,
                    expected_direct_anchors=1,
                    expected_multihop_anchors=2,
                    reconnect_attempts=1,
                    reconnect_delay_s=0.0,
                )
            )

        self.assertIsInstance(qualification, provision.AssignmentQualification)
        self.assertEqual(
            2,
            sum(
                name == "connect"
                for name, _value in FakeBleakClient.operations
            ),
        )

    async def test_reconnect_rejects_a_different_gateway_without_resend(
        self,
    ) -> None:
        class WrongGatewayClient(FakeBleakClient):
            identity_reads = 0
            command_written = False

            async def read_gatt_char(self, _uuid: object) -> bytes:
                type(self).identity_reads += 1
                self.operations.append(("read_identity", None))
                identity = (
                    GATEWAY_ID
                    if type(self).identity_reads == 1
                    else GATEWAY_ID + 1
                )
                return identity.to_bytes(8, "little")

            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                await super().write_gatt_char(
                    characteristic,
                    data,
                    response=response,
                )
                if not type(self).command_written:
                    type(self).command_written = True
                    self.is_connected = False
                    assert self.disconnected_callback is not None
                    asyncio.get_running_loop().call_soon(
                        self.disconnected_callback,
                        self,
                    )

        identity = mock.Mock(return_value=0x12345)
        with (
            mock.patch.object(provision, "BleakClient", WrongGatewayClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", identity),
            mock.patch("builtins.print"),
            self.assertRaisesRegex(RuntimeError, "identity mismatch"),
        ):
            await provision.run(
                args(
                    command="assign-slots",
                    require_survey_success=False,
                    require_assignment_success=True,
                    notification_hold_s=0.0,
                    reconnect_attempts=2,
                    reconnect_delay_s=0.0,
                )
            )

        self.assertEqual(
            1,
            sum(name == "write" for name, _value in FakeBleakClient.operations),
        )
        self.assertEqual(
            2,
            sum(name == "connect" for name, _value in FakeBleakClient.operations),
        )
        identity.assert_called_once_with()

    async def test_disconnect_during_command_write_is_fatal_and_never_retried(
        self,
    ) -> None:
        class WriteDisconnectClient(FakeBleakClient):
            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                await super().write_gatt_char(
                    characteristic,
                    data,
                    response=response,
                )
                self.is_connected = False
                assert self.disconnected_callback is not None
                self.disconnected_callback(self)
                raise EOFError("link dropped before write completion")

        with (
            mock.patch.object(provision, "BleakClient", WriteDisconnectClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
            self.assertRaisesRegex(RuntimeError, "ambiguous"),
        ):
            await provision.run(
                args(
                    command="assign-slots",
                    require_survey_success=False,
                    require_assignment_success=True,
                    notification_hold_s=0.0,
                    reconnect_attempts=3,
                    reconnect_delay_s=0.0,
                )
            )

        self.assertEqual(
            1,
            sum(name == "connect" for name, _value in FakeBleakClient.operations),
        )
        self.assertEqual(
            1,
            sum(name == "write" for name, _value in FakeBleakClient.operations),
        )

    async def test_reconnect_does_not_restart_qualification_deadline(self) -> None:
        class SlowReconnectClient(FakeBleakClient):
            command_written = False
            connects = 0

            async def connect(self) -> None:
                type(self).connects += 1
                if type(self).connects == 2:
                    await asyncio.sleep(0.045)
                await super().connect()

            async def write_gatt_char(
                self,
                characteristic: object,
                data: bytes,
                *,
                response: bool,
            ) -> None:
                await super().write_gatt_char(
                    characteristic,
                    data,
                    response=response,
                )
                if not type(self).command_written:
                    type(self).command_written = True
                    self.is_connected = False
                    assert self.disconnected_callback is not None
                    asyncio.get_running_loop().call_soon(
                        self.disconnected_callback,
                        self,
                    )

        loop = asyncio.get_running_loop()
        started = loop.time()
        with (
            mock.patch.object(provision, "BleakClient", SlowReconnectClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch.object(
                provision,
                "_qualification_timeout_s",
                side_effect=lambda requested, _budget: requested,
            ),
            mock.patch("builtins.print"),
            self.assertRaisesRegex(RuntimeError, "timed out"),
        ):
            await provision.run(
                args(
                    command="assign-slots",
                    require_survey_success=False,
                    require_assignment_success=True,
                    assignment_timeout=0.06,
                    notification_hold_s=0.0,
                    reconnect_attempts=1,
                    reconnect_delay_s=0.0,
                )
            )
        elapsed = loop.time() - started

        self.assertLess(
            elapsed,
            0.09,
            "reconnect consumed the original deadline instead of restarting it",
        )
        self.assertEqual(
            2,
            sum(name == "connect" for name, _value in FakeBleakClient.operations),
        )
        self.assertEqual(
            1,
            sum(name == "write" for name, _value in FakeBleakClient.operations),
        )

    async def test_disconnect_before_command_exhausts_bounded_reconnects(self) -> None:
        class DisconnectingClient(FakeBleakClient):
            async def start_notify(self, uuid: object, callback: object) -> None:
                await super().start_notify(uuid, callback)
                self.is_connected = False
                assert self.disconnected_callback is not None
                self.disconnected_callback(self)

        with (
            mock.patch.object(provision, "BleakClient", DisconnectingClient),
            mock.patch.object(provision, "GatewayReceiveBuffer", FakeDecoder),
            mock.patch.object(provision, "_new_identity", return_value=0x12345),
            mock.patch("builtins.print"),
        ):
            with self.assertRaisesRegex(RuntimeError, "reconnect exhausted"):
                await provision.run(
                    args(
                        command="assign-slots",
                        require_survey_success=False,
                        require_assignment_success=True,
                        notification_hold_s=0.0,
                        reconnect_attempts=1,
                        reconnect_delay_s=0.0,
                    )
                )

    async def test_monitor_fails_if_reconnect_cannot_finish_before_deadline(
        self,
    ) -> None:
        class DisconnectingClient(FakeBleakClient):
            async def start_notify(self, uuid: object, callback: object) -> None:
                await super().start_notify(uuid, callback)
                self.is_connected = False
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
                        duration=0.001,
                        require_survey_success=False,
                        notification_hold_s=0.0,
                        reconnect_attempts=1,
                        reconnect_delay_s=0.01,
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
