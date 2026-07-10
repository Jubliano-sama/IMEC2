from __future__ import annotations

import unittest
from typing import Any

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.cir_reassembly import CirReassembler
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    DEFAULT_HOST_ID,
    MSG_COMMAND_RESULT,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_REASON,
    append_tlv,
    encode_cobs_packet,
    parse_cobs_packet,
)


class FakeVariable:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: str) -> None:
        self.value = value


class FakeWidget:
    def __init__(self) -> None:
        self.options: dict[str, Any] = {}

    def configure(self, **options: Any) -> None:
        self.options.update(options)


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

    @staticmethod
    def identity_gui_model() -> GatewayGui:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = False
        gui.gateway_id = None
        gui.gateway_id_text = FakeVariable("Unavailable")  # type: ignore[assignment]
        gui.gateway_id_source = FakeVariable()  # type: ignore[assignment]
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.connection_text = FakeVariable()  # type: ignore[assignment]
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.connection_label = FakeWidget()  # type: ignore[assignment]
        gui.connect_button = FakeWidget()  # type: ignore[assignment]
        gui.disconnect_button = FakeWidget()  # type: ignore[assignment]
        gui.discovery_button = FakeWidget()  # type: ignore[assignment]
        gui.refresh_button = FakeWidget()  # type: ignore[assignment]
        gui.assignment_button = FakeWidget()  # type: ignore[assignment]
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

    def test_commands_require_current_connection_identity_and_clear_on_disconnect(self) -> None:
        gui = self.identity_gui_model()
        gateway_id = 0xAABBCCDDEEFF0011

        gui._set_connection_state("connecting")
        self.assertEqual(gui.discovery_button.options["state"], "disabled")  # type: ignore[attr-defined]
        self.assertIsNone(gui._accept_gateway_identity(gateway_id, "GATT identity"))
        self.assertEqual(gui.discovery_button.options["state"], "disabled")  # type: ignore[attr-defined]

        gui._set_connection_state("connected")
        self.assertEqual(gui._require_gateway_identity(), gateway_id)
        self.assertEqual(gui.discovery_button.options["state"], "normal")  # type: ignore[attr-defined]
        self.assertEqual(gui.refresh_button.options["state"], "normal")  # type: ignore[attr-defined]
        self.assertEqual(gui.assignment_button.options["state"], "normal")  # type: ignore[attr-defined]

        gui._set_connection_state("disconnected")
        self.assertIsNone(gui.gateway_id)
        self.assertEqual(gui.discovery_button.options["state"], "disabled")  # type: ignore[attr-defined]
        with self.assertRaisesRegex(ValueError, "Connect to a gateway"):
            gui._require_gateway_identity()

    def test_packet_identity_contradiction_invalidates_connected_identity(self) -> None:
        gui = self.identity_gui_model()
        gui.connected = True
        self.assertIsNone(gui._accept_gateway_identity(0x1111222233334444, "GATT identity"))
        errors: list[str] = []
        gui.__dict__["_show_error"] = errors.append

        gui._observe_gateway_id(assignment_result_packet(status=0, reason=6))

        self.assertIsNone(gui.gateway_id)
        self.assertEqual(gui.refresh_button.options["state"], "disabled")  # type: ignore[attr-defined]
        self.assertEqual(len(errors), 1)
        self.assertIn("identity contradiction", errors[0])


if __name__ == "__main__":
    unittest.main()
