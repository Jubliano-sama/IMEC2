from __future__ import annotations

import hashlib
import unittest
from typing import Any

from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    DiscoveryOperationPolicy,
    OperationPolicyProfile,
    PairOperationPolicy,
)
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    CMD_SURVEY_REACHABILITY,
    DEFAULT_HOST_ID,
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    FLAG_COUNT_AS_CLICK,
    FLAG_DIAGNOSTIC,
    FLAG_ERROR,
    FLAG_GATEWAY_ACK_REQUIRED,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN,
    GATEWAY_HOST_RECEIPT_TLV_LEN,
    GATEWAY_STREAM_MAGIC,
    GATEWAY_STREAM_FLAG_TRUNCATED,
    GATEWAY_STREAM_RECORD_HEADER_LEN,
    GATEWAY_STREAM_RECORD_MAX_LEN,
    GATEWAY_STREAM_RECORD_PACKET,
    GATEWAY_STREAM_VERSION,
    GatewayReceiveBuffer,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_GATEWAY_HOST_RECEIPT,
    MSG_MESH_DATA,
    MSG_SELF_TEST_REPORT,
    MSG_SURVEY_DISCOVERY_REPORT,
    PACKET_EXT_MAX_PAYLOAD_LEN,
    SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    TLV_ANCHOR_ID,
    TLV_BURST_ID,
    TLV_CLICKER_ID,
    TLV_CLICKER_CLOCK_OFFSET_RAW,
    TLV_COMMAND_ID,
    TLV_COMMAND_BUDGET_MS,
    TLV_COMMAND_STATUS,
    TLV_DIAG_STATUS_FLAGS,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_DETECTION_SOURCE,
    TLV_DISCOVERY_SLOT_COUNT,
    TLV_DISCOVERY_ASSIGNMENT_EPOCH,
    TLV_DISCOVERY_ASSIGNMENT_HASH,
    TLV_DISCOVERY_ASSIGNMENT_PHASE,
    TLV_DISCOVERY_ASSIGNMENT_TABLE,
    TLV_EXPECTED_NODE_COUNT,
    TLV_OPERATION_POLICY,
    TLV_NODE_BOOT_COUNTER,
    TLV_DISTANCE_MM,
    TLV_DISTANCE_SAMPLES_MM,
    TLV_DURATION_MS,
    TLV_EVENT_SEQ,
    TLV_GATEWAY_HOST_RECEIPT_IDENTITY,
    TLV_QUALITY,
    TLV_RANGE_STATUS,
    TLV_REASON,
    TLV_REACHABILITY_ENTRY,
    TLV_RANGE_ROUND_INDICES,
    TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX,
    TLV_SEQUENCE_START_TIMESTAMPS_MS,
    TLV_SURVEY_ID,
    TLV_SURVEY_OPERATION_GENERATION,
    TLV_TIMESTAMP_MS,
    TLV_ATTEMPT_INDEX,
    TLV_UWB_CIR_SAMPLE,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_FULL_CHUNK,
    TLV_UWB_CIR_START_INDEX,
    TLV_UWB_CIR_TOTAL_BYTES,
    TLV_UWB_CLOCK_OFFSET_RAW,
    DecodeError,
    GatewayHostReceiptIdentity,
    append_tlv,
    build_anchor_discovery_command,
    build_survey_abort_command,
    build_assign_discovery_slots_command,
    build_here_i_am_command,
    build_gateway_host_receipt,
    click_report_session_id,
    click_samples,
    crc16_ccitt_false,
    decode_gateway_host_receipt_identity,
    decode_cir_sample,
    decode_gateway_identity,
    encode_cobs_packet,
    encode_gateway_host_receipt_identity,
    parse_cobs_packet,
    parse_gateway_host_receipt,
    parse_stream_record,
    parse_tlvs,
    validate_click_payload,
    validate_gateway_local_command_result_packet,
    validate_self_test_report_packet,
    validate_survey_discovery_report,
)


def tlv(type_id: int, value: bytes) -> bytes:
    return bytes((type_id, len(value))) + value


def click_payload() -> bytes:
    payload = bytearray()
    append_tlv(payload, TLV_CLICKER_ID, 0x1111222233334444.to_bytes(8, "little"))
    append_tlv(payload, TLV_ANCHOR_ID, 0x5555666677778888.to_bytes(8, "little"))
    append_tlv(payload, TLV_EVENT_SEQ, (0x11223344).to_bytes(4, "little"))
    append_tlv(payload, TLV_TIMESTAMP_MS, (1_234_567).to_bytes(8, "little"))
    append_tlv(payload, TLV_DISTANCE_MM, (4512).to_bytes(4, "little", signed=True))
    append_tlv(payload, TLV_QUALITY, b"\x5a")
    append_tlv(payload, TLV_RANGE_STATUS, b"\x00")
    append_tlv(payload, TLV_SAMPLE_COUNT, (5).to_bytes(2, "little"))
    append_tlv(payload, TLV_SAMPLE_INDEX, (2).to_bytes(2, "little"))
    append_tlv(
        payload,
        TLV_DISTANCE_SAMPLES_MM,
        b"".join(value.to_bytes(4, "little", signed=True) for value in (4500, 4510, 4520)),
    )
    append_tlv(payload, TLV_RANGE_ROUND_INDICES, bytes((7, 8, 9)))
    append_tlv(
        payload,
        TLV_SEQUENCE_START_TIMESTAMPS_MS,
        b"".join(value.to_bytes(8, "little") for value in (10_000, 10_050, 10_100)),
    )
    append_tlv(payload, TLV_DIAG_STATUS_FLAGS, (0x45).to_bytes(4, "little"))
    append_tlv(payload, TLV_BURST_ID, (0xABCDEF01).to_bytes(4, "little"))
    append_tlv(payload, TLV_UWB_CIR_SAMPLE, b"\x01\x02\x03\xfe\xff\xff")
    append_tlv(payload, 0xFE, b"\xaa\xbb")
    append_tlv(payload, 0xFE, b"\xcc")
    return bytes(payload)


def survey_discovery_payload(
    *,
    anchor_id: int = 0x5555666677778888,
    survey_id: int = 0xAABBCCDD,
    operation_generation: int = 0x1234567887654321,
    boot_incarnation: int = 0x10203040,
    entries: tuple[tuple[int, int, int], ...] = (
        (0x1111222233334444, -61, 82),
        (0x2222333344445555, -72, 63),
    ),
    command_status: int = 0,
) -> bytes:
    payload = bytearray()
    append_tlv(payload, TLV_SURVEY_ID, survey_id.to_bytes(4, "little"))
    append_tlv(payload, TLV_ANCHOR_ID, anchor_id.to_bytes(8, "little"))
    for peer_id, rssi_dbm, quality in entries:
        append_tlv(
            payload,
            TLV_REACHABILITY_ENTRY,
            peer_id.to_bytes(8, "little")
            + rssi_dbm.to_bytes(1, "little", signed=True)
            + quality.to_bytes(1, "little"),
        )
    append_tlv(
        payload,
        TLV_SURVEY_OPERATION_GENERATION,
        operation_generation.to_bytes(8, "little"),
    )
    append_tlv(
        payload,
        TLV_NODE_BOOT_COUNTER,
        boot_incarnation.to_bytes(4, "little"),
    )
    append_tlv(
        payload,
        TLV_COMMAND_STATUS,
        command_status.to_bytes(2, "little"),
    )
    return bytes(payload)


def replace_tlv(payload: bytes, type_id: int, value: bytes, *, occurrence: int = 0) -> bytes:
    out = bytearray()
    index = 0
    seen = 0
    while index < len(payload):
        current_type = payload[index]
        value_len = payload[index + 1]
        raw = payload[index + 2:index + 2 + value_len]
        if current_type == type_id and seen == occurrence:
            append_tlv(out, type_id, value)
            seen += 1
        else:
            out.extend((current_type, value_len))
            out.extend(raw)
            if current_type == type_id:
                seen += 1
        index += 2 + value_len
    if seen <= occurrence:
        raise AssertionError(f"TLV 0x{type_id:02x} occurrence {occurrence} was not found")
    return bytes(out)


def remove_tlv(payload: bytes, type_id: int, *, occurrence: int = 0) -> bytes:
    out = bytearray()
    index = 0
    seen = 0
    removed = False
    while index < len(payload):
        current_type = payload[index]
        value_len = payload[index + 1]
        raw = payload[index + 2:index + 2 + value_len]
        if current_type == type_id and seen == occurrence:
            removed = True
            seen += 1
        else:
            out.extend((current_type, value_len))
            out.extend(raw)
            if current_type == type_id:
                seen += 1
        index += 2 + value_len
    if not removed:
        raise AssertionError(f"TLV 0x{type_id:02x} occurrence {occurrence} was not found")
    return bytes(out)


