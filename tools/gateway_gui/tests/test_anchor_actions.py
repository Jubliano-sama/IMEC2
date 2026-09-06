from dataclasses import replace
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from tools.gateway_gui.anchor_actions import AnchorActions, anchor_action_timeout_s
from tools.gateway_gui.anchor_actions_view import GatewayAnchorActionsMixin
from tools.gateway_gui.command_orchestration import GatewayCommandOrchestrator, GatewayCommandPlan
from tools.gateway_gui.command_telemetry import GatewayCommandRequestTracker
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS, CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY,
    CMD_FORCE_REDISCOVERY,
    MSG_COMMAND_RESULT, TLV_ANCHOR_ID, TLV_BATTERY_MV, TLV_COMMAND_ID,
    TLV_COMMAND_STATUS, TLV_DISCOVERY_ASSIGNMENT_EPOCH, TLV_DURATION_MS,
    TLV_HOP_COUNT, TLV_NODE_BOOT_COUNTER, TLV_TIMESTAMP_MS,
    append_tlv, encode_cobs_packet, parse_cobs_packet,
)
from tools.gateway_gui.tests.test_command_orchestration import dispatch, terminal


def enumeration(depth=1):
    event = replace(terminal(dispatch(CMD_ASSIGN_DISCOVERY_SLOTS, 1, 20, 1)),
                    total_count=1, success_count=1, gateway_sequence=33)
    detail = replace(event, flags=0, stage=6, anchor_id=2, discovery_slot=0, hop_count=depth)
    return event, {2: detail}


