from __future__ import annotations

import math
import unittest

from tools.gateway_gui.cir_reassembly import (
    CIR_FRAGMENT_COUNT,
    CIR_WINDOW_BYTES,
    CIR_WINDOW_SAMPLES,
    FLAG_DIAGNOSTIC,
    CirAssemblyKey,
    CirReassembler,
    decode_accumulator_sample,
)
from tools.gateway_gui.protocol import (
    GATEWAY_STREAM_FLAG_TRUNCATED,
    GATEWAY_STREAM_MAGIC,
    GATEWAY_STREAM_RECORD_HEADER_LEN,
    GATEWAY_STREAM_RECORD_PACKET,
    GATEWAY_STREAM_VERSION,
    MSG_CLICK_REPORT,
    TLV_ANCHOR_ID,
    TLV_CLICKER_ID,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_EVENT_SEQ,
    TLV_TIMESTAMP_MS,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_FULL_CHUNK,
    TLV_UWB_CIR_START_INDEX,
    TLV_UWB_CIR_TOTAL_BYTES,
    Packet,
    append_tlv,
    crc16_ccitt_false,
    encode_cobs_packet,
    parse_cobs_packet,
    parse_stream_record,
)


CLICKER_ID = 0x1111222233334444
ANCHOR_ID = 0x5555666677778888
EVENT_SEQ = 73
TIMESTAMP_MS = 1_234_567
START_INDEX = 700
FIRST_PATH_INDEX = 764
FIRST_PACKET_CIR_BYTES = 881
TLV_MAX_BYTES = 255
FRAGMENT_COUNT = CIR_FRAGMENT_COUNT
TLV_MESH_CH9_BATCH_ID = 0xA2
TLV_MESH_CH9_BATCH_FLAGS = 0xA3


def signed24(value: int) -> bytes:
    return value.to_bytes(3, "little", signed=True)


def cir_window() -> bytes:
    return b"".join(
        signed24(index - 96) + signed24(191 - (2 * index))
        for index in range(CIR_WINDOW_SAMPLES)
    )


CIR_BYTES = cir_window()


def fragment_packet(
    fragment_index: int,
    *,
    fragment_count: int = FRAGMENT_COUNT,
    byte_offset: int | None = None,
    total_bytes: int = CIR_WINDOW_BYTES,
    first_path_index: int = FIRST_PATH_INDEX,
    start_index: int = START_INDEX,
    timestamp_ms: int = TIMESTAMP_MS,
    chunk: bytes | None = None,
    chunk_tlvs: tuple[bytes, ...] | None = None,
    event_seq: int = EVENT_SEQ,
    clicker_id: int = CLICKER_ID,
    anchor_id: int = ANCHOR_ID,
) -> Packet:
    default_offset = 0 if fragment_index == 0 else FIRST_PACKET_CIR_BYTES
    offset = default_offset if byte_offset is None else byte_offset
    default_end = FIRST_PACKET_CIR_BYTES if fragment_index == 0 else CIR_WINDOW_BYTES
    fragment_bytes = CIR_BYTES[offset:default_end] if chunk is None else chunk
    chunks = chunk_tlvs
    if chunks is None:
        chunks = tuple(
            fragment_bytes[index:index + TLV_MAX_BYTES]
            for index in range(0, len(fragment_bytes), TLV_MAX_BYTES)
        )
    payload = bytearray()
    for type_id, value in (
        (TLV_CLICKER_ID, clicker_id.to_bytes(8, "little")),
        (TLV_ANCHOR_ID, anchor_id.to_bytes(8, "little")),
        (TLV_EVENT_SEQ, event_seq.to_bytes(4, "little")),
        (TLV_TIMESTAMP_MS, timestamp_ms.to_bytes(8, "little")),
        (TLV_DIAG_FRAGMENT_INDEX, fragment_index.to_bytes(2, "little")),
        (TLV_DIAG_FRAGMENT_COUNT, fragment_count.to_bytes(2, "little")),
        (TLV_UWB_CIR_BYTE_OFFSET, offset.to_bytes(2, "little")),
        (TLV_UWB_CIR_TOTAL_BYTES, total_bytes.to_bytes(2, "little")),
        (TLV_UWB_CIR_FIRST_PATH_INDEX, first_path_index.to_bytes(2, "little")),
        (TLV_UWB_CIR_START_INDEX, start_index.to_bytes(2, "little")),
    ):
        append_tlv(payload, type_id, value)
    for chunk_value in chunks:
        append_tlv(payload, TLV_UWB_CIR_FULL_CHUNK, chunk_value)
    append_tlv(payload, TLV_MESH_CH9_BATCH_ID, event_seq.to_bytes(4, "little"))
    append_tlv(
        payload,
        TLV_MESH_CH9_BATCH_FLAGS,
        bytes((1 if fragment_index == fragment_count - 1 else 0,)),
    )
    frame = encode_cobs_packet(
        msg_type=MSG_CLICK_REPORT,
        flags=FLAG_DIAGNOSTIC,
        src_id=anchor_id,
        dst_id=0x9999AAAABBBBCCCC,
        session_id=event_seq,
        seq=fragment_index + 1,
        ttl=4,
        payload=bytes(payload),
        message_age_ms=0,
    )
    return parse_cobs_packet(frame)


