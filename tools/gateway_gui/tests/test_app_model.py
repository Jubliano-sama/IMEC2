from __future__ import annotations

from dataclasses import replace
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any, cast
from unittest.mock import Mock, patch

import tools.gateway_gui.app as gateway_app
from tools.gateway_gui.app import DEFAULT_COMMAND_BUDGET_TEXT, GatewayGui
from tools.gateway_gui.cir_reassembly import CirReassembler
from tools.gateway_gui.command_orchestration import (
    GATEWAY_COMMAND_COMPLETION_GUARD_S,
    GatewayCommandDispatch,
    GatewayCommandOrchestrator,
    GatewayCommandPlan,
)
from tools.gateway_gui.command_telemetry import (
    GatewayCommandEvent,
    GatewayCommandRequestTracker,
)
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    CMD_SURVEY_REACHABILITY,
    DEFAULT_HOST_ID,
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    FLAG_GATEWAY_ACK_REQUIRED,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    Packet,
    SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    TLV_COMMAND_ID,
    TLV_COMMAND_BUDGET_MS,
    TLV_COMMAND_STATUS,
    TLV_DISCOVERY_ASSIGNMENT_EPOCH,
    TLV_DISCOVERY_ASSIGNMENT_PHASE,
    TLV_EXPECTED_NODE_COUNT,
    TLV_OPERATION_POLICY,
    TLV_REASON,
    append_tlv,
    encode_cobs_packet,
    parse_cobs_packet,
    parse_stream_record,
    parse_tlvs,
)
from tools.gateway_gui.tests.test_protocol import (
    gateway_assignment_event_payload,
    stream_record,
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
    def test_app_binds_the_survey_sample_default_used_during_construction(self) -> None:
        self.assertEqual(
            gateway_app.SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
        )

    @staticmethod
    def set_default_policy_variables(
        gui: GatewayGui, *, expected_anchors: str = ""
    ) -> None:
        gui.assignment_expected_anchors_text = FakeVariable(expected_anchors)  # type: ignore[assignment]
        gui.assignment_budget_text = FakeVariable(
            str(gateway_app.ASSIGNMENT_DEFAULT_BUDGET_MS)
        )  # type: ignore[assignment]
        gui.assignment_response_spread_text = FakeVariable("1000")  # type: ignore[assignment]
        gui.discovery_start_delay_text = FakeVariable(  # type: ignore[assignment]
            str(gateway_app.DISCOVERY_DEFAULT_START_DELAY_MS)
        )
        gui.discovery_slot_ms_text = FakeVariable("40")  # type: ignore[assignment]
        gui.discovery_slots_text = FakeVariable("6")  # type: ignore[assignment]
        gui.discovery_round_count_text = FakeVariable("4")  # type: ignore[assignment]
        gui.duration_text = FakeVariable("250")  # type: ignore[assignment]
        gui.discovery_budget_text = FakeVariable(
            str(gateway_app.DISCOVERY_DEFAULT_BUDGET_MS)
        )  # type: ignore[assignment]
        gui.pair_max_reruns_text = FakeVariable("2")  # type: ignore[assignment]
        gui.pair_max_parallel_text = FakeVariable("auto (25)")  # type: ignore[assignment]

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

    def test_conflicting_click_remains_visible_without_mutating_canonical_models(
        self,
    ) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock()
        gui.__dict__["_packet_summary"] = Mock(return_value="click")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()
        canonical = Packet(
            transport="test",
            raw_transport=b"",
            raw_packet=None,
            msg_type=MSG_CLICK_REPORT,
            flags=0x24,
            src_id=0x1111,
            dst_id=0x2222,
            session_id=7,
            seq=3,
            ttl=4,
            age_ms=0,
            age_kind="test",
            payload=b"canonical",
            tlvs=(),
        )
        conflict = replace(canonical, payload=b"changed")

        gui._add_packet(canonical)
        gui._add_packet(conflict)

        cast(Mock, gui._observe_diagnostic_packet).assert_called_once_with(
            canonical, received_at=None
        )
        gui.cir_reassembler.ingest.assert_called_once_with(canonical)
        self.assertEqual(gui.packet_tree.insert.call_count, 2)
        self.assertIs(gui.packet_by_iid["packet-2"], conflict)
        self.assertTrue(any(
            "Conflicting click report" in call.args[1]
            for call in cast(Mock, gui._append_log).call_args_list
        ))

    def test_reliable_non_click_replay_is_suppressed_and_conflict_stays_forensic(
        self,
    ) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.gateway_id = 0xAAA
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock()
        gui.__dict__["_packet_summary"] = Mock(return_value="command result")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()
        canonical = Packet(
            transport="test",
            raw_transport=b"canonical-record",
            raw_packet=None,
            msg_type=MSG_COMMAND_RESULT,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=0x1111,
            dst_id=0x2222,
            session_id=7,
            seq=3,
            ttl=4,
            age_ms=10,
            age_kind="gateway_queue_age_ms",
            payload=b"canonical",
            tlvs=(),
        )
        replay = replace(
            canonical,
            transport="gateway-stream-v1",
            raw_transport=b"replayed-record",
            age_ms=900,
        )
        conflict = replace(canonical, payload=b"changed")

        gui._add_packet(canonical)
        gui._add_packet(replay)

        cast(Mock, gui._observe_diagnostic_packet).assert_called_once_with(
            canonical, received_at=None
        )
        gui.cir_reassembler.ingest.assert_called_once_with(canonical)
        self.assertEqual(gui.packet_tree.insert.call_count, 1)
        self.assertEqual(gui.packet_counter, 1)

        gui._add_packet(conflict)

        # The conflicting packet remains visible, but it cannot trigger a
        # second diagnostic/model or CIR mutation.
        cast(Mock, gui._observe_diagnostic_packet).assert_called_once_with(
            canonical, received_at=None
        )
        gui.cir_reassembler.ingest.assert_called_once_with(canonical)
        self.assertEqual(gui.packet_tree.insert.call_count, 2)
        self.assertEqual(gui.packet_counter, 2)
        self.assertIs(gui.packet_by_iid["packet-2"], conflict)
        self.assertTrue(any(
            "Conflicting command result" in call.args[1]
            for call in cast(Mock, gui._append_log).call_args_list
        ))

    def test_host_receipt_retries_after_reconnect_but_conflict_stays_unreceipted(
        self,
    ) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.gateway_id = 0x2222
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.transport = Mock()
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock()
        gui.__dict__["_packet_summary"] = Mock(return_value="command result")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()
        canonical = Packet(
            transport="gateway-stream-v1",
            raw_transport=b"immutable-stream-record",
            raw_packet=None,
            msg_type=MSG_COMMAND_RESULT,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=0x1111,
            dst_id=0x2222,
            session_id=7,
            seq=3,
            ttl=None,
            age_ms=10,
            age_kind="gateway_queue_age_ms",
            payload=b"canonical",
            tlvs=(),
        )
        replay = replace(canonical, age_ms=900)
        conflict = replace(canonical, payload=b"changed")
        receipt = SimpleNamespace(frame=b"exact-cobs-receipt")

        with patch(
            "tools.gateway_gui.app.build_gateway_host_receipt",
            return_value=receipt,
        ) as builder:
            gui._add_packet(canonical)
            # Reconnect temporarily clears the GATT identity. The same packet
            # destination keeps the RAM scope and permits a receipt retry.
            gui.gateway_id = None
            gui._add_packet(replay)
            gui._add_packet(conflict)

        self.assertEqual(builder.call_count, 2)
        self.assertEqual(
            [call.kwargs for call in builder.call_args_list],
            [
                {
                    "host_id": DEFAULT_HOST_ID,
                    "gateway_id": 0x2222,
                },
                {
                    "host_id": DEFAULT_HOST_ID,
                    "gateway_id": 0x2222,
                },
            ],
        )
        self.assertEqual(
            [call.args for call in gui.transport.send_frame.call_args_list],
            [
                (b"exact-cobs-receipt", "gateway host receipt"),
                (b"exact-cobs-receipt", "gateway host receipt"),
            ],
        )
        cast(Mock, gui._observe_diagnostic_packet).assert_called_once_with(
            canonical, received_at=None
        )
        self.assertEqual(gui.cir_reassembler.ingest.call_count, 1)
        self.assertEqual(gui.packet_tree.insert.call_count, 2)
        self.assertIs(gui.packet_by_iid["packet-2"], conflict)
        self.assertTrue(any(
            "Conflicting command result" in call.args[1]
            for call in cast(Mock, gui._append_log).call_args_list
        ))

    def test_command_event_semantic_replay_receipts_without_reapplying_gui_state(
        self,
    ) -> None:
        gateway_id = 0x9999AAAABBBBCCCC
        gui = GatewayGui.__new__(GatewayGui)
        gui.gateway_id = gateway_id
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.transport = Mock()
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock()
        gui.__dict__["_packet_summary"] = Mock(return_value="command event")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()

        def command_event(event_sequence: int, *, attempt: int, replay: bool) -> Packet:
            payload = bytearray(
                gateway_assignment_event_payload(event_sequence=event_sequence)
            )
            payload[4] = 0x04 if replay else 0
            payload[5] = attempt
            return parse_stream_record(
                stream_record(
                    bytes(payload),
                    msg_type=MSG_GATEWAY_COMMAND_EVENT,
                    packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                    packet_src_id=gateway_id,
                    packet_dst_id=gateway_id,
                    packet_session_id=event_sequence,
                    packet_seq=event_sequence & 0xFFFF,
                )
            )

        canonical = command_event(0x10203040, attempt=0, replay=False)
        replay = command_event(0x50607080, attempt=0, replay=True)
        with patch(
            "tools.gateway_gui.app.build_gateway_host_receipt",
            return_value=SimpleNamespace(frame=b"command-event-receipt"),
        ) as builder:
            gui._add_packet(canonical)
            gui._add_packet(replay)

        cast(Mock, gui._observe_diagnostic_packet).assert_called_once_with(
            canonical, received_at=None
        )
        self.assertEqual(gui.packet_tree.insert.call_count, 1)
        self.assertEqual(builder.call_count, 2)
        self.assertEqual(gui.transport.send_frame.call_count, 2)

    def test_unknown_gatt_identity_uses_stream_destination_for_receipt_scope(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.gateway_id = None
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.transport = Mock()
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock()
        gui.__dict__["_packet_summary"] = Mock(return_value="stream")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()
        packet = Packet(
            transport="gateway-stream-v1",
            raw_transport=b"stream",
            raw_packet=None,
            msg_type=MSG_COMMAND_RESULT,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=0x1111,
            dst_id=0x3333,
            session_id=8,
            seq=4,
            ttl=None,
            age_ms=0,
            age_kind="gateway_queue_age_ms",
            payload=b"result",
            tlvs=(),
        )

        with patch(
            "tools.gateway_gui.app.build_gateway_host_receipt",
            return_value=SimpleNamespace(frame=b"receipt"),
        ) as builder:
            gui._add_packet(packet)

        self.assertEqual(gui.delivery_dedup.gateway_id, 0x3333)
        builder.assert_called_once_with(
            packet,
            host_id=DEFAULT_HOST_ID,
            gateway_id=0x3333,
        )
        gui.transport.send_frame.assert_called_once_with(
            b"receipt", "gateway host receipt"
        )

    def test_host_receipt_skips_best_effort_non_stream_and_gateway_local_records(
        self,
    ) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.gateway_id = 0x2222
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.transport = Mock()
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock()
        gui.__dict__["_packet_summary"] = Mock(return_value="record")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()

        def packet(*, transport: str, msg_type: int, flags: int, seq: int) -> Packet:
            return Packet(
                transport=transport,
                raw_transport=f"record-{seq}".encode(),
                raw_packet=None,
                msg_type=msg_type,
                flags=flags,
                src_id=0x1111,
                dst_id=0x2222,
                session_id=9,
                seq=seq,
                ttl=None,
                age_ms=0,
                age_kind="gateway_queue_age_ms",
                payload=b"record",
                tlvs=(),
            )

        with patch("tools.gateway_gui.app.build_gateway_host_receipt") as builder:
            gui._add_packet(
                packet(
                    transport="gateway-stream-v1",
                    msg_type=MSG_CLICK_REPORT,
                    flags=0,
                    seq=1,
                )
            )
            gui._add_packet(
                packet(
                    transport="cobs-shared-packet",
                    msg_type=MSG_COMMAND_RESULT,
                    flags=FLAG_GATEWAY_ACK_REQUIRED,
                    seq=2,
                )
            )
            gui._add_packet(
                packet(
                    transport="gateway-stream-v1",
                    msg_type=MSG_GATEWAY_COMMAND_EVENT,
                    flags=0,
                    seq=3,
                )
            )

        builder.assert_not_called()
        gui.transport.send_frame.assert_not_called()

    def test_failed_semantic_apply_does_not_commit_or_receipt_a_new_record(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.gateway_id = 0x2222
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.transport = Mock()
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.packet_tree = Mock()
        gui.packet_tree.get_children.return_value = ()
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.__dict__["_append_log"] = Mock()
        gui.__dict__["_observe_diagnostic_packet"] = Mock(
            side_effect=RuntimeError("model apply failed")
        )
        gui.__dict__["_packet_summary"] = Mock(return_value="result")
        gui.__dict__["_diagnostic_packet_tags"] = Mock(return_value=())
        gui.__dict__["_register_diagnostic_packet_row"] = Mock()
        gui.__dict__["_forget_diagnostic_packet_row"] = Mock()
        gui.__dict__["_observe_gateway_id"] = Mock()
        gui.__dict__["_refresh_selected_cir"] = Mock()
        packet = Packet(
            transport="gateway-stream-v1",
            raw_transport=b"stream-record",
            raw_packet=None,
            msg_type=MSG_COMMAND_RESULT,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=0x1111,
            dst_id=0x2222,
            session_id=10,
            seq=5,
            ttl=None,
            age_ms=0,
            age_kind="gateway_queue_age_ms",
            payload=b"result",
            tlvs=(),
        )

        with patch(
            "tools.gateway_gui.app.build_gateway_host_receipt",
            return_value=SimpleNamespace(frame=b"receipt"),
        ) as builder:
            with self.assertRaisesRegex(RuntimeError, "model apply failed"):
                gui._add_packet(packet)

            # The failed semantic path left no authoritative RAM entry and no
            # receipt, so the replay must apply normally and become receipted.
            self.assertEqual(gui.delivery_dedup.size, 0)
            gui.transport.send_frame.assert_not_called()
            cast(Mock, gui._observe_diagnostic_packet).side_effect = None
            gui._add_packet(packet)

        self.assertEqual(gui.delivery_dedup.size, 1)
        self.assertEqual(cast(Mock, gui._observe_diagnostic_packet).call_count, 2)
        builder.assert_called_once_with(
            packet,
            host_id=DEFAULT_HOST_ID,
            gateway_id=0x2222,
        )
        gui.transport.send_frame.assert_called_once_with(
            b"receipt", "gateway host receipt"
        )

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
        self.set_default_policy_variables(gui)
        gui.sample_count_text = FakeVariable("5")  # type: ignore[assignment]
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.transport = Mock()
        gui.geometry_model = Mock()
        gui.geometry_model.generation = 1
        gui.click_location_model = Mock()
        gui.click_location_model.state = Mock()
        gui.anchor_geometry_view = Mock()
        gui.click_diagnostics_view = Mock()
        submit_command = Mock(return_value=True)
        gui.__dict__["_submit_gateway_command"] = submit_command
        gui.__dict__["_show_error"] = Mock()
        command = Mock(
            frame=b"survey-frame",
            label="survey",
            command_id=CMD_SURVEY_REACHABILITY,
        )
        here_i_am = Mock(
            frame=b"route-frame",
            label="route",
            command_id=CMD_FORCE_REDISCOVERY,
        )

        with patch("tools.gateway_gui.app.time.time_ns", return_value=99), \
             patch("tools.gateway_gui.app.time.monotonic_ns", return_value=99), \
             patch("tools.gateway_gui.app.build_anchor_discovery_command",
                   return_value=command) as builder, \
             patch("tools.gateway_gui.app.build_here_i_am_command",
                   return_value=here_i_am):
            gui._send_discovery()
            gui._send_discovery()

        survey_ids = [call.kwargs["survey_id"] for call in builder.call_args_list]
        self.assertEqual(len(survey_ids), 2)
        self.assertNotEqual(survey_ids[0], survey_ids[1])
        self.assertTrue(all(1 <= survey_id <= 0xFFFFFFFF for survey_id in survey_ids))
        self.assertEqual(gui.survey_id_text.get(), str(survey_ids[-1]))
        self.assertEqual(submit_command.call_count, 2)
        plans = [call.args[0] for call in submit_command.call_args_list]
        self.assertTrue(all(plan.preflight is not None for plan in plans))
        self.assertTrue(all(
            plan.preflight.session_id != plan.target.session_id for plan in plans
        ))
        gui.geometry_model.begin_survey.assert_not_called()
        plans[-1].target.on_dispatch()
        gui.geometry_model.begin_survey.assert_called_once_with(
            survey_ids[-1],
            host_session_id=plans[-1].target.session_id,
            host_sequence=plans[-1].target.sequence,
        )

    def test_survey_frame_and_geometry_wait_for_successful_here_i_am(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui._survey_id_counter = 100
        gui._used_survey_ids = set()
        gui.survey_id_auto = FakeVariable(True)  # type: ignore[assignment]
        gui.survey_id_text = FakeVariable("100")  # type: ignore[assignment]
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.command_budget_text = FakeVariable(  # type: ignore[assignment]
            str(GATEWAY_COMMAND_BUDGET_MAX_MS)
        )
        self.set_default_policy_variables(gui, expected_anchors="2")
        gui.sample_count_text = FakeVariable("5")  # type: ignore[assignment]
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.transport = Mock()
        gui.geometry_model = Mock()
        gui.geometry_model.generation = 1
        gui.click_location_model = Mock()
        gui.click_location_model.state = Mock()
        gui.anchor_geometry_view = Mock()
        gui.click_diagnostics_view = Mock()
        gui.command_request_tracker = GatewayCommandRequestTracker()
        gui.command_orchestrator = GatewayCommandOrchestrator(
            gui.command_request_tracker
        )
        gui.__dict__["_update_command_state"] = Mock()
        gui.__dict__["_show_error"] = Mock()

        with patch("tools.gateway_gui.app.time.monotonic_ns", return_value=99):
            gui._send_discovery()

        self.assertEqual(gui.transport.send_frame.call_count, 1)
        gui.geometry_model.begin_survey.assert_not_called()
        preflight = gui.command_orchestrator.current
        self.assertIsNotNone(preflight)
        assert preflight is not None
        self.assertEqual(preflight.command_id, CMD_FORCE_REDISCOVERY)
        preflight_policy = tuple(
            value.raw for value in parse_cobs_packet(preflight.frame).tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )
        self.assertEqual(len(preflight_policy), 3)
        route_terminal = GatewayCommandEvent(
            preflight.command_kind, 12, 1, 1, 0, 0,
            preflight.command_id, 0, preflight.session_id, 1,
            preflight.session_id, preflight.sequence, 1,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        )

        gui._apply_gateway_command_transition(
            gui.command_orchestrator.observe_event(route_terminal)
        )

        self.assertEqual(gui.transport.send_frame.call_count, 2)
        target = gui.command_orchestrator.current
        self.assertIsNotNone(target)
        assert target is not None
        self.assertEqual(target.command_id, CMD_SURVEY_REACHABILITY)
        self.assertNotEqual(target.session_id, preflight.session_id)
        target_policy = tuple(
            value.raw for value in parse_cobs_packet(target.frame).tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )
        self.assertEqual(target_policy, preflight_policy[1:])
        self.assertEqual(
            parse_cobs_packet(target.frame).value(TLV_EXPECTED_NODE_COUNT),
            2,
        )
        self.assertEqual(
            parse_cobs_packet(target.frame).value(TLV_COMMAND_BUDGET_MS),
            GATEWAY_COMMAND_BUDGET_MAX_MS,
        )
        self.assertEqual(
            target.timeout_s,
            GATEWAY_COMMAND_BUDGET_MAX_MS / 1000.0
            + GATEWAY_COMMAND_COMPLETION_GUARD_S,
        )
        gui.geometry_model.begin_survey.assert_called_once_with(
            101,
            host_session_id=target.session_id,
            host_sequence=target.sequence,
        )

    def test_assignment_is_snapshotted_behind_distinct_here_i_am_preflight(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.command_budget_text = FakeVariable("")  # type: ignore[assignment]
        self.set_default_policy_variables(gui, expected_anchors="5")
        submit_command = Mock(return_value=True)
        show_error = Mock()
        gui.__dict__["_submit_gateway_command"] = submit_command
        gui.__dict__["_show_error"] = show_error

        with patch("tools.gateway_gui.app.time.monotonic_ns", return_value=123):
            gui._send_assign_discovery_slots()

        show_error.assert_not_called()
        plan = submit_command.call_args.args[0]
        self.assertEqual(plan.target.command_id, CMD_ASSIGN_DISCOVERY_SLOTS)
        self.assertIsNotNone(plan.preflight)
        assert plan.preflight is not None
        self.assertEqual(plan.preflight.command_id, CMD_FORCE_REDISCOVERY)
        self.assertNotEqual(plan.preflight.session_id, plan.target.session_id)
        target_policy = tuple(
            value.raw for value in parse_cobs_packet(plan.target.frame).tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )
        preflight_policy = tuple(
            value.raw for value in parse_cobs_packet(plan.preflight.frame).tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )
        self.assertEqual(len(target_policy), 1)
        self.assertEqual(target_policy, preflight_policy[:1])
        self.assertEqual(
            plan.target.timeout_s,
            DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS / 1000.0
            + GATEWAY_COMMAND_COMPLETION_GUARD_S,
        )
        self.assertEqual(
            plan.target.status_text,
            "Enumerating 5 expected anchors and assigning discovery slots...",
        )
        self.assertEqual(plan.preflight.timeout_s, 125.0)

    def test_assignment_unknown_roster_explains_full_horizon(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.command_budget_text = FakeVariable("")  # type: ignore[assignment]
        self.set_default_policy_variables(gui)
        submit_command = Mock(return_value=True)
        show_error = Mock()
        gui.__dict__["_submit_gateway_command"] = submit_command
        gui.__dict__["_show_error"] = show_error

        gui._send_assign_discovery_slots()

        show_error.assert_not_called()
        plan = submit_command.call_args.args[0]
        self.assertEqual(
            plan.target.status_text,
            "Enumerating an unknown anchor roster across the full 8-hop "
            "horizon; set Expected anchors for fast completion...",
        )

    def test_manual_here_i_am_carries_the_current_full_policy(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.command_budget_text = FakeVariable("")  # type: ignore[assignment]
        self.set_default_policy_variables(gui, expected_anchors="5")
        gui.assignment_response_spread_text = FakeVariable("750")  # type: ignore[assignment]
        gui.discovery_round_count_text = FakeVariable("3")  # type: ignore[assignment]
        gui.pair_max_parallel_text = FakeVariable("8")  # type: ignore[assignment]
        submit_command = Mock(return_value=True)
        gui.__dict__["_submit_gateway_command"] = submit_command
        gui.__dict__["_show_error"] = Mock()

        with patch("tools.gateway_gui.app.time.monotonic_ns", return_value=456):
            gui._send_here_i_am()

        plan = submit_command.call_args.args[0]
        self.assertIsNone(plan.preflight)
        policies = tuple(
            value.decoded for value in parse_cobs_packet(plan.target.frame).tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )
        self.assertEqual(len(policies), 3)
        self.assertEqual(policies[0]["expected_anchor_count"], 5)
        self.assertEqual(policies[0]["response_spread_ms"], 750)
        self.assertEqual(policies[1]["round_count"], 3)
        self.assertEqual(policies[2]["max_parallel_pairs"], 8)

    def test_accepted_survey_telemetry_refreshes_geometry_view(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.command_timeline_model = Mock()
        gui.command_request_tracker = GatewayCommandRequestTracker()
        gui.command_orchestrator = GatewayCommandOrchestrator(
            gui.command_request_tracker
        )
        gui.__dict__["_apply_gateway_command_transition"] = Mock()
        gui.mesh_diagnostics_view = Mock()
        gui.geometry_model = Mock()
        gui.geometry_model.observe_command_event.return_value = True
        gui.anchor_geometry_view = Mock()
        gui.topology_model = Mock()
        gui.topology_model.observe.return_value = None
        gui.__dict__["_show_error"] = Mock()
        telemetry_event = Mock()
        payload = b""
        telemetry_packet = Packet(
            "test", payload, None, MSG_GATEWAY_COMMAND_EVENT, 0, 1, 2, 3, 4,
            None, 0, "gateway_queue_age_ms", payload, parse_tlvs(payload),
        )

        with patch(
            "tools.gateway_gui.diagnostics_integration.decode_gateway_command_event",
            return_value=telemetry_event,
        ):
            gui._observe_diagnostic_packet(telemetry_packet)

        gui.geometry_model.observe_command_event.assert_called_once_with(telemetry_event)
        gui.anchor_geometry_view.show_model.assert_called_once_with(gui.geometry_model)

    def test_event_drain_consumes_received_packets_before_wall_clock_expiry(
        self,
    ) -> None:
        source = (
            Path(__file__).resolve().parents[1] / "app.py"
        ).read_text(encoding="utf-8")
        body_start = source.index("    def _drain_events")
        body_end = source.index("\n    def ", body_start + 1)
        body = source[body_start:body_end]

        self.assertLess(
            body.index("self._handle_event(event)"),
            body.index("self._expire_gateway_command()"),
        )

    def test_immediate_ok_result_does_not_release_command_before_typed_terminal(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.command_request_tracker = GatewayCommandRequestTracker()
        gui.command_orchestrator = GatewayCommandOrchestrator(
            gui.command_request_tracker
        )
        current = GatewayCommandDispatch(
            command_kind=1,
            command_id=CMD_ASSIGN_DISCOVERY_SLOTS,
            session_id=0x11223344,
            sequence=7,
            frame=b"assignment",
            label="assignment",
            timeout_s=10.0,
            status_text="assignment",
        )
        gui.command_orchestrator.begin(
            GatewayCommandPlan(target=current, preflight=None), now=0.0
        )
        gui.geometry_model = Mock()
        gui.geometry_model.observe_pair_packet.return_value = None
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        update_command_state = Mock()
        gui.__dict__["_update_command_state"] = update_command_state

        gui._observe_diagnostic_packet(assignment_result_packet(status=0, reason=0))

        self.assertIsNotNone(gui.command_request_tracker.pending)
        update_command_state.assert_not_called()

        gui._observe_diagnostic_packet(assignment_result_packet(status=2, reason=2))
        self.assertIsNone(gui.command_request_tracker.pending)
        update_command_state.assert_called_once()

    def test_auto_survey_id_wraps_past_zero(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui._survey_id_counter = 0xFFFFFFFF
        gui._used_survey_ids = set()
        gui.survey_id_auto = FakeVariable(True)  # type: ignore[assignment]
        gui.survey_id_text = FakeVariable("invalid")  # type: ignore[assignment]

        self.assertEqual(gui._survey_id_for_send(), 1)
        self.assertEqual(gui._survey_id_for_send(), 2)
        self.assertEqual(gui.survey_id_text.get(), "2")

    def test_blank_command_limit_uses_full_robust_gui_wait(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        self.assertEqual(DEFAULT_COMMAND_BUDGET_TEXT, "")
        gui.command_budget_text = FakeVariable(  # type: ignore[assignment]
            DEFAULT_COMMAND_BUDGET_TEXT
        )

        self.assertIsNone(gui._command_budget_ms())
        self.assertEqual(
            gui._command_timeout_s(None),
            GATEWAY_COMMAND_BUDGET_MAX_MS / 1000.0
            + GATEWAY_COMMAND_COMPLETION_GUARD_S,
        )
        self.assertEqual(
            gui._command_timeout_s(
                None,
                DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
            ),
            DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS / 1000.0
            + GATEWAY_COMMAND_COMPLETION_GUARD_S,
        )
        self.assertEqual(gui._command_timeout_s(20000), 25.0)

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
