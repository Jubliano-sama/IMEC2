import unittest

from tools.gateway_gui.command_telemetry import CommandTelemetryDecodeError, decode_gateway_command_event
from tools.gateway_gui.protocol import COMMAND_STATUS_NAMES, MSG_GATEWAY_COMMAND_EVENT
from tools.gateway_gui.tests.test_protocol import stream_record
from tools.gateway_gui.protocol import parse_stream_record


def event_payload():
    raw = bytearray(78)
    raw[0:8] = bytes((1, 78, 2, 9, 0, 3, 0, 0))
    raw[8:10] = (0x0100).to_bytes(2, "little")
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
        self.assertEqual(event.correlation_key, (2, 44, 7, 8))
        self.assertEqual(event.pair_initiator_id, 0xA1)
        self.assertEqual(event.total_count, 10)

    def test_unknown_schema_and_nonzero_reserved_are_rejected(self):
        for offset, value in ((0, 2), (26, 1)):
            raw = bytearray(event_payload()); raw[offset] = value
            packet = parse_stream_record(stream_record(bytes(raw), msg_type=MSG_GATEWAY_COMMAND_EVENT))
            with self.assertRaises(CommandTelemetryDecodeError):
                decode_gateway_command_event(packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES))

    def test_exact_length_and_all_unknown_enums_are_rejected(self):
        for offset, value in ((2, 4), (3, 13), (4, 0x80), (6, 0xFF), (7, 14)):
            with self.subTest(offset=offset):
                raw = bytearray(event_payload()); raw[offset] = value
                packet = parse_stream_record(stream_record(bytes(raw), msg_type=MSG_GATEWAY_COMMAND_EVENT))
                with self.assertRaises(CommandTelemetryDecodeError):
                    decode_gateway_command_event(packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES))
        short = parse_stream_record(stream_record(event_payload()[:-1], msg_type=MSG_GATEWAY_COMMAND_EVENT))
        with self.assertRaisesRegex(CommandTelemetryDecodeError, "exactly 78"):
            decode_gateway_command_event(short.payload, valid_statuses=set(COMMAND_STATUS_NAMES))


if __name__ == "__main__":
    unittest.main()