def non_cir_diagnostic_fragment_packet() -> Packet:
    payload = bytearray()
    append_tlv(payload, TLV_CLICKER_ID, CLICKER_ID.to_bytes(8, "little"))
    append_tlv(payload, TLV_ANCHOR_ID, ANCHOR_ID.to_bytes(8, "little"))
    append_tlv(payload, TLV_EVENT_SEQ, EVENT_SEQ.to_bytes(4, "little"))
    append_tlv(payload, TLV_DIAG_FRAGMENT_INDEX, (0).to_bytes(2, "little"))
    append_tlv(payload, TLV_DIAG_FRAGMENT_COUNT, (2).to_bytes(2, "little"))
    frame = encode_cobs_packet(
        msg_type=MSG_CLICK_REPORT,
        flags=FLAG_DIAGNOSTIC,
        src_id=ANCHOR_ID,
        dst_id=0x9999AAAABBBBCCCC,
        session_id=EVENT_SEQ,
        seq=1,
        ttl=4,
        payload=bytes(payload),
        message_age_ms=0,
    )
    return parse_cobs_packet(frame)


def truncated_first_fragment_packet() -> Packet:
    payload = bytearray(fragment_packet(0).payload)
    batch_metadata_bytes = 9
    final_chunk_bytes = FIRST_PACKET_CIR_BYTES % TLV_MAX_BYTES
    final_tlv_offset = len(payload) - batch_metadata_bytes - (2 + final_chunk_bytes)
    assert payload[final_tlv_offset] == TLV_UWB_CIR_FULL_CHUNK
    payload[final_tlv_offset + 1] = 255
    record = bytearray(GATEWAY_STREAM_RECORD_HEADER_LEN)
    record[0:2] = GATEWAY_STREAM_MAGIC.to_bytes(2, "little")
    record[2] = GATEWAY_STREAM_VERSION
    record[3] = GATEWAY_STREAM_RECORD_HEADER_LEN
    record[4] = GATEWAY_STREAM_RECORD_PACKET
    record[5] = 1
    record[7] = GATEWAY_STREAM_FLAG_TRUNCATED
    record[8] = MSG_CLICK_REPORT
    record[9] = FLAG_DIAGNOSTIC
    record[10:12] = (1).to_bytes(2, "little")
    record[12:16] = EVENT_SEQ.to_bytes(4, "little")
    record[16:24] = ANCHOR_ID.to_bytes(8, "little")
    record[24:32] = (0x9999AAAABBBBCCCC).to_bytes(8, "little")
    record[36:38] = len(payload).to_bytes(2, "little")
    record[38:40] = crc16_ccitt_false(payload).to_bytes(2, "little")
    record.extend(payload)
    return parse_stream_record(bytes(record))


class CirDecodeTests(unittest.TestCase):
    def test_signed_component_order_and_magnitude(self) -> None:
        raw = b"\x01\x02\x03\xfe\xff\xff"

        sample = decode_accumulator_sample(raw, window_index=5, start_index=400)

        self.assertEqual(sample.real, 0x030201)
        self.assertEqual(sample.imaginary, -2)
        self.assertEqual(sample.window_index, 5)
        self.assertEqual(sample.absolute_index, 405)
        self.assertEqual(sample.byte_offset, 30)
        self.assertEqual(sample.raw, raw)
        self.assertAlmostEqual(sample.magnitude, math.hypot(0x030201, -2))

    def test_sample_decode_requires_exactly_six_bytes(self) -> None:
        with self.assertRaisesRegex(ValueError, "expected 6 CIR bytes"):
            decode_accumulator_sample(b"\x00" * 5, window_index=0, start_index=0)


