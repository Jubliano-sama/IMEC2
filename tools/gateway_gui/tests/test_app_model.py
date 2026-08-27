from __future__ import annotations

from dataclasses import replace
import inspect
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any, cast
from unittest.mock import Mock, patch

import tools.gateway_gui.app as gateway_app
from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.cir_reassembly import CirReassembler
from tools.gateway_gui.command_orchestration import (
    GATEWAY_COMMAND_COMPLETION_GUARD_S,
    GatewayAssignmentReplayBarrier,
    GatewayCommandDispatch,
    GatewayCommandOrchestrator,
    GatewayCommandPlan,
)
from tools.gateway_gui.command_telemetry import (
    GatewayCommandEvent,
    GatewayCommandRequestTracker,
)
from tools.gateway_gui.diagnostic_models import CommandTimelineModel
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    DEFAULT_HOST_ID,
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    FLAG_GATEWAY_ACK_REQUIRED,
    GATEWAY_COMMAND_EVENT_FLAG_TERMINAL,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    Packet,
    parse_stream_record,
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

    @staticmethod
    def set_default_policy_variables(
        gui: GatewayGui, *, expected_anchors: str = ""
    ) -> None:
        gui.assignment_expected_anchors_text = FakeVariable(expected_anchors)  # type: ignore[assignment]
        gui._topology_deepest_hop = 0
        gui.assignment_budget_text = FakeVariable(
            str(gateway_app.ASSIGNMENT_DEFAULT_BUDGET_MS)
        )  # type: ignore[assignment]
        gui.assignment_response_spread_text = FakeVariable("1000")  # type: ignore[assignment]

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
        gui._topology_deepest_hop = 0
        gui.gateway_id_text = FakeVariable("Unavailable")  # type: ignore[assignment]
        gui.gateway_id_source = FakeVariable()  # type: ignore[assignment]
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.connection_text = FakeVariable()  # type: ignore[assignment]
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.connection_label = FakeWidget()  # type: ignore[assignment]
        gui.connect_button = FakeWidget()  # type: ignore[assignment]
        gui.disconnect_button = FakeWidget()  # type: ignore[assignment]
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
        self.assertIsNone(gui._accept_gateway_identity(gateway_id, "GATT identity"))

        gui._set_connection_state("connected")
        self.assertEqual(gui._require_gateway_identity(), gateway_id)
        self.assertEqual(gui.refresh_button.options["state"], "normal")  # type: ignore[attr-defined]
        self.assertEqual(gui.assignment_button.options["state"], "normal")  # type: ignore[attr-defined]

        gui._set_connection_state("disconnected")
        self.assertIsNone(gui.gateway_id)
        with self.assertRaisesRegex(ValueError, "Connect to a gateway"):
            gui._require_gateway_identity()

    def test_same_gateway_reconnect_preserves_enumeration_topology(self) -> None:
        gui = self.identity_gui_model()
        self.set_default_policy_variables(gui, expected_anchors="4")
        gui.operation_estimate_text = FakeVariable()  # type: ignore[assignment]
        gateway_id = 0x1111222233334444
        gui.gateway_id = gateway_id
        gui._topology_gateway_id = gateway_id
        gui._topology_slot_span = 7
        gui._topology_deepest_hop = 3
        gui._topology_timing_summary = (
            "Topology: 4 anchors, max hop 3, slot span 7."
        )

        for state in ("reconnecting", "connecting"):
            gui._set_connection_state(state)
            self.assertIsNone(gui.gateway_id)
            self.assertEqual(gui._topology_gateway_id, gateway_id)
            self.assertEqual(gui.assignment_expected_anchors_text.get(), "4")
            self.assertEqual(gui._topology_deepest_hop, 3)
            self.assertEqual(gui._topology_slot_span, 7)
            self.assertIn("4 anchors, max hop 3, slot span 7",
                          gui._topology_timing_summary)

        self.assertIsNone(gui._accept_gateway_identity(gateway_id, "GATT identity"))
        gui._set_connection_state("connected")
        self.assertEqual(gui._topology_gateway_id, gateway_id)
        self.assertEqual(gui.assignment_expected_anchors_text.get(), "4")
        self.assertEqual(gui._topology_deepest_hop, 3)
        self.assertEqual(gui._topology_slot_span, 7)
        self.assertIn("4 anchors, max hop 3, slot span 7",
                      gui._topology_timing_summary)

    def test_different_gateway_clears_topology_before_policy_calculation(
        self,
    ) -> None:
        gui = self.identity_gui_model()
        self.set_default_policy_variables(gui, expected_anchors="4")
        gui.operation_estimate_text = FakeVariable()  # type: ignore[assignment]
        old_gateway_id = 0x1111222233334444
        new_gateway_id = 0x5555666677778888
        gui.gateway_id = old_gateway_id
        gui._topology_gateway_id = old_gateway_id
        gui._topology_slot_span = 7
        gui._topology_deepest_hop = 3
        gui._topology_timing_summary = (
            "Topology: 4 anchors, max hop 3, slot span 7."
        )

        gui._set_connection_state("reconnecting")
        self.assertIsNone(gui.gateway_id)
        self.assertIsNone(
            gui._accept_gateway_identity(new_gateway_id, "GATT identity")
        )

        self.assertEqual(gui.gateway_id, new_gateway_id)
        self.assertIsNone(gui._topology_gateway_id)
        self.assertIsNone(gui._topology_slot_span)
        self.assertEqual(
            gui._topology_timing_summary,
            "Topology estimate pending enumeration.",
        )
        self.assertEqual(
            gui.assignment_expected_anchors_text.get(),
            gateway_app.DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT,
        )
        self.assertEqual(gui._topology_deepest_hop, 0)
        policy = gui._operation_policy_profile()
        self.assertEqual(
            policy.assignment.expected_anchor_count,
            int(gateway_app.DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT),
        )
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

    def test_retained_generic_command_event_does_not_advance_assignment_barrier(
        self,
    ) -> None:
        gateway_id = 0x9999AAAABBBBCCCC
        event_sequence = 0x11223344
        gui = GatewayGui.__new__(GatewayGui)
        gui.assignment_replay_barrier = GatewayAssignmentReplayBarrier()
        generic = parse_stream_record(
            stream_record(
                gateway_assignment_event_payload(
                    event_sequence=event_sequence,
                    stage=1,
                    anchor_id=0,
                    discovery_slot=0xFF,
                    progress_count=0,
                    total_count=0,
                    success_count=0,
                    failure_count=0,
                ),
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                packet_src_id=gateway_id,
                packet_dst_id=gateway_id,
                packet_session_id=event_sequence,
                packet_seq=event_sequence & 0xFFFF,
            )
        )

        self.assertIsNone(gui._observe_assignment_replay(generic))
        self.assertFalse(gui.assignment_replay_barrier.active)

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

    def test_assignment_is_snapshotted_behind_distinct_here_i_am_preflight(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
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
            [value.type_id for value in parse_cobs_packet(plan.target.frame).tlvs],
            [TLV_COMMAND_ID, TLV_OPERATION_POLICY],
        )
        self.assertEqual(
            [value.type_id for value in parse_cobs_packet(plan.preflight.frame).tlvs],
            [TLV_COMMAND_ID, TLV_OPERATION_POLICY],
        )
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

    def test_survey_enumeration_is_explicitly_ram_only(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        self.set_default_policy_variables(gui, expected_anchors="4")
        gui.__dict__["_submit_gateway_command"] = Mock(return_value=True)
        gui.__dict__["_show_error"] = Mock()

        self.assertTrue(
            gui._send_assign_discovery_slots(ram_only_iteration=True)
        )

        plan = gui._submit_gateway_command.call_args.args[0]
        policies = tuple(
            value.decoded
            for value in parse_cobs_packet(plan.target.frame).tlvs
            if value.type_id == TLV_OPERATION_POLICY
        )
        self.assertEqual(len(policies), 1)
        self.assertTrue(policies[0]["ram_only_iteration"])

    def test_manual_here_i_am_carries_the_current_full_policy(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0xAABBCCDDEEFF0011
        gui.sequence = 0
        gui._last_command_session_id = 0
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        self.set_default_policy_variables(gui, expected_anchors="5")
        gui.assignment_response_spread_text = FakeVariable("750")  # type: ignore[assignment]
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
        self.assertEqual(len(policies), 1)
        self.assertEqual(policies[0]["expected_anchor_count"], 5)
        self.assertEqual(policies[0]["response_spread_ms"], 750)
        self.assertEqual(
            [value.type_id for value in parse_cobs_packet(plan.target.frame).tlvs],
            [TLV_COMMAND_ID, TLV_OPERATION_POLICY],
        )

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

    def test_default_command_timeout_uses_full_robust_gui_wait(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
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

    @staticmethod
    def clear_memory_guard_model() -> GatewayGui:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connection_state = "disconnected"
        gui.connected = False
        gui.gateway_id = None
        gui._clear_packets = Mock()  # type: ignore[method-assign]
        gui.delivery_dedup = Mock()
        gui._assignment_replay_receipts = {b"receipt": object()}
        gui.assignment_replay_barrier = Mock()  # type: ignore[assignment]
        gui.command_request_tracker = Mock()  # type: ignore[assignment]
        gui.command_request_tracker.pending = None
        gui.command_orchestrator = Mock()  # type: ignore[assignment]
        gui.command_orchestrator.active = False
        gui.geometry_model = Mock()
        gui.click_location_model = Mock()
        gui.wake_monitor = Mock()
        gui.command_timeline_model = Mock()
        gui.topology_model = Mock()
        gui._reset_topology_timing = Mock()  # type: ignore[method-assign]
        gui.status_text = FakeVariable("Ready")  # type: ignore[assignment]
        gui._append_log = Mock()  # type: ignore[method-assign]
        gui._show_error = Mock()  # type: ignore[method-assign]
        return gui

    def test_clear_memory_action_copy_makes_connected_reboot_explicit(
        self,
    ) -> None:
        controls_source = inspect.getsource(GatewayGui._build_controls)

        self.assertIn(
            'text="Clear Host Memory / Reboot Gateway"', controls_source
        )
        self.assertIn("when connected", controls_source)
        self.assertIn("reboots the gateway board", controls_source)

    def test_clear_memory_handler_blocks_active_or_transition_state(self) -> None:
        cases = (
            ("active tracker", "connected", True, False),
            ("active orchestrator", "connected", False, True),
            ("connecting", "connecting", False, False),
            ("reconnecting", "reconnecting", False, False),
            ("disconnecting", "disconnecting", False, False),
        )

        for label, state, tracker_active, orchestrator_active in cases:
            with self.subTest(label=label):
                gui = self.clear_memory_guard_model()
                gui.connection_state = state
                gui.connected = state == "connected"
                gui.gateway_id = (
                    0x9999888877776666 if gui.connected else None
                )
                gui.command_request_tracker.pending = (
                    object() if tracker_active else None
                )
                gui.command_orchestrator.active = orchestrator_active

                gui._clear_gateway_memory()

                gui._clear_packets.assert_not_called()
                gui.delivery_dedup.clear.assert_not_called()
                gui.assignment_replay_barrier.reset.assert_not_called()
                gui.command_request_tracker.reset.assert_not_called()
                gui.command_orchestrator.reset.assert_not_called()
                gui._show_error.assert_called_once()

    def test_clear_gateway_memory_allows_offline_idle_host_ram_reset(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connection_state = "disconnected"
        gui.connected = False
        gui.delivery_dedup = gateway_app.GatewayPacketDeduplicator()
        gui.cir_reassembler = CirReassembler()
        gui.packet_by_iid = {"iid1": Mock()}
        gui._wake_row_iids = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.status_text = FakeVariable("Ready")  # type: ignore[assignment]
        gui.sample_warning_text = FakeVariable("")  # type: ignore[assignment]
        gui.cir_state_text = FakeVariable("")  # type: ignore[assignment]
        gui.packet_tree = Mock()
        gui.overview_tree = Mock()
        gui.sample_tree = Mock()
        gui.cir_tree = Mock()
        gui.tlv_tree = Mock()
        gui.diagnostics_text = Mock()
        gui.raw_text = Mock()
        gui.log_text = Mock()
        gui._set_cir_plot = Mock()
        gui._clear_tree = Mock()
        gui._set_text = Mock()
        gui._append_log = Mock()
        gui._initialize_gateway_diagnostics()

        gui._clear_gateway_memory()

        self.assertEqual(len(gui.delivery_dedup._entries), 0)
        self.assertEqual(gui.packet_by_iid, {})
        self.assertEqual(gui.status_text.get(), "Gateway external RAM and deduplication state cleared.")
        gui._append_log.assert_called()

    def test_clear_gateway_memory_reboots_connected_gateway_board(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connection_state = "connected"
        gui.delivery_dedup = gateway_app.GatewayPacketDeduplicator()
        gui.cir_reassembler = CirReassembler()
        gui.packet_by_iid = {}
        gui._wake_row_iids = {}
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui.status_text = FakeVariable("Ready")  # type: ignore[assignment]
        gui.sample_warning_text = FakeVariable("")  # type: ignore[assignment]
        gui.cir_state_text = FakeVariable("")  # type: ignore[assignment]
        gui.host_id_text = FakeVariable(f"0x{DEFAULT_HOST_ID:016x}")  # type: ignore[assignment]
        gui.connected = True
        gui.gateway_id = 0x9999888877776666
        gui.sequence = 0
        gui.transport = Mock()
        gui.command_request_tracker = GatewayCommandRequestTracker()
        gui.command_orchestrator = GatewayCommandOrchestrator(gui.command_request_tracker)
        gui.assignment_replay_barrier = GatewayAssignmentReplayBarrier()
        gui.packet_tree = Mock()
        gui.overview_tree = Mock()
        gui.sample_tree = Mock()
        gui.cir_tree = Mock()
        gui.tlv_tree = Mock()
        gui.diagnostics_text = Mock()
        gui.raw_text = Mock()
        gui.log_text = Mock()
        gui._set_cir_plot = Mock()
        gui._clear_tree = Mock()
        gui._set_text = Mock()
        gui._append_log = Mock()
        gui._update_command_state = Mock()
        gui._initialize_gateway_diagnostics()

        gui._clear_gateway_memory()

        gui.transport.send_frame.assert_called_once()
        frame, label = gui.transport.send_frame.call_args[0]
        self.assertEqual(label, "Reboot gateway board")
        self.assertIn("reboot command to gateway board", gui.status_text.get())

    def test_expected_anchors_default_is_three(self) -> None:
        self.assertEqual(gateway_app.DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT, "3")

    def test_active_command_state_shows_deadline_countdown(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.connected = True
        gui.gateway_id = 0x1234
        gui._command_progress_text = "Confirmations 2/3"
        gui.command_request_tracker = GatewayCommandRequestTracker()
        self.assertTrue(gui.command_request_tracker.begin(
            1, 55, 4, now=100.0, timeout_s=10.0
        ))
        gui.command_orchestrator = SimpleNamespace(
            active=True, phase="target", plan=None
        )
        gui.refresh_button = FakeWidget()  # type: ignore[assignment]
        gui.assignment_button = FakeWidget()  # type: ignore[assignment]
        gui.command_availability_text = FakeVariable()  # type: ignore[assignment]

        with patch("tools.gateway_gui.app.time.monotonic", return_value=105.0):
            gui._update_command_state()

        self.assertEqual(
            gui.command_availability_text.get(),
            "Confirmations 2/3. Deadline countdown: 0:05.",
        )
        self.assertEqual(gui.assignment_button.options["state"], "disabled")

    def test_enumeration_updates_topology_view_live_with_discovered_anchors(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui.status_text = FakeVariable()  # type: ignore[assignment]
        gui.transport = Mock()
        gui.command_timeline_model = CommandTimelineModel()
        gui.command_request_tracker = GatewayCommandRequestTracker()
        gui.command_orchestrator = GatewayCommandOrchestrator(gui.command_request_tracker)
        gui.assignment_replay_barrier = GatewayAssignmentReplayBarrier()
        gui.topology_model = Mock()
        gui.topology_model.observe.return_value = None
        gui.mesh_diagnostics_view = Mock()
        gui.geometry_model = Mock()
        gui.geometry_model.observe_command_event.return_value = False
        gui._apply_gateway_command_transition = Mock()  # type: ignore[assignment]

        packet = parse_stream_record(
            stream_record(
                gateway_assignment_event_payload(
                    anchor_id=0x56DA25FE4AF6D141,
                    discovery_slot=0,
                    event_sequence=1,
                ),
                msg_type=MSG_GATEWAY_COMMAND_EVENT,
                packet_flags=0,
                packet_src_id=0x9999888877776666,
                packet_dst_id=0x9999888877776666,
                packet_session_id=100,
                packet_seq=1,
            )
        )
        gui._observe_diagnostic_packet(packet)
        gui.mesh_diagnostics_view.show_topology.assert_called_once()
        call_args = gui.mesh_diagnostics_view.show_topology.call_args
        self.assertIsNone(call_args[0][0])
        self.assertIn(0x56DA25FE4AF6D141, call_args[0][1])

if __name__ == "__main__":
    unittest.main()
