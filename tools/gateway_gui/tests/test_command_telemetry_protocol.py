import unittest
from dataclasses import replace

from tools.gateway_gui.command_telemetry import (
    CommandTelemetryDecodeError, GatewayCommandEvent,
    GatewayCommandRequestTracker,
    decode_gateway_command_event,
    is_enumeration_count_mismatch,
)
from tools.gateway_gui.protocol import COMMAND_STATUS_NAMES, MSG_GATEWAY_COMMAND_EVENT
from tools.gateway_gui.tests.test_protocol import stream_record
from tools.gateway_gui.protocol import parse_stream_record


def event_payload():
    raw = bytearray(78)
    raw[0:8] = bytes((1, 78, 1, 6, 0, 3, 0, 0))
    raw[8:10] = (0x0104).to_bytes(2, "little")
    raw[12:16] = (44).to_bytes(4, "little")
    raw[16:20] = (99).to_bytes(4, "little")
    raw[20:24] = (7).to_bytes(4, "little")
    raw[24:26] = (8).to_bytes(2, "little")
    raw[28:32] = (123).to_bytes(4, "little")
    raw[40:48] = (0xA1).to_bytes(8, "little")
    raw[48:56] = (0xA2).to_bytes(8, "little")
    raw[64:66] = (4).to_bytes(2, "little")
    raw[66:68] = (10).to_bytes(2, "little")
    raw[77] = 255
    return bytes(raw)


class CommandTelemetryProtocolTests(unittest.TestCase):
    def test_fixed_payload_bypasses_tlv_parser_and_decodes(self):
        packet = parse_stream_record(stream_record(event_payload(), msg_type=MSG_GATEWAY_COMMAND_EVENT))
        self.assertEqual(packet.tlvs, ())
        event = decode_gateway_command_event(packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES))
        self.assertEqual(event.correlation_key, (1, 44, 7, 8))
        self.assertEqual(event.anchor_id, 0)
        self.assertEqual(event.total_count, 10)

    def test_unknown_schema_and_nonzero_reserved_are_rejected(self):
        for offset, value in ((0, 2), (26, 1)):
            raw = bytearray(event_payload()); raw[offset] = value
            packet = parse_stream_record(stream_record(bytes(raw), msg_type=MSG_GATEWAY_COMMAND_EVENT))
            with self.assertRaises(CommandTelemetryDecodeError):
                decode_gateway_command_event(packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES))

    def test_exact_length_and_all_unknown_enums_are_rejected(self):
        for offset, value in ((2, 4), (3, 13), (4, 0x80), (6, 0xFF), (7, 15)):
            with self.subTest(offset=offset):
                raw = bytearray(event_payload()); raw[offset] = value
                packet = parse_stream_record(stream_record(bytes(raw), msg_type=MSG_GATEWAY_COMMAND_EVENT))
                with self.assertRaises(CommandTelemetryDecodeError):
                    decode_gateway_command_event(packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES))
        short = parse_stream_record(stream_record(event_payload()[:-1], msg_type=MSG_GATEWAY_COMMAND_EVENT))
        with self.assertRaisesRegex(CommandTelemetryDecodeError, "exactly 78"):
            decode_gateway_command_event(short.payload, valid_statuses=set(COMMAND_STATUS_NAMES))

    def test_request_tracker_bounds_rapid_commands_and_recovers(self):
        tracker = GatewayCommandRequestTracker(timeout_s=10.0)
        self.assertTrue(tracker.begin(1, 100, 7, now=0.0))
        for sequence in range(8, 13):
            self.assertFalse(tracker.begin(1, 100, sequence, now=0.1))
        self.assertEqual(tracker.pending.host_sequence, 7)
        self.assertFalse(tracker.observe_command_result(100, 8, 2))
        self.assertTrue(tracker.observe_command_result(100, 7, 2))
        self.assertEqual(tracker.last_outcome, "failed")
        self.assertTrue(tracker.begin(1, 101, 8, now=1.0))

    def test_request_tracker_terminal_loss_timeout_disconnect_and_reconnect(self):
        tracker = GatewayCommandRequestTracker(timeout_s=5.0)
        self.assertTrue(tracker.begin(1, 200, 9, now=0.0))
        self.assertFalse(tracker.expire(now=4.9))
        self.assertTrue(tracker.expire(now=5.0))
        self.assertTrue(tracker.begin(1, 201, 10, now=6.0))
        tracker.disconnect()
        self.assertIsNone(tracker.pending)
        self.assertTrue(tracker.begin(1, 202, 11, now=7.0))

    def test_request_tracker_uses_each_commands_explicit_budget(self):
        tracker = GatewayCommandRequestTracker(timeout_s=100.0)
        self.assertTrue(tracker.begin(1, 210, 12, now=0.0, timeout_s=15.0))
        self.assertFalse(tracker.expire(now=14.9))
        self.assertTrue(tracker.expire(now=15.0))
        self.assertTrue(tracker.begin(2, 211, 13, now=16.0))
        self.assertFalse(tracker.expire(now=115.9))
        self.assertTrue(tracker.expire(now=116.0))

    def test_request_tracker_ignores_intermediate_and_duplicate_correlations(self):
        tracker = GatewayCommandRequestTracker()
        self.assertTrue(tracker.begin(1, 300, 12, now=0.0))
        intermediate = decode_gateway_command_event(event_payload(), valid_statuses=set(COMMAND_STATUS_NAMES))
        self.assertFalse(tracker.observe_event(intermediate))
        wrong = replace(
            intermediate, command_kind=1, stage=12, flags=1,
            host_session_id=301, host_sequence=12,
        )
        self.assertFalse(tracker.observe_event(wrong))
        terminal = replace(
            wrong, host_session_id=300, event_sequence=2,
            progress_count=1, success_count=1,
        )
        self.assertTrue(tracker.observe_event(terminal))
        self.assertFalse(tracker.observe_event(terminal))

    def test_expected_anchor_count_mismatch_completes_with_warning_semantics(self):
        tracker = GatewayCommandRequestTracker()
        self.assertTrue(tracker.begin(1, 300, 12, now=0.0))
        intermediate = decode_gateway_command_event(
            event_payload(), valid_statuses=set(COMMAND_STATUS_NAMES)
        )
        fewer = replace(
            intermediate,
            stage=12,
            flags=1,
            host_session_id=300,
            host_sequence=12,
            command_status=0,
            reason=6,
            total_count=4,
            success_count=3,
            failure_count=1,
        )

        self.assertTrue(is_enumeration_count_mismatch(fewer))
        self.assertTrue(tracker.observe_event(fewer))
        self.assertEqual(tracker.last_outcome, "complete")

        more = replace(
            fewer,
            reason=0,
            total_count=2,
            success_count=3,
            failure_count=0,
        )
        self.assertTrue(is_enumeration_count_mismatch(more))
        self.assertFalse(
            is_enumeration_count_mismatch(
                replace(fewer, command_status=3)
            )
        )


if __name__ == "__main__":
    unittest.main()
