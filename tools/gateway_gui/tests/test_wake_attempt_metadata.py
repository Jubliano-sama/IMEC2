import unittest

from tools.gateway_gui.diagnostic_models import PendingWakeAttemptAdapter
from tools.gateway_gui.protocol import (
    MSG_CLICK_REPORT,
    Packet,
    TLV_ATTEMPT_INDEX,
    TLV_DETECTION_SOURCE,
    parse_tlvs,
)


def packet_with(payload: bytes) -> Packet:
    return Packet(
        transport="test",
        raw_transport=b"",
        raw_packet=None,
        msg_type=MSG_CLICK_REPORT,
        flags=0,
        src_id=1,
        dst_id=2,
        session_id=3,
        seq=4,
        ttl=1,
        age_ms=0,
        age_kind="packet",
        payload=payload,
        tlvs=parse_tlvs(payload),
    )


class WakeAttemptMetadataTests(unittest.TestCase):
    def test_adapter_reads_explicit_uwb_attempt(self) -> None:
        packet = packet_with(bytes((TLV_ATTEMPT_INDEX, 1, 3,
                                    TLV_DETECTION_SOURCE, 1, 1)))
        self.assertEqual(PendingWakeAttemptAdapter().attempt(packet), 3)

    def test_adapter_keeps_legacy_and_unknown_source_packets_unknown(self) -> None:
        self.assertIsNone(PendingWakeAttemptAdapter().attempt(packet_with(b"")))
        packet = packet_with(bytes((TLV_ATTEMPT_INDEX, 1, 2,
                                    TLV_DETECTION_SOURCE, 1, 9)))
        self.assertIsNone(PendingWakeAttemptAdapter().attempt(packet))


if __name__ == "__main__":
    unittest.main()
