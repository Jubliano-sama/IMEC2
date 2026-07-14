from __future__ import annotations

import unittest
from typing import Any
from unittest.mock import Mock, patch

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.cir_reassembly import CirReassembler
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    DEFAULT_HOST_ID,
    MSG_COMMAND_RESULT,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_DISCOVERY_ASSIGNMENT_EPOCH,
    TLV_DISCOVERY_ASSIGNMENT_PHASE,
    TLV_REASON,
    append_tlv,
    encode_cobs_packet,
    parse_cobs_packet,
)


class FakeVariable:
    def __init__(self, value: Any = "") -> None:
        self.value = value

    def get(self) -> Any:
        return self.value

    def set(self, value: Any) -> None:
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


def assignment_phase_packet(phase: int, epoch: int):
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_ASSIGN_DISCOVERY_SLOTS.to_bytes(2, "little"))
    append_tlv(payload, TLV_COMMAND_STATUS, (0).to_bytes(2, "little"))
    append_tlv(payload, TLV_REASON, b"\x00")
    append_tlv(payload, TLV_DISCOVERY_ASSIGNMENT_PHASE, bytes((phase,)))
    append_tlv(payload, TLV_DISCOVERY_ASSIGNMENT_EPOCH, epoch.to_bytes(4, "little"))
    frame = encode_cobs_packet(
        msg_type=MSG_COMMAND_RESULT,
        flags=0,
        src_id=0x2222222222222301,
        dst_id=0x9999888877776666,
        session_id=epoch,
        seq=8,
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

        self.assertIn("status=6 (RADIO_ERROR)", summary)
        self.assertIn("reason=1", summary)
        self.assertNotIn("assigned_anchors=", summary)

    def test_assignment_phase_is_not_labeled_as_terminal_anchor_count(self) -> None:
        summary = self.gui_model()._packet_summary(
            assignment_phase_packet(phase=3, epoch=0x11223344)
        )

        self.assertIn("phase=3 (ACK)", summary)
        self.assertIn("epoch=287454020", summary)
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

    def test_auto_survey_id_is_fresh_for_each_send_with_frozen_clocks(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._survey_id_counter = 1234
        gui._used_survey_ids = set()
        gui.survey_id_auto = FakeVariable(True)  # type: ignore[assignment]
        gui.survey_id_text = FakeVariable("1234")  # type: ignore[assignment]
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.command_budget_text = FakeVariable("")  # type: ignore[assignment]
        gui.duration_text = FakeVariable("250")  # type: ignore[assignment]
        gui.discovery_slots_text = FakeVariable("6")  # type: ignore[assignment]
        gui.sample_count_text = FakeVariable("1")  # type: ignore[assignment]
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.transport = Mock()
        gui.__dict__["_begin_gateway_command"] = Mock(return_value=True)
        gui.__dict__["_show_error"] = Mock()
        command = Mock(frame=b"frame", label="survey")

        with patch("tools.gateway_gui.app.time.time_ns", return_value=99), \
             patch("tools.gateway_gui.app.time.monotonic_ns", return_value=99), \
             patch("tools.gateway_gui.app.build_anchor_discovery_command",
                   return_value=command) as builder:
            gui._send_discovery()
            gui._send_discovery()

        survey_ids = [call.kwargs["survey_id"] for call in builder.call_args_list]
        self.assertEqual(len(survey_ids), 2)
        self.assertNotEqual(survey_ids[0], survey_ids[1])
        self.assertTrue(all(1 <= survey_id <= 0xFFFFFFFF for survey_id in survey_ids))
        self.assertEqual(gui.survey_id_text.get(), str(survey_ids[-1]))
        self.assertEqual(gui.transport.send_frame.call_count, 2)

    def test_auto_survey_id_wraps_past_zero(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui._survey_id_counter = 0xFFFFFFFF
        gui._used_survey_ids = set()
        gui.survey_id_auto = FakeVariable(True)  # type: ignore[assignment]
        gui.survey_id_text = FakeVariable("invalid")  # type: ignore[assignment]

        self.assertEqual(gui._survey_id_for_send(), 1)
        self.assertEqual(gui._survey_id_for_send(), 2)
        self.assertEqual(gui.survey_id_text.get(), "2")

    def test_manual_survey_id_mode_honors_exact_entry(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui._survey_id_counter = 1234
        gui._used_survey_ids = set()
        gui.survey_id_auto = FakeVariable(False)  # type: ignore[assignment]
        gui.survey_id_text = FakeVariable("0xabcdef01")  # type: ignore[assignment]

        self.assertEqual(gui._survey_id_for_send(), 0xABCDEF01)
        self.assertEqual(gui._survey_id_for_send(), 0xABCDEF01)
        self.assertEqual(gui.survey_id_text.get(), "0xabcdef01")
        self.assertEqual(gui._survey_id_counter, 1234)


if __name__ == "__main__":
    unittest.main()