class CirReassemblyTests(unittest.TestCase):
    key = CirAssemblyKey(CLICKER_ID, ANCHOR_ID, EVENT_SEQ)

    def test_out_of_order_fragments_complete_192_sample_view(self) -> None:
        reassembler = CirReassembler()

        packets = (fragment_packet(1), fragment_packet(0))
        self.assertEqual(len(packets[1].payload), 958)
        self.assertEqual(
            [
                len(tlv.raw)
                for tlv in packets[1].tlvs
                if tlv.type_id == TLV_UWB_CIR_FULL_CHUNK
            ],
            [255, 255, 255, 116],
        )
        self.assertEqual(
            [
                len(tlv.raw)
                for tlv in packets[0].tlvs
                if tlv.type_id == TLV_UWB_CIR_FULL_CHUNK
            ],
            [255, 16],
        )
        for packet in packets:
            result = reassembler.ingest(packet)
            self.assertIsNotNone(result)
            assert result is not None
            self.assertTrue(result.accepted)

        view = reassembler.view(self.key)
        self.assertIsNotNone(view)
        assert view is not None
        self.assertTrue(view.complete)
        self.assertEqual(view.state, "complete")
        self.assertEqual(view.raw, CIR_BYTES)
        self.assertEqual(len(view.samples), CIR_WINDOW_SAMPLES)
        self.assertEqual(view.received_fragment_indices, tuple(range(FRAGMENT_COUNT)))
        self.assertEqual(view.missing_fragment_indices, ())
        self.assertEqual(view.gaps, ())
        self.assertEqual(view.start_index, START_INDEX)
        self.assertEqual(view.first_path_index, FIRST_PATH_INDEX)
        self.assertEqual(view.samples[64].absolute_index, FIRST_PATH_INDEX)
        self.assertEqual(view.samples[64].raw, CIR_BYTES[384:390])
        self.assertAlmostEqual(
            view.samples[64].magnitude,
            math.hypot(view.samples[64].real, view.samples[64].imaginary),
        )

    def test_missing_fragment_stays_incomplete_without_synthetic_samples(self) -> None:
        reassembler = CirReassembler()

        reassembler.ingest(fragment_packet(0))

        view = reassembler.view(self.key)
        self.assertIsNotNone(view)
        assert view is not None
        self.assertFalse(view.complete)
        self.assertEqual(view.state, "incomplete")
        self.assertEqual(view.errors, ())
        self.assertEqual(view.missing_fragment_indices, (1,))
        self.assertEqual(view.gaps, ((FIRST_PACKET_CIR_BYTES, CIR_WINDOW_BYTES),))
        self.assertIsNone(view.raw)
        self.assertEqual(view.samples, ())

    def test_overlap_is_rejected_and_kept_as_visible_error(self) -> None:
        reassembler = CirReassembler()
        reassembler.ingest(fragment_packet(0))

        result = reassembler.ingest(
            fragment_packet(1, byte_offset=880, chunk=CIR_BYTES[880:])
        )

        self.assertIsNotNone(result)
        assert result is not None and result.view is not None
        self.assertFalse(result.accepted)
        self.assertTrue(any("overlaps fragment 0" in error for error in result.errors))
        self.assertEqual(result.view.state, "malformed")
        self.assertEqual(result.view.received_fragment_indices, (0,))
        self.assertEqual(result.view.samples, ())

    def test_out_of_bounds_chunk_is_rejected(self) -> None:
        reassembler = CirReassembler()

        result = reassembler.ingest(
            fragment_packet(0, byte_offset=1100, chunk=CIR_BYTES[:128])
        )

        self.assertIsNotNone(result)
        assert result is not None and result.view is not None
        self.assertFalse(result.accepted)
        self.assertTrue(any("exceeds total 1152" in error for error in result.errors))
        self.assertEqual(result.view.state, "malformed")
        self.assertEqual(result.view.received_fragment_indices, ())

    def test_fragment_metadata_mismatch_is_rejected(self) -> None:
        cases = (
            (fragment_packet(1, timestamp_ms=TIMESTAMP_MS + 1), "TIMESTAMP_MS"),
            (fragment_packet(1, fragment_count=FRAGMENT_COUNT + 1), "DIAG_FRAGMENT_COUNT"),
            (fragment_packet(1, total_bytes=CIR_WINDOW_BYTES - 6), "UWB_CIR_TOTAL_BYTES"),
            (
                fragment_packet(1, first_path_index=FIRST_PATH_INDEX + 1),
                "UWB_CIR_FIRST_PATH_INDEX",
            ),
            (fragment_packet(1, start_index=START_INDEX + 1), "UWB_CIR_START_INDEX"),
        )
        for mismatched_packet, field_name in cases:
            with self.subTest(field_name=field_name):
                reassembler = CirReassembler()
                reassembler.ingest(fragment_packet(0))

                result = reassembler.ingest(mismatched_packet)

                self.assertIsNotNone(result)
                assert result is not None and result.view is not None
                self.assertFalse(result.accepted)
                self.assertTrue(
                    any(f"metadata mismatch for {field_name}" in error for error in result.errors)
                )
                self.assertEqual(result.view.state, "malformed")
                self.assertEqual(result.view.received_fragment_indices, (0,))

    def test_terminal_gap_is_malformed_instead_of_zero_filled(self) -> None:
        reassembler = CirReassembler()
        reassembler.ingest(fragment_packet(0))

        result = reassembler.ingest(
            fragment_packet(
                1,
                byte_offset=FIRST_PACKET_CIR_BYTES + 1,
                chunk=CIR_BYTES[FIRST_PACKET_CIR_BYTES + 1:],
            )
        )

        self.assertIsNotNone(result)
        assert result is not None and result.view is not None
        self.assertTrue(result.accepted)
        self.assertEqual(result.view.missing_fragment_indices, ())
        self.assertEqual(
            result.view.gaps,
            ((FIRST_PACKET_CIR_BYTES, FIRST_PACKET_CIR_BYTES + 1),),
        )
        self.assertEqual(result.view.state, "malformed")
        self.assertTrue(any("byte coverage has gaps" in error for error in result.view.errors))
        self.assertIsNone(result.view.raw)
        self.assertEqual(result.view.samples, ())

    def test_duplicate_fragment_index_is_rejected(self) -> None:
        reassembler = CirReassembler()
        reassembler.ingest(fragment_packet(0))

        result = reassembler.ingest(fragment_packet(0))

        self.assertIsNotNone(result)
        assert result is not None and result.view is not None
        self.assertFalse(result.accepted)
        self.assertEqual(result.errors, ("duplicate fragment index 0",))
        self.assertEqual(result.view.state, "malformed")

    def test_empty_repeated_chunk_tlv_rejects_whole_packet_fragment(self) -> None:
        reassembler = CirReassembler()
        packet = fragment_packet(
            1,
            chunk_tlvs=(
                CIR_BYTES[FIRST_PACKET_CIR_BYTES:FIRST_PACKET_CIR_BYTES + TLV_MAX_BYTES],
                b"",
                CIR_BYTES[FIRST_PACKET_CIR_BYTES + TLV_MAX_BYTES:],
            ),
        )

        result = reassembler.ingest(packet)

        self.assertIsNotNone(result)
        assert result is not None
        self.assertFalse(result.accepted)
        self.assertIsNone(result.key)
        self.assertIsNone(result.view)
        self.assertEqual(
            result.errors,
            ("UWB_CIR_FULL_CHUNK TLV occurrence 1 must not be empty",),
        )

    def test_truncated_repeated_chunk_tail_rejects_whole_packet_fragment(self) -> None:
        reassembler = CirReassembler()

        result = reassembler.ingest(truncated_first_fragment_packet())

        self.assertIsNotNone(result)
        assert result is not None
        self.assertFalse(result.accepted)
        self.assertIsNone(result.key)
        self.assertIsNone(result.view)
        self.assertEqual(len(result.errors), 1)
        self.assertIn("invalid UWB_CIR_FULL_CHUNK TLV occurrence 3", result.errors[0])
        self.assertIn("truncated", result.errors[0])

    def test_non_cir_diagnostic_fragment_is_not_claimed(self) -> None:
        reassembler = CirReassembler()

        result = reassembler.ingest(non_cir_diagnostic_fragment_packet())

        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