def diagnostic_range_payload() -> bytes:
    payload = bytearray()
    append_tlv(payload, TLV_CLICKER_ID, 0x1111222233334444.to_bytes(8, "little"))
    append_tlv(payload, TLV_ANCHOR_ID, 0x5555666677778888.to_bytes(8, "little"))
    append_tlv(payload, TLV_EVENT_SEQ, (0x11223344).to_bytes(4, "little"))
    append_tlv(payload, TLV_TIMESTAMP_MS, (1_234_567).to_bytes(8, "little"))
    append_tlv(payload, TLV_DISTANCE_MM, (4512).to_bytes(4, "little", signed=True))
    append_tlv(payload, TLV_QUALITY, b"\x5a")
    append_tlv(payload, TLV_RANGE_STATUS, b"\x00")
    return bytes(payload)


def cir_payload(fragment_index: int, byte_offset: int, chunks: tuple[bytes, ...]) -> bytes:
    payload = bytearray()
    append_tlv(payload, TLV_CLICKER_ID, 0x1111222233334444.to_bytes(8, "little"))
    append_tlv(payload, TLV_ANCHOR_ID, 0x5555666677778888.to_bytes(8, "little"))
    append_tlv(payload, TLV_EVENT_SEQ, (0x11223344).to_bytes(4, "little"))
    append_tlv(payload, TLV_TIMESTAMP_MS, (1_234_567).to_bytes(8, "little"))
    append_tlv(payload, TLV_DIAG_FRAGMENT_INDEX, fragment_index.to_bytes(2, "little"))
    append_tlv(payload, TLV_DIAG_FRAGMENT_COUNT, (2).to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_BYTE_OFFSET, byte_offset.to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_TOTAL_BYTES, (1152).to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_FIRST_PATH_INDEX, (17).to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_START_INDEX, (23).to_bytes(2, "little"))
    for chunk in chunks:
        append_tlv(payload, TLV_UWB_CIR_FULL_CHUNK, chunk)
    return bytes(payload)


def stream_record(
    payload: bytes,
    *,
    msg_type: int = MSG_CLICK_REPORT,
    packet_flags: int = 0x24,
    stream_flags: int = 0,
    packet_session_id: int | None = None,
    packet_src_id: int = 0x5555666677778888,
    packet_dst_id: int = 0x9999AAAABBBBCCCC,
    packet_seq: int = 0x1234,
) -> bytes:
    record = bytearray(GATEWAY_STREAM_RECORD_HEADER_LEN)
    record[0:2] = GATEWAY_STREAM_MAGIC.to_bytes(2, "little")
    record[2] = GATEWAY_STREAM_VERSION
    record[3] = GATEWAY_STREAM_RECORD_HEADER_LEN
    record[4] = GATEWAY_STREAM_RECORD_PACKET
    record[5] = 1
    record[6] = 0
    record[7] = stream_flags
    record[8] = msg_type
    record[9] = packet_flags
    record[10:12] = packet_seq.to_bytes(2, "little")
    if packet_session_id is None:
        packet_session_id = click_report_session_id(
            0x1111222233334444, 0x11223344
        )
    record[12:16] = packet_session_id.to_bytes(4, "little")
    record[16:24] = packet_src_id.to_bytes(8, "little")
    record[24:32] = packet_dst_id.to_bytes(8, "little")
    record[32:36] = (17).to_bytes(4, "little")
    record[36:38] = len(payload).to_bytes(2, "little")
    record[38:40] = crc16_ccitt_false(payload).to_bytes(2, "little")
    record.extend(payload)
    return bytes(record)


def gateway_assignment_event_payload(
    *,
    event_sequence: int = 0x10203040,
    stage: int = 6,
    flags: int = 0,
    anchor_id: int = 0x5555666677778888,
    discovery_slot: int = 4,
    progress_count: int = 1,
    total_count: int = 1,
    success_count: int = 1,
    failure_count: int = 0,
) -> bytes:
    raw = bytearray(78)
    raw[0:8] = bytes((1, 78, 1, stage, flags, 0, 0, 0))
    raw[8:10] = CMD_ASSIGN_DISCOVERY_SLOTS.to_bytes(2, "little")
    raw[10:12] = (17).to_bytes(2, "little")
    raw[12:16] = (0x55667788).to_bytes(4, "little")
    raw[16:20] = (0x12345678).to_bytes(4, "little")
    raw[20:24] = (0x55667788).to_bytes(4, "little")
    raw[24:26] = (0x1234).to_bytes(2, "little")
    raw[28:32] = event_sequence.to_bytes(4, "little")
    raw[32:40] = anchor_id.to_bytes(8, "little")
    raw[64:66] = progress_count.to_bytes(2, "little")
    raw[66:68] = total_count.to_bytes(2, "little")
    raw[68:70] = success_count.to_bytes(2, "little")
    raw[70:72] = failure_count.to_bytes(2, "little")
    raw[77] = discovery_slot
    return bytes(raw)


def gateway_local_command_result_payload(
    *, command_id: int = CMD_ASSIGN_DISCOVERY_SLOTS, status: int = 0, reason: int = 0
) -> bytes:
    return (
        bytes((TLV_COMMAND_ID, 2))
        + command_id.to_bytes(2, "little")
        + bytes((TLV_COMMAND_STATUS, 2))
        + status.to_bytes(2, "little")
        + bytes((TLV_REASON, 1, reason))
    )


def synthetic_truncated_payload() -> bytes:
    payload = bytearray()
    for type_id, value_len in (
        (0x59, 4),
        (0x5A, 2),
        (0x5B, 4),
        (0x5C, 8),
        (0x5D, 8),
        (0x5E, 4),
        (0x97, 4),
        (0x98, 8),
        (0x99, 1),
        (0x9A, 2),
    ):
        append_tlv(payload, type_id, bytes((type_id,)) * value_len)
    append_tlv(payload, 0x05, bytes(range(15)))
    assert len(payload) == 82
    payload.extend((0x61, 0xFF))
    payload.extend(b"\xa5" * (255 - len(payload)))
    assert len(payload) == 255
    return bytes(payload)


def self_test_report_payload(
    *, clicker_id: int, event_seq: int, failure: int = 0, battery_mv: int = 3000
) -> bytes:
    return b"".join(
        (
            tlv(TLV_CLICKER_ID, clicker_id.to_bytes(8, "little")),
            tlv(TLV_EVENT_SEQ, event_seq.to_bytes(4, "little")),
            tlv(0x04, failure.to_bytes(2, "little")),
            tlv(0x02, battery_mv.to_bytes(2, "little")),
        )
    )


def extended_stream_payload() -> bytes:
    payload = bytearray()
    for chunk_len in (255, 255, 255, 125):
        append_tlv(payload, TLV_UWB_CIR_FULL_CHUNK, b"\xa5" * chunk_len)
    for type_id in (0xE0, 0xE1, 0xE2):
        append_tlv(payload, type_id, bytes((type_id,)) * 18)
    assert len(payload) == PACKET_EXT_MAX_PAYLOAD_LEN
    return bytes(payload)


