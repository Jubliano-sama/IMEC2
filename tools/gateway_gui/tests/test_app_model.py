from __future__ import annotations

import unittest

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.cir_reassembly import CirReassembler
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    MSG_COMMAND_RESULT,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_REASON,
    append_tlv,
    encode_cobs_packet,
    parse_cobs_packet,
)


def assignment_result_packet(status: int, reason: int):
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_ASSIGN_DISCOVERY_SLOTS.to_bytes(2, "little"))
    append_tlv(payload, TLV_COMMAND_STATUS, status.to_bytes(2, "little"))
    append_tlv(payload, TLV_REASON, bytes((reason,)))
    frame = encode_cobs_packet(
        msg_type=MSG_COMMAND_RESULT,
        flags=0,
        src_id=0xAABBCCDDEEFF0011,
        dst_id=0xA1C1BEEFC0DE0001,
        session_id=0x11223344,
        seq=7,
        ttl=1,
        payload=bytes(payload),
    )
    return parse_cobs_packet(frame)


class AppModelTests(unittest.TestCase):
    @staticmethod
    def gui_model() -> GatewayGui:
        gui = GatewayGui.__new__(GatewayGui)
        gui.cir_reassembler = CirReassembler()
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        return gui

    def test_successful_assignment_result_labels_assigned_anchor_count(self) -> None:
        summary = self.gui_model()._packet_summary(assignment_result_packet(status=0, reason=6))

        self.assertIn("command=ASSIGN_DISCOVERY_SLOTS", summary)
        self.assertIn("status=0 (OK)", summary)
        self.assertIn("assigned_anchors=6", summary)
        self.assertNotIn("reason=", summary)

    def test_failed_assignment_result_keeps_reason_semantics(self) -> None:
        summary = self.gui_model()._packet_summary(assignment_result_packet(status=6, reason=1))

        self.assertIn("status=6 (BUSY)", summary)
        self.assertIn("reason=1", summary)
        self.assertNotIn("assigned_anchors=", summary)


if __name__ == "__main__":
    unittest.main()