class AnchorActionTests(unittest.TestCase):
    def setUp(self):
        self.model = AnchorActions()

    def prepare(self, command_id=CMD_READ_ANCHOR_BATTERY):
        return self.model.prepare(gateway_id=1, host_id=9, anchor_id=2,
                                  command_id=command_id, session_id=100, sequence=7)

    def reply(self, *, status=0, command_id=CMD_READ_ANCHOR_BATTERY,
              omit=None, duplicate=None, **overrides):
        fields = [(TLV_COMMAND_ID, command_id, 2), (TLV_COMMAND_STATUS, status, 2),
                  (TLV_ANCHOR_ID, 2, 8), (TLV_DISCOVERY_ASSIGNMENT_EPOCH, 33, 4),
                  (TLV_NODE_BOOT_COUNTER, 10, 4), (TLV_TIMESTAMP_MS, 1234, 8)]
        fields += [(TLV_BATTERY_MV, 3712, 2)] if command_id == CMD_READ_ANCHOR_BATTERY else [(TLV_DURATION_MS, 10000, 4)]
        payload = bytearray()
        for tag, value, width in fields:
            if tag == omit:
                continue
            append_tlv(payload, tag, value.to_bytes(width, "little"))
            if tag == duplicate:
                append_tlv(payload, tag, value.to_bytes(width, "little"))
        args = dict(msg_type=MSG_COMMAND_RESULT, flags=1, src_id=2, dst_id=1,
                    session_id=100, seq=7, ttl=8, message_age_ms=300, payload=bytes(payload))
        args.update(overrides)
        return parse_cobs_packet(encode_cobs_packet(**args))

    def test_requires_current_connection_enumeration(self):
        with self.assertRaises(ValueError): self.prepare()
        self.model.remember_enumeration(1, *enumeration(3))
        command = self.prepare()
        self.assertEqual(command.packet.dst_id, 2)
        self.assertEqual(command.packet.value(TLV_HOP_COUNT), 3)
        self.assertEqual(command.packet.value(TLV_DISCOVERY_ASSIGNMENT_EPOCH), 33)
        self.assertEqual(len(command.packet.payload), 13)
        self.model.reset()
        with self.assertRaises(ValueError): self.prepare()

    def test_invalid_enumeration_never_publishes_roster(self):
        event, anchors = enumeration()
        for invalid in (replace(event, flags=5), replace(event, flags=0),
                        replace(event, command_status=7), replace(event, success_count=2),
                        replace(event, gateway_sequence=0)):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError): self.model.remember_enumeration(1, invalid, anchors)
                self.assertFalse(self.model.anchors)
        for changes in (dict(hop_count=0), dict(hop_count=9), dict(discovery_slot=255),
                        dict(anchor_id=3), dict(gateway_sequence=34)):
            with self.assertRaises(ValueError):
                self.model.remember_enumeration(1, event, {2: replace(anchors[2], **changes)})

    def test_depth_based_waits_and_bounds(self):
        self.assertEqual([anchor_action_timeout_s(h) for h in (1, 2, 8)], [29, 53, 197])
        for invalid in (0, 9, -1, True, 1.5):
            with self.assertRaises(ValueError): anchor_action_timeout_s(invalid)

    def test_only_exact_anchor_success_completes(self):
        self.model.remember_enumeration(1, *enumeration())
        self.prepare()
        for changes in (dict(src_id=3), dict(src_id=1), dict(session_id=99), dict(seq=6)):
            self.assertIsNone(self.model.observe(self.reply(**changes)))
        reply = self.model.observe(self.reply())
        self.assertEqual(reply.battery_mv, 3712)
        self.assertEqual(reply.age_ms, 300)
        self.assertEqual(reply.sampled_at_ms, 1234)
        self.model.pending = None
        self.assertIsNone(self.model.observe(self.reply()))

    def test_gateway_queue_age_is_not_displayed_as_sample_age(self):
        self.model.remember_enumeration(1, *enumeration())
        self.prepare()
        packet = replace(self.reply(), age_ms=9000, age_kind="gateway_queue_age_ms")
        reply = self.model.observe(packet)
        self.assertIsNone(reply.age_ms)
        self.assertEqual(reply.battery_mv, 3712)
        self.assertEqual(reply.sampled_at_ms, 1234)
        self.assertIn("just received", reply.text)
        self.assertNotIn("sample age", reply.text)

        self.model.pending = None
        gui = SimpleNamespace(anchor_actions=self.model, gateway_id=1,
            anchor_selection=Mock(), anchor_selection_text=Mock(),
            anchor_identify_button=Mock(), anchor_battery_button=Mock(),
            anchor_action_text=Mock())
        gui.anchor_selection_text.get.return_value = "0000000000000002 · slot 0 · 1 hop"
        with patch("tools.gateway_gui.anchor_actions_view.time.monotonic",
                   return_value=reply.received_at + 2):
            GatewayAnchorActionsMixin._update_anchor_action_controls(gui, "normal")
        gui.anchor_action_text.set.assert_called_once_with(
            "Battery: 3.712 V · received 2.0 s ago")

    def test_malformed_results_and_failure_are_not_zero_voltage(self):
        self.model.remember_enumeration(1, *enumeration())
        self.prepare()
        for field in (TLV_BATTERY_MV, TLV_ANCHOR_ID, TLV_NODE_BOOT_COUNTER,
                      TLV_TIMESTAMP_MS, TLV_DISCOVERY_ASSIGNMENT_EPOCH, TLV_COMMAND_STATUS):
            for option in ("omit", "duplicate"):
                with self.assertRaises(ValueError): self.model.observe(self.reply(**{option: field}))
        self.assertFalse(self.model.replies)
        failure = self.model.observe(self.reply(status=7, src_id=1, omit=TLV_BATTERY_MV))
        self.assertIsNone(failure.battery_mv)

    def test_identify_acceptance(self):
        self.model.remember_enumeration(1, *enumeration())
        self.prepare(CMD_IDENTIFY_ANCHOR)
        reply = self.model.observe(self.reply(command_id=CMD_IDENTIFY_ANCHOR))
        self.assertEqual(reply.status, 0)
        self.assertIn("10-second", reply.text)

    def test_no_hia_and_no_completion_from_unvalidated_result(self):
        target = dispatch(CMD_IDENTIFY_ANCHOR, 2, 100, 7)
        owner = GatewayCommandOrchestrator(GatewayCommandRequestTracker())
        self.assertIs(owner.begin(GatewayCommandPlan.user_triggered(target), now=0), target)
        self.assertFalse(owner.observe_event(terminal(target), now=1).matched)
        args = dict(command_id=CMD_IDENTIFY_ANCHOR, host_session_id=100,
                    host_sequence=7, command_status=0, now=1)
        self.assertFalse(owner.observe_command_result(**args).matched)
        self.assertTrue(owner.observe_command_result(**args, anchor_result_validated=True).completed)

    def test_survey_gui_guard_sends_nothing(self):
        errors = []
        gui = SimpleNamespace(connected=True, _survey_phase="ranging", _show_error=errors.append,
                              _update_command_state=lambda: None)
        GatewayAnchorActionsMixin._send_anchor_action(gui, CMD_IDENTIFY_ANCHOR)
        self.assertEqual(len(errors), 1)

    def test_route_refresh_invalidates_saved_action_paths_before_dispatch(self):
        from tools.gateway_gui.app import GatewayGui
        self.model.remember_enumeration(1, *enumeration())
        sent = []
        def send(frame, label):
            self.assertFalse(self.model.anchors)
            sent.append(frame)
        gui = SimpleNamespace(anchor_actions=self.model,
            command_orchestrator=SimpleNamespace(phase="target"),
            _set_scheduled_phase_estimate=lambda *args: None,
            status_text=SimpleNamespace(set=lambda value: None),
            transport=SimpleNamespace(send_frame=send))
        GatewayGui._dispatch_gateway_command(gui, dispatch(CMD_FORCE_REDISCOVERY, 2, 100, 7))
        self.assertEqual(len(sent), 1)
        with self.assertRaises(ValueError): self.prepare()

    def test_late_reply_cannot_replace_timeout_or_disconnection(self):
        for disconnected in (False, True):
            with self.subTest(disconnected=disconnected):
                self.model.reset()
                self.model.remember_enumeration(1, *enumeration())
                self.prepare()
                owner = GatewayCommandOrchestrator(GatewayCommandRequestTracker())
                target = dispatch(CMD_READ_ANCHOR_BATTERY, 2, 100, 7)
                owner.begin(GatewayCommandPlan.user_triggered(target), now=0)
                transitions = []
                gui = SimpleNamespace(anchor_actions=self.model, command_orchestrator=owner)
                def apply(transition):
                    transitions.append(transition)
                    self.model.pending = None
                gui._apply_gateway_command_transition = apply
                if disconnected:
                    apply(owner.disconnect())
                    self.model.reset()
                self.assertTrue(GatewayAnchorActionsMixin._observe_anchor_action_result(
                    gui, self.reply(), received_at=10))
                self.assertFalse(self.model.replies)
                self.assertFalse(owner.active)
                self.assertIsNone(self.model.pending)
                self.assertEqual(transitions[-1].outcome,
                                 "disconnected" if disconnected else "timeout")


if __name__ == "__main__":
    unittest.main()