class ProtocolTests(unittest.TestCase):
    def test_click_report_transport_session_matches_firmware_vectors(self) -> None:
        self.assertEqual(
            click_report_session_id(0x1111222233334444, 0x11223344),
            0x1BCF6CE5,
        )
        first = click_report_session_id(0xAAAABBBBCCCCDDDD, 7)
        second = click_report_session_id(0xAAAABBBBCCCCDDDE, 7)
        self.assertEqual(first, 0xB0CAC892)
        self.assertEqual(second, 0x598766A9)
        self.assertNotEqual(first, second)
        self.assertEqual(first, click_report_session_id(0xAAAABBBBCCCCDDDD, 7))
        self.assertNotEqual(first, 0)
        self.assertNotEqual(second, 0)

    def test_cobs_packet_round_trip_decodes_envelope_and_all_tlvs(self) -> None:
        payload = click_payload()
        frame = encode_cobs_packet(
            msg_type=MSG_CLICK_REPORT,
            flags=0x24,
            src_id=0x5555666677778888,
            dst_id=0x9999AAAABBBBCCCC,
            session_id=0x11223344,
            seq=0x1234,
            ttl=4,
            payload=payload,
            message_age_ms=81,
        )

        packet = parse_cobs_packet(frame)

        self.assertEqual(packet.transport, "cobs-shared-packet")
        self.assertEqual(packet.raw_transport, frame)
        self.assertIsNotNone(packet.raw_packet)
        self.assertEqual(packet.msg_type, MSG_CLICK_REPORT)
        self.assertEqual(packet.src_id, 0x5555666677778888)
        self.assertEqual(packet.dst_id, 0x9999AAAABBBBCCCC)
        self.assertEqual(packet.session_id, 0x11223344)
        self.assertEqual(packet.seq, 0x1234)
        self.assertEqual(packet.ttl, 4)
        self.assertEqual(packet.age_ms, 81)
        self.assertEqual(packet.value(TLV_DISTANCE_MM), 4512)
        self.assertEqual(packet.value(TLV_DIAG_STATUS_FLAGS), 0x45)
        unknown = [value for value in packet.tlvs if value.type_id == 0xFE]
        self.assertEqual([value.raw for value in unknown], [b"\xaa\xbb", b"\xcc"])
        self.assertTrue(all(not value.known for value in unknown))

        samples, warnings = click_samples(packet)
        self.assertEqual(warnings, [])
        self.assertEqual([sample.sample_index for sample in samples], [2, 3, 4])
        self.assertEqual([sample.distance_mm for sample in samples], [4500, 4510, 4520])
        self.assertEqual([sample.round_index for sample in samples], [7, 8, 9])
        self.assertEqual([sample.timestamp_ms for sample in samples], [10_000, 10_050, 10_100])

    def test_cobs_packet_rejects_crc_corruption(self) -> None:
        frame = bytearray(
            encode_cobs_packet(
                msg_type=MSG_CLICK_REPORT,
                flags=0,
                src_id=0x5555666677778888,
                dst_id=2,
                session_id=3,
                seq=4,
                ttl=4,
                payload=tlv(TLV_EVENT_SEQ, (1).to_bytes(4, "little")),
            )
        )
        frame[-3] ^= 0x40
        with self.assertRaisesRegex(DecodeError, "CRC"):
            parse_cobs_packet(bytes(frame))

    def test_tlv_parser_rejects_structural_overrun_but_keeps_bad_scalar_visible(self) -> None:
        with self.assertRaisesRegex(DecodeError, "overruns payload"):
            parse_tlvs(bytes((TLV_EVENT_SEQ, 4, 1, 2)))
        values = parse_tlvs(tlv(TLV_EVENT_SEQ, b"\x01\x02"))
        self.assertEqual(len(values), 1)
        self.assertIn("expected 4 bytes", values[0].decode_error or "")

    def test_gateway_stream_record_and_fragmented_receive_buffer(self) -> None:
        record = stream_record(click_payload())
        packet = parse_stream_record(record)
        self.assertEqual(packet.transport, "gateway-stream-v1")
        self.assertIsNone(packet.raw_packet)
        self.assertIsNone(packet.ttl)
        self.assertEqual(packet.age_kind, "gateway_queue_age_ms")
        self.assertEqual(packet.age_ms, 17)
        self.assertEqual(packet.stream_class, 1)
        self.assertEqual(packet.value(TLV_BURST_ID), 0xABCDEF01)

        decoder = GatewayReceiveBuffer()
        first = decoder.feed(record[:9])
        second = decoder.feed(record[9:63])
        third = decoder.feed(record[63:])
        self.assertEqual(first.packets, ())
        self.assertEqual(second.packets, ())
        self.assertEqual(third.errors, ())
        self.assertEqual(len(third.packets), 1)
        self.assertEqual(third.packets[0].seq, 0x1234)

    def test_receive_buffer_admits_only_canonical_gateway_command_events(self) -> None:
        gateway_id = 0x9999AAAABBBBCCCC
        event_sequence = 0x10203040
        payload = gateway_assignment_event_payload(event_sequence=event_sequence)
        record = stream_record(
            payload,
            msg_type=MSG_GATEWAY_COMMAND_EVENT,
            packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
            packet_src_id=gateway_id,
            packet_dst_id=gateway_id,
            packet_session_id=event_sequence,
            packet_seq=event_sequence & 0xFFFF,
        )
        accepted = GatewayReceiveBuffer().feed(record)
        self.assertEqual(accepted.errors, ())
        self.assertEqual(len(accepted.packets), 1)
        self.assertEqual(accepted.packets[0].msg_type, MSG_GATEWAY_COMMAND_EVENT)
        receipt = build_gateway_host_receipt(
            accepted.packets[0],
            host_id=0xA1C1BEEFC0DE0001,
            gateway_id=gateway_id,
        )
        self.assertEqual(receipt.identity.src_id, gateway_id)
        self.assertEqual(receipt.identity.dst_id, gateway_id)
        self.assertEqual(
            receipt.identity.original_flags, FLAG_GATEWAY_ACK_REQUIRED
        )

        # Generic pre-commit claim progress deliberately has no durable
        # assignment identity and no outer ACK flag. It remains visible but
        # cannot acquire publisher/host-receipt custody.
        precommit_payload = gateway_assignment_event_payload(
            event_sequence=0x11223344,
            discovery_slot=0xFF,
            progress_count=1,
            total_count=0,
            success_count=0,
            failure_count=0,
        )
        precommit = GatewayReceiveBuffer().feed(
            stream_record(
                precommit_payload,
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=0,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x11223344,
                packet_seq=0x3344,
            )
        )
        self.assertEqual(precommit.errors, ())
        self.assertEqual(len(precommit.packets), 1)
        with self.assertRaises(ValueError):
            build_gateway_host_receipt(
                precommit.packets[0],
                host_id=0xA1C1BEEFC0DE0001,
                gateway_id=gateway_id,
            )

        malformed_records = (
            stream_record(
                payload,
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=1,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_sequence,
                packet_seq=event_sequence & 0xFFFF,
            ),
            stream_record(
                payload,
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id + 1,
                packet_dst_id=gateway_id,
                packet_session_id=event_sequence,
                packet_seq=event_sequence & 0xFFFF,
            ),
            stream_record(
                payload,
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_sequence + 1,
                packet_seq=event_sequence & 0xFFFF,
            ),
            stream_record(
                gateway_assignment_event_payload(
                    event_sequence=0x11223344,
                    discovery_slot=0xFF,
                ),
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x11223344,
                packet_seq=0x3344,
            ),
            stream_record(
                payload[:-1],
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_sequence,
                packet_seq=event_sequence & 0xFFFF,
            ),
        )
        for record in malformed_records:
            with self.subTest(record=record[:16]):
                rejected = GatewayReceiveBuffer().feed(record)
                self.assertEqual(rejected.packets, ())
                self.assertEqual(len(rejected.errors), 1)
                self.assertIn("decode failed", rejected.errors[0])

    def test_gateway_local_command_result_receipt_boundary(self) -> None:
        gateway_id = 0x9999AAAABBBBCCCC
        local = GatewayReceiveBuffer().feed(
            stream_record(
                gateway_local_command_result_payload(),
                msg_type=MSG_COMMAND_RESULT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x10203040,
                packet_seq=0x3040,
            )
        )
        self.assertEqual(local.errors, ())
        self.assertEqual(len(local.packets), 1)
        validate_gateway_local_command_result_packet(local.packets[0])
        local_receipt = build_gateway_host_receipt(
            local.packets[0],
            host_id=0xA1C1BEEFC0DE0001,
            gateway_id=gateway_id,
        )
        self.assertEqual(local_receipt.identity.original_msg_type, MSG_COMMAND_RESULT)
        self.assertEqual(
            local_receipt.identity.original_flags, FLAG_GATEWAY_ACK_REQUIRED
        )

        error_local = parse_stream_record(
            stream_record(
                gateway_local_command_result_payload(status=5),
                msg_type=MSG_COMMAND_RESULT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_ERROR,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x10203041,
                packet_seq=0x3041,
            )
        )
        validate_gateway_local_command_result_packet(error_local)

        # The same result type is valid for a mesh producer only with distinct
        # endpoints and ACK-required custody; it must not be coerced local.
        mesh_result = parse_stream_record(
            stream_record(
                gateway_local_command_result_payload(),
                msg_type=MSG_COMMAND_RESULT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=0x1111222233334444,
                packet_dst_id=gateway_id,
                packet_session_id=0x10203042,
                packet_seq=0x3042,
            )
        )
        mesh_receipt = build_gateway_host_receipt(
            mesh_result,
            host_id=0xA1C1BEEFC0DE0001,
            gateway_id=gateway_id,
        )
        self.assertEqual(mesh_receipt.identity.src_id, mesh_result.src_id)
        self.assertNotEqual(mesh_receipt.identity.src_id, mesh_receipt.identity.dst_id)

        malformed_cases = (
            stream_record(
                gateway_local_command_result_payload(),
                msg_type=MSG_COMMAND_RESULT,
                packet_flags=0,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x10203043,
                packet_seq=0x3043,
            ),
            stream_record(
                bytes((TLV_REASON, 2, 0, 0))
                + gateway_local_command_result_payload()[4:],
                msg_type=MSG_COMMAND_RESULT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x10203044,
                packet_seq=0x3044,
            ),
            stream_record(
                gateway_local_command_result_payload(status=5),
                msg_type=MSG_COMMAND_RESULT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=0x10203045,
                packet_seq=0x3045,
            ),
        )
        for record in malformed_cases:
            with self.subTest(record=record[:16]):
                rejected = GatewayReceiveBuffer().feed(record)
                self.assertEqual(rejected.packets, ())
                self.assertEqual(len(rejected.errors), 1)

    def test_self_test_report_is_validated_before_live_host_receipt(self) -> None:
        clicker_id = 0xA2603D21D805AE52
        gateway_id = 0x9999888877776666
        event_seq = 0x01020304
        payload = self_test_report_payload(
            clicker_id=clicker_id, event_seq=event_seq
        )
        record = stream_record(
            payload,
            msg_type=MSG_SELF_TEST_REPORT,
            packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            packet_src_id=clicker_id,
            packet_dst_id=gateway_id,
            packet_session_id=event_seq,
            packet_seq=event_seq & 0xFFFF,
        )
        accepted = GatewayReceiveBuffer().feed(record)
        self.assertEqual(accepted.errors, ())
        self.assertEqual(len(accepted.packets), 1)
        validate_self_test_report_packet(accepted.packets[0])
        receipt = build_gateway_host_receipt(
            accepted.packets[0],
            host_id=DEFAULT_HOST_ID,
            gateway_id=gateway_id,
        )
        self.assertEqual(receipt.identity.original_msg_type, MSG_SELF_TEST_REPORT)

        malformed = (
            stream_record(
                payload,
                msg_type=MSG_SELF_TEST_REPORT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=clicker_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_seq,
                packet_seq=event_seq & 0xFFFF,
            ),
            stream_record(
                self_test_report_payload(
                    clicker_id=clicker_id, event_seq=event_seq, failure=7
                ),
                msg_type=MSG_SELF_TEST_REPORT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
                packet_src_id=clicker_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_seq,
                packet_seq=event_seq & 0xFFFF,
            ),
            stream_record(
                payload + tlv(0x02, (3000).to_bytes(2, "little")),
                msg_type=MSG_SELF_TEST_REPORT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
                packet_src_id=clicker_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_seq,
                packet_seq=event_seq & 0xFFFF,
            ),
        )
        for candidate in malformed:
            with self.subTest(candidate=candidate[:16]):
                rejected = GatewayReceiveBuffer().feed(candidate)
                self.assertEqual(rejected.packets, ())
                self.assertEqual(len(rejected.errors), 1)

    def test_receive_buffer_resynchronizes_suffix_before_full_stream_retry(self) -> None:
        record = stream_record(click_payload())
        decoder = GatewayReceiveBuffer()

        # Model a dropped ATT prefix: the first notification leaves only the
        # record suffix in the parser, then the gateway retries the full head.
        suffix = decoder.feed(record[17:])
        self.assertEqual(suffix.packets, ())

        retry = decoder.feed(record)
        self.assertEqual(retry.errors, ())
        self.assertEqual(len(retry.packets), 1)
        self.assertEqual(retry.packets[0].transport, "gateway-stream-v1")
        self.assertEqual(retry.packets[0].seq, 0x1234)

    def test_receive_buffer_discards_corrupt_cobs_before_valid_legacy_frame(self) -> None:
        report_session_id = click_report_session_id(
            0x1111222233334444, 0x11223344
        )
        corrupt = bytearray(
            encode_cobs_packet(
                msg_type=MSG_CLICK_REPORT,
                flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
                src_id=0x5555666677778888,
                dst_id=2,
                session_id=report_session_id,
                seq=4,
                ttl=4,
                payload=click_payload(),
            )
        )
        # Corrupt the encoded CRC without introducing an extra zero delimiter;
        # that keeps this regression about one bad legacy frame followed by a
        # valid one, rather than two syntactically separate corrupt frames.
        corrupt[-2] = 2 if corrupt[-2] == 1 else corrupt[-2] ^ 1
        valid = encode_cobs_packet(
            msg_type=MSG_CLICK_REPORT,
            flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
            src_id=0x5555666677778888,
            dst_id=2,
            session_id=report_session_id,
            seq=5,
            ttl=4,
            payload=click_payload(),
        )

        result = GatewayReceiveBuffer().feed(bytes(corrupt) + valid)

        self.assertEqual(len(result.errors), 1)
        self.assertEqual(len(result.packets), 1)
        self.assertEqual(result.packets[0].seq, 5)

    def test_click_semantics_accept_normal_and_diagnostic_range_reports(self) -> None:
        normal = parse_stream_record(stream_record(click_payload()))
        validate_click_payload(normal)

        diagnostic = parse_stream_record(
            stream_record(
                diagnostic_range_payload(),
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            )
        )
        validate_click_payload(diagnostic)

    def test_click_semantics_accept_repeated_cir_chunks_across_fragments(self) -> None:
        first_chunks = (b"a" * 255, b"b" * 255, b"c" * 255, b"d" * 116)
        second_chunks = (b"e" * 255, b"f" * 16)
        first = parse_stream_record(
            stream_record(
                cir_payload(0, 0, first_chunks),
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            )
        )
        second = parse_stream_record(
            stream_record(
                cir_payload(1, 881, second_chunks),
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            )
        )
        validate_click_payload(first)
        validate_click_payload(second)
        received = GatewayReceiveBuffer().feed(
            stream_record(
                cir_payload(0, 0, first_chunks),
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            )
            + stream_record(
                cir_payload(1, 881, second_chunks),
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            )
        )
        self.assertEqual(received.errors, ())
        self.assertEqual(len(received.packets), 2)

    def test_receive_buffer_rejects_malformed_click_but_direct_decode_remains_available(self) -> None:
        cases = {
            "missing burst": remove_tlv(click_payload(), TLV_BURST_ID),
            "mismatched identity": replace_tlv(
                click_payload(), TLV_ANCHOR_ID, (0x1234).to_bytes(8, "little")
            ),
            "duplicate singleton": click_payload() + tlv(TLV_QUALITY, b"\x5a"),
            "unaligned sample arrays": replace_tlv(
                click_payload(), TLV_RANGE_ROUND_INDICES, b"\x07\x08"
            ),
            "sample bounds": replace_tlv(click_payload(), TLV_SAMPLE_INDEX, (5).to_bytes(2, "little")),
        }
        for label, payload in cases.items():
            with self.subTest(label=label):
                record = stream_record(payload)
                # Direct envelope decoding is intentionally syntax-only.
                decoded = parse_stream_record(record)
                self.assertEqual(decoded.msg_type, MSG_CLICK_REPORT)
                received = GatewayReceiveBuffer().feed(record)
                self.assertEqual(received.packets, ())
                self.assertEqual(len(received.errors), 1)
                self.assertIn("malformed click report", received.errors[0])

    def test_click_semantics_reject_ack_mode_and_detection_contract_violations(self) -> None:
        payload = click_payload()
        for label, packet_flags, mutation in (
            ("missing ACK", FLAG_COUNT_AS_CLICK, payload),
            (
                "unrelated route flag",
                FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK | 0x02,
                payload,
            ),
            (
                "both modes",
                FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC,
                payload,
            ),
            (
                "partial detection pair",
                FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
                payload + tlv(0xA9, b"\x01"),
            ),
        ):
            with self.subTest(label=label):
                packet = parse_stream_record(stream_record(mutation, packet_flags=packet_flags))
                with self.assertRaisesRegex(DecodeError, "malformed click report"):
                    validate_click_payload(packet)

    def test_survey_discovery_semantics_keep_boot_and_operation_identities_distinct(
        self,
    ) -> None:
        operation_generation = 0x1234567887654321
        records = b"".join(
            stream_record(
                survey_discovery_payload(
                    operation_generation=operation_generation,
                    boot_incarnation=boot_incarnation,
                ),
                msg_type=MSG_SURVEY_DISCOVERY_REPORT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
                packet_session_id=boot_incarnation,
                packet_seq=1,
            )
            for boot_incarnation in (41, 42)
        )

        received = GatewayReceiveBuffer().feed(records)

        self.assertEqual(received.errors, ())
        self.assertEqual(len(received.packets), 2)
        self.assertEqual(
            [packet.session_id for packet in received.packets], [41, 42]
        )
        for packet in received.packets:
            validate_survey_discovery_report(packet)
            self.assertEqual(
                packet.value(TLV_SURVEY_OPERATION_GENERATION),
                operation_generation,
            )
            self.assertEqual(packet.value(TLV_NODE_BOOT_COUNTER), packet.session_id)
            self.assertNotEqual(
                packet.session_id, operation_generation & 0xFFFFFFFF
            )

    def test_receive_buffer_rejects_noncanonical_survey_discovery_reports(
        self,
    ) -> None:
        anchor_id = 0x5555666677778888
        gateway_id = 0x9999AAAABBBBCCCC
        boot_incarnation = 0x10203040
        operation_generation = 0x1234567887654321
        valid = survey_discovery_payload(
            anchor_id=anchor_id,
            operation_generation=operation_generation,
            boot_incarnation=boot_incarnation,
        )
        duplicate_boot = valid + tlv(
            TLV_NODE_BOOT_COUNTER, boot_incarnation.to_bytes(4, "little")
        )
        invalid_peer = survey_discovery_payload(
            anchor_id=anchor_id,
            operation_generation=operation_generation,
            boot_incarnation=boot_incarnation,
            entries=((gateway_id, -61, 82),),
        )
        duplicate_peer = survey_discovery_payload(
            anchor_id=anchor_id,
            operation_generation=operation_generation,
            boot_incarnation=boot_incarnation,
            entries=((0x1111222233334444, -61, 82),) * 2,
        )
        cases = {
            "missing operation generation": (
                remove_tlv(valid, TLV_SURVEY_OPERATION_GENERATION),
                boot_incarnation,
                anchor_id,
            ),
            "duplicate boot": (duplicate_boot, boot_incarnation, anchor_id),
            "unsupported TLV": (valid + tlv(0xFE, b"\x01"), boot_incarnation, anchor_id),
            "old operation-session header": (
                valid,
                operation_generation & 0xFFFFFFFF,
                anchor_id,
            ),
            "source anchor mismatch": (valid, boot_incarnation, anchor_id + 1),
            "zero operation projection": (
                replace_tlv(
                    valid,
                    TLV_SURVEY_OPERATION_GENERATION,
                    0x1234567800000000.to_bytes(8, "little"),
                ),
                boot_incarnation,
                anchor_id,
            ),
            "invalid command status": (
                replace_tlv(valid, TLV_COMMAND_STATUS, (9).to_bytes(2, "little")),
                boot_incarnation,
                anchor_id,
            ),
            "gateway reachability peer": (
                invalid_peer,
                boot_incarnation,
                anchor_id,
            ),
            "duplicate reachability peer": (
                duplicate_peer,
                boot_incarnation,
                anchor_id,
            ),
        }
        for label, (payload, session_id, source_id) in cases.items():
            with self.subTest(label=label):
                record = stream_record(
                    payload,
                    msg_type=MSG_SURVEY_DISCOVERY_REPORT,
                    packet_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
                    packet_session_id=session_id,
                    packet_src_id=source_id,
                )
                decoded = parse_stream_record(record)
                self.assertEqual(decoded.msg_type, MSG_SURVEY_DISCOVERY_REPORT)
                with self.assertRaisesRegex(
                    DecodeError, "malformed survey discovery report"
                ):
                    validate_survey_discovery_report(decoded)
                received = GatewayReceiveBuffer().feed(record)
                self.assertEqual(received.packets, ())
                self.assertEqual(len(received.errors), 1)
                self.assertIn(
                    "malformed survey discovery report", received.errors[0]
                )

    def test_gateway_stream_accepts_extended_payload_maximum(self) -> None:
        payload = extended_stream_payload()
        record = stream_record(payload, msg_type=MSG_MESH_DATA, packet_flags=0)

        self.assertEqual(len(record), GATEWAY_STREAM_RECORD_MAX_LEN)
        packet = parse_stream_record(record)
        self.assertEqual(len(packet.payload), PACKET_EXT_MAX_PAYLOAD_LEN)
        self.assertEqual(
            [
                len(value.raw)
                for value in packet.tlvs
                if value.type_id == TLV_UWB_CIR_FULL_CHUNK
            ],
            [255, 255, 255, 125],
        )

        decoder = GatewayReceiveBuffer()
        first = decoder.feed(record[:39])
        second = decoder.feed(record[39:700])
        third = decoder.feed(record[700:])
        self.assertEqual(first.packets, ())
        self.assertEqual(second.packets, ())
        self.assertEqual(third.errors, ())
        self.assertEqual(len(third.packets), 1)
        self.assertEqual(third.packets[0].payload, payload)

        oversized = stream_record(payload + tlv(0xEF, b""))
        with self.assertRaisesRegex(DecodeError, "gateway stream record length .* exceeds maximum"):
            parse_stream_record(oversized)

    def test_marked_truncated_stream_preserves_complete_tlvs_and_partial_tail(self) -> None:
        payload = synthetic_truncated_payload()
        record = stream_record(
            payload,
            msg_type=MSG_MESH_DATA,
            packet_flags=0x10,
            stream_flags=GATEWAY_STREAM_FLAG_TRUNCATED,
        )

        packet = parse_stream_record(record)

        self.assertEqual(packet.msg_type, MSG_MESH_DATA)
        self.assertEqual(len(packet.tlvs), 12)
        self.assertTrue(all(not value.truncated for value in packet.tlvs[:-1]))
        partial = packet.tlvs[-1]
        self.assertEqual(partial.type_id, 0x61)
        self.assertEqual(partial.name, "MESH_TEST_PADDING")
        self.assertEqual(len(partial.raw), 171)
        self.assertTrue(partial.truncated)
        self.assertIn("declared 255 bytes, received 171", partial.decode_error or "")
        self.assertIn("truncated", partial.display)

        received = GatewayReceiveBuffer().feed(record)
        self.assertEqual(received.errors, ())
        self.assertEqual(len(received.packets), 1)
        self.assertTrue(received.packets[0].tlvs[-1].truncated)

    def test_unmarked_stream_rejects_same_partial_tlv_tail(self) -> None:
        record = stream_record(
            synthetic_truncated_payload(),
            msg_type=MSG_MESH_DATA,
            packet_flags=0x10,
        )

        with self.assertRaisesRegex(
            DecodeError,
            r"TLV 0x61 length 255 overruns payload at offset 82",
        ):
            parse_stream_record(record)

        received = GatewayReceiveBuffer().feed(record)
        self.assertEqual(received.packets, ())
        self.assertEqual(len(received.errors), 1)
        self.assertIn("TLV 0x61 length 255 overruns payload at offset 82", received.errors[0])

    def test_gateway_host_receipt_builder_round_trip_and_wire_parity(self) -> None:
        stream = stream_record(click_payload())
        source = parse_stream_record(stream)
        receipt = build_gateway_host_receipt(
            source,
            host_id=0xA1C1BEEFC0DE0001,
            gateway_id=0x9999888877776666,
        )

        self.assertEqual(receipt.identity.original_msg_type, MSG_CLICK_REPORT)
        self.assertEqual(receipt.identity.original_flags, 0x24)
        self.assertEqual(receipt.identity.src_id, source.src_id)
        self.assertEqual(receipt.identity.dst_id, source.dst_id)
        self.assertEqual(receipt.identity.session_id, source.session_id)
        self.assertEqual(receipt.identity.seq, source.seq)
        self.assertEqual(
            receipt.identity.stream_record_digest,
            hashlib.sha256(stream).digest(),
        )
        self.assertEqual(
            receipt.identity.stream_record_digest.hex(),
            hashlib.sha256(source.raw_transport).hexdigest(),
        )
        self.assertEqual(receipt.packet.transport, "cobs-shared-packet")
        self.assertEqual(receipt.packet.msg_type, MSG_GATEWAY_HOST_RECEIPT)
        self.assertEqual(receipt.packet.src_id, 0xA1C1BEEFC0DE0001)
        self.assertEqual(receipt.packet.dst_id, 0x9999888877776666)
        self.assertEqual(receipt.packet.session_id, source.session_id)
        self.assertEqual(receipt.packet.seq, source.seq)
        self.assertEqual(receipt.packet.ttl, 1)
        self.assertEqual(receipt.packet.flags, 0)
        self.assertEqual(len(receipt.packet.payload), GATEWAY_HOST_RECEIPT_TLV_LEN)
        self.assertEqual(
            receipt.packet.payload[:2],
            bytes((TLV_GATEWAY_HOST_RECEIPT_IDENTITY, 56)),
        )
        self.assertEqual(
            receipt.packet.payload[2:],
            bytes((MSG_CLICK_REPORT, 0x24))
            + source.src_id.to_bytes(8, "little")
            + source.dst_id.to_bytes(8, "little")
            + source.session_id.to_bytes(4, "little")
            + source.seq.to_bytes(2, "little")
            + hashlib.sha256(stream).digest(),
        )
        decoded = parse_gateway_host_receipt(receipt.packet)
        self.assertEqual(decoded, receipt.identity)

    def test_gateway_host_receipt_identity_fixed_vector_matches_c_layout(self) -> None:
        identity = GatewayHostReceiptIdentity(
            original_msg_type=MSG_CLICK_REPORT,
            original_flags=0x24,
            src_id=0x1122334455667788,
            dst_id=0x99AABBCCDDEEFF00,
            session_id=0xA1B2C3D4,
            seq=0xE5F6,
            stream_record_digest=bytes(range(32)),
        )
        expected = bytes.fromhex(
            "2024"
            "8877665544332211"
            "00ffeeddccbbaa99"
            "d4c3b2a1"
            "f6e5"
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f"
        )
        encoded = encode_gateway_host_receipt_identity(identity)
        self.assertEqual(len(encoded), GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN)
        self.assertEqual(encoded, expected)
        self.assertEqual(decode_gateway_host_receipt_identity(encoded), identity)

        command_identity = GatewayHostReceiptIdentity(
            original_msg_type=MSG_GATEWAY_COMMAND_EVENT,
            original_flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=0x9999AAAABBBBCCCC,
            dst_id=0x9999AAAABBBBCCCC,
            session_id=0x10203040,
            seq=0x3040,
            stream_record_digest=bytes(range(32)),
        )
        command_encoded = encode_gateway_host_receipt_identity(command_identity)
        self.assertEqual(
            decode_gateway_host_receipt_identity(command_encoded),
            command_identity,
        )
        crosswired_command = GatewayHostReceiptIdentity(
            original_msg_type=MSG_GATEWAY_COMMAND_EVENT,
            original_flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=command_identity.src_id,
            dst_id=command_identity.dst_id + 1,
            session_id=command_identity.session_id,
            seq=command_identity.seq,
            stream_record_digest=command_identity.stream_record_digest,
        )
        with self.assertRaises(ValueError):
            encode_gateway_host_receipt_identity(crosswired_command)
        malformed_command = bytearray(command_encoded)
        malformed_command[10] ^= 1
        with self.assertRaises(DecodeError):
            decode_gateway_host_receipt_identity(bytes(malformed_command))

        local_result = GatewayHostReceiptIdentity(
            original_msg_type=MSG_COMMAND_RESULT,
            original_flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_ERROR,
            src_id=command_identity.src_id,
            dst_id=command_identity.src_id,
            session_id=command_identity.session_id,
            seq=command_identity.seq,
            stream_record_digest=command_identity.stream_record_digest,
        )
        self.assertEqual(
            decode_gateway_host_receipt_identity(
                encode_gateway_host_receipt_identity(local_result)
            ),
            local_result,
        )
        mesh_result = GatewayHostReceiptIdentity(
            original_msg_type=MSG_COMMAND_RESULT,
            original_flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=command_identity.src_id,
            dst_id=command_identity.src_id + 1,
            session_id=command_identity.session_id,
            seq=command_identity.seq,
            stream_record_digest=command_identity.stream_record_digest,
        )
        self.assertEqual(
            decode_gateway_host_receipt_identity(
                encode_gateway_host_receipt_identity(mesh_result)
            ),
            mesh_result,
        )
        with self.assertRaises(ValueError):
            encode_gateway_host_receipt_identity(
                GatewayHostReceiptIdentity(
                    original_msg_type=MSG_COMMAND_RESULT,
                    original_flags=FLAG_ERROR,
                    src_id=local_result.src_id,
                    dst_id=local_result.dst_id,
                    session_id=local_result.session_id,
                    seq=local_result.seq,
                    stream_record_digest=local_result.stream_record_digest,
                )
            )

    def test_gateway_host_receipt_rejects_malformed_identity_and_trailing_tlvs(self) -> None:
        source = parse_stream_record(stream_record(click_payload()))
        receipt = build_gateway_host_receipt(
            source,
            host_id=0xA1C1BEEFC0DE0001,
            gateway_id=0x9999888877776666,
        )
        identity_value = receipt.packet.payload[2:]

        for malformed in (
            identity_value[:-1],
            b"\x00" * GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN,
        ):
            with self.assertRaises(DecodeError):
                decode_gateway_host_receipt_identity(malformed)

        duplicate_payload = receipt.packet.payload + receipt.packet.payload
        duplicate_frame = encode_cobs_packet(
            msg_type=MSG_GATEWAY_HOST_RECEIPT,
            flags=0,
            src_id=receipt.packet.src_id,
            dst_id=receipt.packet.dst_id,
            session_id=receipt.packet.session_id,
            seq=receipt.packet.seq,
            ttl=1,
            payload=duplicate_payload,
        )
        with self.assertRaises(DecodeError):
            parse_gateway_host_receipt(parse_cobs_packet(duplicate_frame))

        trailing_payload = receipt.packet.payload + bytes((0x01, 0x00))
        trailing_frame = encode_cobs_packet(
            msg_type=MSG_GATEWAY_HOST_RECEIPT,
            flags=0,
            src_id=receipt.packet.src_id,
            dst_id=receipt.packet.dst_id,
            session_id=receipt.packet.session_id,
            seq=receipt.packet.seq,
            ttl=1,
            payload=trailing_payload,
        )
        with self.assertRaises(DecodeError):
            parse_gateway_host_receipt(parse_cobs_packet(trailing_frame))

    def test_gateway_host_receipt_rejects_invalid_source_and_nonreceiptable_records(self) -> None:
        source = bytearray(stream_record(click_payload()))
        source[16:24] = b"\x00" * 8
        malformed = parse_stream_record(bytes(source))
        with self.assertRaises(ValueError):
            build_gateway_host_receipt(
                malformed,
                host_id=0xA1C1BEEFC0DE0001,
                gateway_id=0x9999888877776666,
            )

        generic_command_event = parse_stream_record(
            stream_record(
                gateway_assignment_event_payload(
                    event_sequence=0x10203040,
                    discovery_slot=0xFF,
                    progress_count=1,
                    total_count=0,
                    success_count=0,
                    failure_count=0,
                ),
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=0,
                packet_src_id=0x9999AAAABBBBCCCC,
                packet_dst_id=0x9999AAAABBBBCCCC,
                packet_session_id=0x10203040,
                packet_seq=0x3040,
            )
        )
        with self.assertRaises(ValueError):
            build_gateway_host_receipt(
                generic_command_event,
                host_id=0xA1C1BEEFC0DE0001,
                gateway_id=0x9999888877776666,
            )

        nonreceiptable = parse_stream_record(
            stream_record(b"", msg_type=MSG_GATEWAY_HOST_RECEIPT, packet_flags=0)
        )
        with self.assertRaises(ValueError):
            build_gateway_host_receipt(
                nonreceiptable,
                host_id=0xA1C1BEEFC0DE0001,
                gateway_id=0x9999888877776666,
            )

    def test_receive_buffer_accepts_stream_then_legacy_cobs(self) -> None:
        record = stream_record(click_payload())
        legacy = encode_cobs_packet(
            msg_type=MSG_MESH_DATA,
            flags=0,
            src_id=1,
            dst_id=2,
            session_id=3,
            seq=4,
            ttl=4,
            payload=tlv(TLV_EVENT_SEQ, (5).to_bytes(4, "little")),
        )
        result = GatewayReceiveBuffer().feed(record + legacy)
        self.assertEqual(result.errors, ())
        self.assertEqual([packet.transport for packet in result.packets], ["gateway-stream-v1", "cobs-shared-packet"])

    def test_anchor_discovery_command_uses_real_gateway_contract(self) -> None:
        gateway_id = 0xAABBCCDDEEFF0011
        command = build_anchor_discovery_command(
            host_id=DEFAULT_HOST_ID,
            gateway_id=gateway_id,
            session_id=0x10203040,
            seq=9,
            survey_id=0xA0B0C0D0,
            duration_ms=250,
            discovery_slot_count=6,
            sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
        )
        packet = command.packet
        self.assertEqual(command.command_id, CMD_SURVEY_REACHABILITY)
        self.assertEqual(packet.dst_id, gateway_id)
        self.assertEqual(packet.src_id, DEFAULT_HOST_ID)
        self.assertEqual(packet.session_id, 0x10203040)
        self.assertEqual(packet.seq, 9)
        self.assertEqual(packet.value(TLV_COMMAND_ID), CMD_SURVEY_REACHABILITY)
        self.assertEqual(packet.value(TLV_SURVEY_ID), 0xA0B0C0D0)
        self.assertEqual(packet.value(TLV_DURATION_MS), 250)
        self.assertEqual(
            packet.value(TLV_SAMPLE_COUNT),
            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
        )
        self.assertEqual(packet.value(TLV_DISCOVERY_SLOT_COUNT), 6)
        self.assertEqual(
            [value.type_id for value in packet.tlvs],
            [TLV_COMMAND_ID, TLV_SURVEY_ID, TLV_DURATION_MS, TLV_SAMPLE_COUNT, TLV_DISCOVERY_SLOT_COUNT],
        )

    def test_gateway_identity_decodes_exact_little_endian_device_id(self) -> None:
        gateway_id = 0xAABBCCDDEEFF0011

        self.assertEqual(decode_gateway_identity(gateway_id.to_bytes(8, "little")), gateway_id)

        with self.assertRaisesRegex(DecodeError, "exactly 8 bytes"):
            decode_gateway_identity(b"\x01" * 7)
        with self.assertRaisesRegex(DecodeError, "non-zero"):
            decode_gateway_identity(b"\x00" * 8)

    def test_here_i_am_command_targets_local_gateway_with_only_command_id(self) -> None:
        gateway_id = 0xAABBCCDDEEFF0011
        command = build_here_i_am_command(
            host_id=DEFAULT_HOST_ID,
            gateway_id=gateway_id,
            session_id=0x55667788,
            seq=11,
        )
        self.assertEqual(command.command_id, CMD_FORCE_REDISCOVERY)
        self.assertEqual(command.packet.dst_id, gateway_id)
        self.assertEqual(command.packet.value(TLV_COMMAND_ID), CMD_FORCE_REDISCOVERY)
        self.assertEqual([value.type_id for value in command.packet.tlvs], [TLV_COMMAND_ID])

    def test_survey_abort_targets_local_gateway_with_only_command_id(self) -> None:
        gateway_id = 0xAABBCCDDEEFF0011
        command = build_survey_abort_command(
            host_id=DEFAULT_HOST_ID,
            gateway_id=gateway_id,
            session_id=0x55667789,
            seq=12,
        )

        self.assertEqual(command.command_id, 0x0103)
        self.assertEqual(command.packet.dst_id, gateway_id)
        self.assertEqual(command.packet.value(TLV_COMMAND_ID), 0x0103)
        self.assertEqual(
            [value.type_id for value in command.packet.tlvs],
            [TLV_COMMAND_ID],
        )

    def test_assign_discovery_slots_command_targets_local_gateway_with_only_command_id(self) -> None:
        gateway_id = 0xAABBCCDDEEFF0011

        command = build_assign_discovery_slots_command(
            host_id=DEFAULT_HOST_ID,
            gateway_id=gateway_id,
            session_id=0x99AABBCC,
            seq=12,
        )

        self.assertEqual(command.command_id, CMD_ASSIGN_DISCOVERY_SLOTS)
        self.assertEqual(command.packet.src_id, DEFAULT_HOST_ID)
        self.assertEqual(command.packet.dst_id, gateway_id)
        self.assertEqual(command.packet.session_id, 0x99AABBCC)
        self.assertEqual(command.packet.seq, 12)
        self.assertEqual(command.packet.value(TLV_COMMAND_ID), CMD_ASSIGN_DISCOVERY_SLOTS)
        self.assertEqual([value.type_id for value in command.packet.tlvs], [TLV_COMMAND_ID])
        self.assertIn("ASSIGN_DISCOVERY_SLOTS", command.packet.tlvs[0].display)

        bounded = build_assign_discovery_slots_command(
            host_id=DEFAULT_HOST_ID,
            gateway_id=gateway_id,
            session_id=0x99AABBCD,
            seq=13,
            expected_anchor_count=50,
        )
        self.assertEqual(bounded.packet.value(TLV_EXPECTED_NODE_COUNT), 50)
        with self.assertRaisesRegex(ValueError, "expected anchor count"):
            build_assign_discovery_slots_command(
                host_id=DEFAULT_HOST_ID,
                gateway_id=gateway_id,
                session_id=1,
                seq=1,
                expected_anchor_count=51,
            )

        with self.assertRaisesRegex(ValueError, "gateway ID must differ from host ID"):
            build_assign_discovery_slots_command(
                host_id=DEFAULT_HOST_ID,
                gateway_id=DEFAULT_HOST_ID,
                session_id=1,
                seq=1,
            )

    def test_gateway_command_budget_is_optional_and_shared_by_all_workflows(self) -> None:
        common: dict[str, Any] = {
            "host_id": DEFAULT_HOST_ID,
            "gateway_id": 0xAABBCCDDEEFF0011,
            "session_id": 1,
            "seq": 2,
            "command_budget_ms": 1_600_000,
        }
        commands = (
            build_here_i_am_command(**common),
            build_anchor_discovery_command(
                **common,
                survey_id=3,
                duration_ms=250,
                discovery_slot_count=3,
                sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
            ),
        )
        for command in commands:
            with self.subTest(command=command.label):
                self.assertEqual(
                    command.packet.value(TLV_COMMAND_BUDGET_MS), 1_600_000
                )
                budget_tlv = next(
                    value for value in command.packet.tlvs
                    if value.type_id == TLV_COMMAND_BUDGET_MS
                )
                self.assertEqual(budget_tlv.name, "COMMAND_BUDGET_MS")

        assignment = build_assign_discovery_slots_command(
            **{
                **common,
                "command_budget_ms":
                    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
            }
        )
        self.assertEqual(
            assignment.packet.value(TLV_COMMAND_BUDGET_MS),
            DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
        )
        self.assertEqual(
            build_assign_discovery_slots_command(**common).packet.value(
                TLV_COMMAND_BUDGET_MS
            ),
            1_600_000,
        )

        maximum = {**common, "command_budget_ms": GATEWAY_COMMAND_BUDGET_MAX_MS}
        self.assertEqual(
            build_assign_discovery_slots_command(**maximum).packet.value(
                TLV_COMMAND_BUDGET_MS
            ),
            GATEWAY_COMMAND_BUDGET_MAX_MS,
        )

        for invalid in (999, GATEWAY_COMMAND_BUDGET_MAX_MS + 1):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(ValueError, "command budget"):
                    build_here_i_am_command(**{**common, "command_budget_ms": invalid})

    def test_bare_builders_reject_budgets_firmware_cannot_admit(self) -> None:
        common: dict[str, Any] = {
            "host_id": DEFAULT_HOST_ID,
            "gateway_id": 0xAABBCCDDEEFF0011,
            "session_id": 1,
            "seq": 2,
        }
        with self.assertRaisesRegex(ValueError, "assignment policy: minimum"):
            build_assign_discovery_slots_command(
                **common,
                command_budget_ms=(
                    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS - 1
                ),
            )
        three_anchor = build_assign_discovery_slots_command(
            **common,
            expected_anchor_count=3,
            command_budget_ms=751_204,
        )
        self.assertEqual(
            three_anchor.packet.value(TLV_COMMAND_BUDGET_MS),
            751_204,
        )

        with self.assertRaisesRegex(
            ValueError, "survey discovery policy: minimum 209993"
        ):
            build_anchor_discovery_command(
                **common,
                survey_id=3,
                duration_ms=250,
                discovery_slot_count=6,
                sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
                command_budget_ms=209_992,
            )

        # Route refresh is different: firmware accepts a shorter explicit
        # horizon to intentionally limit retries, while omission selects its
        # robust 120-second default.
        route = build_here_i_am_command(**common, command_budget_ms=1_000)
        self.assertEqual(route.packet.value(TLV_COMMAND_BUDGET_MS), 1_000)

    def test_v1_operation_policy_is_repeated_decoded_and_phase_budget_is_independent(self) -> None:
        profile = OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(5, 1_600_000, 750),
            discovery=DiscoveryOperationPolicy(
                90_000, 80, 12, 3, 1_500, 500_000
            ),
            pair=PairOperationPolicy(1, 8),
        )
        common: dict[str, Any] = {
            "host_id": DEFAULT_HOST_ID,
            "gateway_id": 0xAABBCCDDEEFF0011,
            "operation_policy": profile,
        }
        survey = build_anchor_discovery_command(
            **common,
            session_id=10,
            seq=11,
            survey_id=12,
            duration_ms=1_500,
            discovery_slot_count=12,
            sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
            command_budget_ms=500_000,
        )
        assignment = build_assign_discovery_slots_command(
            **common,
            session_id=13,
            seq=14,
            expected_anchor_count=5,
            command_budget_ms=1_600_000,
        )
        here_i_am = build_here_i_am_command(
            **common,
            session_id=15,
            seq=16,
        )

        expected_values = profile.encoded_values()
        expected_by_command = (
            (survey, expected_values[1:], ["survey_discovery", "survey_pair"]),
            (assignment, expected_values[:1], ["assignment"]),
            (
                here_i_am,
                expected_values,
                ["assignment", "survey_discovery", "survey_pair"],
            ),
        )
        for command, expected, families in expected_by_command:
            with self.subTest(command=command.label):
                policy_tlvs = tuple(
                    value for value in command.packet.tlvs
                    if value.type_id == TLV_OPERATION_POLICY
                )
                self.assertEqual(
                    tuple(value.raw for value in policy_tlvs), expected
                )
                self.assertEqual(
                    [value.decoded["family"] for value in policy_tlvs],
                    families,
                )
                self.assertTrue(all(
                    value.name == "OPERATION_POLICY" for value in policy_tlvs
                ))

        self.assertEqual(survey.packet.value(TLV_DURATION_MS), 1_500)
        self.assertEqual(survey.packet.value(TLV_DISCOVERY_SLOT_COUNT), 12)
        self.assertEqual(survey.packet.value(TLV_COMMAND_BUDGET_MS), 500_000)
        self.assertEqual(assignment.packet.value(TLV_EXPECTED_NODE_COUNT), 5)
        self.assertEqual(
            assignment.packet.value(TLV_COMMAND_BUDGET_MS), 1_600_000
        )

        with self.assertRaisesRegex(ValueError, "legacy duration"):
            build_anchor_discovery_command(
                **common, session_id=20, seq=21, survey_id=22,
                duration_ms=1_499, discovery_slot_count=12,
                sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
                command_budget_ms=500_000,
            )
        with self.assertRaisesRegex(ValueError, "legacy discovery slot"):
            build_anchor_discovery_command(
                **common, session_id=20, seq=21, survey_id=22,
                duration_ms=1_500, discovery_slot_count=11,
                sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
                command_budget_ms=500_000,
            )
        independent_budget = build_anchor_discovery_command(
            **common, session_id=20, seq=21, survey_id=22,
            duration_ms=1_500, discovery_slot_count=12,
            sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
            command_budget_ms=499_999,
        )
        self.assertEqual(
            independent_budget.packet.value(TLV_COMMAND_BUDGET_MS), 499_999
        )
        discovery_policy = tuple(
            value.raw for value in independent_budget.packet.tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )[0]
        self.assertEqual(int.from_bytes(discovery_policy[15:19], "little"), 500_000)
        with self.assertRaisesRegex(ValueError, "legacy expected"):
            build_assign_discovery_slots_command(
                **common, session_id=23, seq=24,
                expected_anchor_count=4, command_budget_ms=1_600_000,
            )
        with self.assertRaisesRegex(ValueError, "legacy command budget"):
            build_assign_discovery_slots_command(
                **common, session_id=23, seq=24,
                expected_anchor_count=5, command_budget_ms=1_600_001,
            )

    def test_discovery_assignment_tlvs_and_clock_offsets_decode_exact_wire_shapes(self) -> None:
        entries = (
            (0x1111222233334444, 0x0102030405060708, 0),
            (0x5555666677778888, 0x1112131415161718, 1),
            (0x9999AAAABBBBCCCC, 0x2122232425262728, 2),
        )

        def entry_bytes(entry: tuple[int, int, int]) -> bytes:
            anchor_id, hash_value, slot = entry
            return (
                anchor_id.to_bytes(8, "little")
                + hash_value.to_bytes(8, "little")
                + bytes((slot,))
            )

        payload = bytearray()
        append_tlv(payload, TLV_DISCOVERY_ASSIGNMENT_PHASE, b"\x02")
        append_tlv(payload, TLV_DISCOVERY_ASSIGNMENT_EPOCH, (0x12345678).to_bytes(4, "little"))
        append_tlv(
            payload,
            TLV_DISCOVERY_ASSIGNMENT_HASH,
            (0x8877665544332211).to_bytes(8, "little"),
        )
        append_tlv(
            payload,
            TLV_DISCOVERY_ASSIGNMENT_TABLE,
            entry_bytes(entries[0]) + entry_bytes(entries[1]),
        )
        append_tlv(payload, TLV_DISCOVERY_ASSIGNMENT_TABLE, entry_bytes(entries[2]))
        append_tlv(payload, TLV_UWB_CLOCK_OFFSET_RAW, (-123).to_bytes(2, "little", signed=True))
        append_tlv(
            payload,
            TLV_CLICKER_CLOCK_OFFSET_RAW,
            (321).to_bytes(2, "little", signed=True),
        )

        values = parse_tlvs(bytes(payload))
        phase = next(value for value in values if value.type_id == TLV_DISCOVERY_ASSIGNMENT_PHASE)
        epoch = next(value for value in values if value.type_id == TLV_DISCOVERY_ASSIGNMENT_EPOCH)
        hash_value = next(value for value in values if value.type_id == TLV_DISCOVERY_ASSIGNMENT_HASH)
        tables = [value for value in values if value.type_id == TLV_DISCOVERY_ASSIGNMENT_TABLE]

        self.assertEqual(phase.decoded, 2)
        self.assertEqual(phase.display, "2 (TABLE)")
        self.assertEqual(epoch.decoded, 0x12345678)
        self.assertEqual(hash_value.display, "0x8877665544332211")
        self.assertEqual(len(tables), 2)
        self.assertEqual(
            tables[0].decoded,
            [
                {"anchor_id": entries[0][0], "hash": entries[0][1], "slot": 0},
                {"anchor_id": entries[1][0], "hash": entries[1][1], "slot": 1},
            ],
        )
        self.assertIn("anchor=0x1111222233334444", tables[0].display)
        self.assertIn("slot=2", tables[1].display)
        self.assertEqual(
            next(value.decoded for value in values if value.type_id == TLV_UWB_CLOCK_OFFSET_RAW),
            -123,
        )
        self.assertEqual(
            next(
                value.decoded
                for value in values
                if value.type_id == TLV_CLICKER_CLOCK_OFFSET_RAW
            ),
            321,
        )

        malformed = parse_tlvs(tlv(TLV_DISCOVERY_ASSIGNMENT_TABLE, b"\x00" * 16))[0]
        self.assertIn("non-empty multiple of 17 bytes", malformed.decode_error or "")

    def test_protocol_identity_tlvs_decode_only_exact_wire_shapes(self) -> None:
        wire_shapes = (
            (0xB1, "DISCOVERY_ASSIGNMENT_SCHEME_VERSION", b"\x02"),
            (
                0xB2,
                "DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT",
                bytes(range(32)),
            ),
            (
                0xB6,
                "SURVEY_OPERATION_GENERATION",
                (0x1122334455667788).to_bytes(8, "little"),
            ),
            (
                0xB7,
                "SURVEY_ROUND_COMMITMENT",
                bytes(reversed(range(32))),
            ),
            (
                0xB8,
                "MESH_ACK_SEMANTIC_IDENTITY",
                (
                    (0xAABBCCDD).to_bytes(4, "little")
                    + (0x1234).to_bytes(2, "little")
                    + bytes(range(32))
                ),
            ),
        )

        for type_id, expected_name, raw in wire_shapes:
            with self.subTest(type_id=f"0x{type_id:02x}"):
                decoded = parse_tlvs(tlv(type_id, raw))[0]
                self.assertTrue(decoded.known)
                self.assertEqual(decoded.name, expected_name)
                self.assertIsNone(decoded.decode_error)

                for malformed_raw in (raw[:-1], raw + b"\x00"):
                    malformed = parse_tlvs(tlv(type_id, malformed_raw))[0]
                    self.assertTrue(malformed.known)
                    self.assertEqual(malformed.name, expected_name)
                    self.assertIsNotNone(malformed.decode_error)

    def test_cir_is_one_complex_sample_not_a_trace(self) -> None:
        decoded = decode_cir_sample(b"\x01\x00\x00\xfe\xff\xff")
        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded["real_signed24"], 1)
        self.assertEqual(decoded["imag_signed24"], -2)
        self.assertAlmostEqual(float(decoded["magnitude"]), 5 ** 0.5)


if __name__ == "__main__":
    unittest.main()
