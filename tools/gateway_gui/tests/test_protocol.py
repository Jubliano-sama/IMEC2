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
    FLAG_GATEWAY_ACK_REQUIRED,
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
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_GATEWAY_HOST_RECEIPT,
    MSG_MESH_DATA,
    PACKET_EXT_MAX_PAYLOAD_LEN,
    SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    TLV_ANCHOR_ID,
    TLV_BURST_ID,
    TLV_CLICKER_ID,
    TLV_CLICKER_CLOCK_OFFSET_RAW,
    TLV_COMMAND_ID,
    TLV_COMMAND_BUDGET_MS,
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
    TLV_DISTANCE_MM,
    TLV_DISTANCE_SAMPLES_MM,
    TLV_DURATION_MS,
    TLV_EVENT_SEQ,
    TLV_GATEWAY_HOST_RECEIPT_IDENTITY,
    TLV_QUALITY,
    TLV_RANGE_STATUS,
    TLV_RANGE_ROUND_INDICES,
    TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX,
    TLV_SEQUENCE_START_TIMESTAMPS_MS,
    TLV_SURVEY_ID,
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
    record[10:12] = (0x1234).to_bytes(2, "little")
    record[12:16] = click_report_session_id(
        0x1111222233334444, 0x11223344
    ).to_bytes(4, "little")
    record[16:24] = (0x5555666677778888).to_bytes(8, "little")
    record[24:32] = (0x9999AAAABBBBCCCC).to_bytes(8, "little")
    record[32:36] = (17).to_bytes(4, "little")
    record[36:38] = len(payload).to_bytes(2, "little")
    record[38:40] = crc16_ccitt_false(payload).to_bytes(2, "little")
    record.extend(payload)
    return bytes(record)


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
                src_id=1,
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

    def test_gateway_host_receipt_rejects_invalid_source_and_host_only_records(self) -> None:
        source = bytearray(stream_record(click_payload()))
        source[16:24] = b"\x00" * 8
        malformed = parse_stream_record(bytes(source))
        with self.assertRaises(ValueError):
            build_gateway_host_receipt(
                malformed,
                host_id=0xA1C1BEEFC0DE0001,
                gateway_id=0x9999888877776666,
            )

        host_only = parse_stream_record(
            stream_record(b"", msg_type=MSG_GATEWAY_COMMAND_EVENT, packet_flags=0)
        )
        with self.assertRaises(ValueError):
            build_gateway_host_receipt(
                host_only,
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
            "command_budget_ms": 15000,
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
                self.assertEqual(command.packet.value(TLV_COMMAND_BUDGET_MS), 15000)
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
            15000,
        )

        maximum = {**common, "command_budget_ms": 900000}
        self.assertEqual(
            build_assign_discovery_slots_command(**maximum).packet.value(
                TLV_COMMAND_BUDGET_MS
            ),
            900000,
        )

        for invalid in (999, 900001):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(ValueError, "command budget"):
                    build_here_i_am_command(**{**common, "command_budget_ms": invalid})

    def test_v1_operation_policy_is_repeated_decoded_and_legacy_equal(self) -> None:
        profile = OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(5, 300_000, 750),
            discovery=DiscoveryOperationPolicy(
                9_000, 80, 12, 3, 1_500, 500_000
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
            command_budget_ms=300_000,
        )
        here_i_am = build_here_i_am_command(
            **common,
            session_id=15,
            seq=16,
        )

        expected_values = profile.encoded_values()
        for command in (survey, assignment, here_i_am):
            with self.subTest(command=command.label):
                policy_tlvs = tuple(
                    value for value in command.packet.tlvs
                    if value.type_id == TLV_OPERATION_POLICY
                )
                self.assertEqual(
                    tuple(value.raw for value in policy_tlvs), expected_values
                )
                self.assertEqual(
                    [value.decoded["family"] for value in policy_tlvs],
                    ["assignment", "survey_discovery", "survey_pair"],
                )
                self.assertTrue(all(
                    value.name == "OPERATION_POLICY" for value in policy_tlvs
                ))

        self.assertEqual(survey.packet.value(TLV_DURATION_MS), 1_500)
        self.assertEqual(survey.packet.value(TLV_DISCOVERY_SLOT_COUNT), 12)
        self.assertEqual(survey.packet.value(TLV_COMMAND_BUDGET_MS), 500_000)
        self.assertEqual(assignment.packet.value(TLV_EXPECTED_NODE_COUNT), 5)
        self.assertEqual(assignment.packet.value(TLV_COMMAND_BUDGET_MS), 300_000)

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
        with self.assertRaisesRegex(ValueError, "legacy command budget"):
            build_anchor_discovery_command(
                **common, session_id=20, seq=21, survey_id=22,
                duration_ms=1_500, discovery_slot_count=12,
                sample_count=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
                command_budget_ms=499_999,
            )
        with self.assertRaisesRegex(ValueError, "legacy expected"):
            build_assign_discovery_slots_command(
                **common, session_id=23, seq=24,
                expected_anchor_count=4, command_budget_ms=300_000,
            )
        with self.assertRaisesRegex(ValueError, "legacy command budget"):
            build_assign_discovery_slots_command(
                **common, session_id=23, seq=24,
                expected_anchor_count=5, command_budget_ms=300_001,
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
