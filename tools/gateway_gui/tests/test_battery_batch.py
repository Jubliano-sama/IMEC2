import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

from tools.gateway_gui.anchor_actions import AnchorActions, EnumeratedAnchor, anchor_action_timeout_s
from tools.gateway_gui.anchor_actions_view import GatewayAnchorActionsMixin
from tools.gateway_gui.command_orchestration import GatewayCommandOrchestrator
from tools.gateway_gui.command_telemetry import GatewayCommandRequestTracker
from tools.gateway_gui.protocol import (
    CMD_READ_ANCHOR_BATTERY, CMD_IDENTIFY_ANCHOR, MSG_COMMAND_RESULT,
    TLV_COMMAND_ID, TLV_COMMAND_STATUS, TLV_ANCHOR_ID, TLV_DISCOVERY_ASSIGNMENT_EPOCH,
    TLV_NODE_BOOT_COUNTER, TLV_TIMESTAMP_MS, TLV_BATTERY_MV, TLV_DURATION_MS,
    append_tlv, parse_cobs_packet, encode_cobs_packet,
)


def reply(request, *, status=0):
    payload = bytearray()
    fields = [(TLV_COMMAND_ID, request.command_id, 2), (TLV_COMMAND_STATUS, status, 2),
              (TLV_ANCHOR_ID, request.anchor.node_id, 8), (TLV_DISCOVERY_ASSIGNMENT_EPOCH, request.epoch, 4),
              (TLV_NODE_BOOT_COUNTER, 2, 4), (TLV_TIMESTAMP_MS, request.sequence * 1000, 8)]
    fields += ([(TLV_BATTERY_MV, 3700, 2)] if request.command_id == CMD_READ_ANCHOR_BATTERY
               else [(TLV_DURATION_MS, 10000, 4)])
    for tag, value, width in fields:
        append_tlv(payload, tag, value.to_bytes(width, 'little'))
    return parse_cobs_packet(encode_cobs_packet(msg_type=MSG_COMMAND_RESULT, flags=1,
        src_id=request.anchor.node_id, dst_id=1, session_id=request.session_id,
        seq=request.sequence, ttl=8, message_age_ms=300, payload=bytes(payload)))


class BatchGui(GatewayAnchorActionsMixin):
    def __init__(self, count=3):
        self.anchor_actions = AnchorActions()
        self.anchor_actions.gateway_id = self.gateway_id = 1
        self.anchor_actions.epoch = 42
        self.anchor_actions.anchors = {100+i: EnumeratedAnchor(100+i, i, 1+i%8) for i in reversed(range(count))}
        self.command_request_tracker = GatewayCommandRequestTracker()
        self.command_orchestrator = GatewayCommandOrchestrator(self.command_request_tracker)
        self.connected = True
        self._survey_phase = 'idle'
        self.errors = []
        self.callbacks = []
        self.sent = []
        self.root = SimpleNamespace(after_idle=self.callbacks.append)
        self.host_id_text = SimpleNamespace(get=lambda: '9')
        self.status_text = Mock()
        self.sequence = 0

    def _show_error(self, text): self.errors.append(text)
    def _update_command_state(self): pass
    def _require_gateway_identity(self): return self.gateway_id
    def _parse_int(self, label, text): return int(text)
    def _next_identity(self):
        self.sequence += 1
        return 77, self.sequence
    def _submit_gateway_command(self, plan):
        dispatch = self.command_orchestrator.begin(plan)
        if dispatch is None: return False
        self.sent.append(dispatch)
        return True
    def _append_log(self, *_args): pass
    def _apply_gateway_command_transition(self, transition): self._finish_anchor_action(transition)
    def pump(self):
        callbacks = list(self.callbacks)
        self.callbacks.clear()
        for callback in callbacks: callback()
    def complete(self, status=0):
        packet = reply(self.anchor_actions.pending, status=status)
        self._observe_anchor_action_result(packet)
        return packet


class BatteryBatchTests(unittest.TestCase):
    def test_one_action_reads_all_50_with_sequential_exact_depth_budgets(self):
        gui = BatchGui(50)
        gui._read_all_batteries()
        for i in range(50):
            pending = gui.anchor_actions.pending
            self.assertEqual(pending.anchor.node_id, 100+i)
            self.assertEqual(len(gui.sent), i+1)
            self.assertEqual(gui.sent[-1].timeout_s, anchor_action_timeout_s(1+i%8))
            gui.complete()
            self.assertIsNone(gui.anchor_actions.pending)
            self.assertEqual(len(gui.sent), i+1)  # No nested dispatch during result handling.
            self.assertEqual(gui.anchor_actions.battery_batch_active, i<49)
            gui.pump()
        self.assertFalse(gui.errors)
        self.assertEqual(len(gui.anchor_actions.batteries), 50)
        self.assertEqual(set(gui.anchor_actions.battery_outcomes.values()), {'complete'})

    def test_failure_timeout_and_late_reply_cannot_fake_or_block_remaining_readings(self):
        gui = BatchGui()
        gui._read_all_batteries()
        late = reply(gui.anchor_actions.pending)
        gui._apply_gateway_command_transition(gui.command_orchestrator.expire(now=time.monotonic()+200))
        gui.pump()
        gui._observe_anchor_action_result(late)
        self.assertEqual(gui.anchor_actions.pending.anchor.node_id, 101)
        self.assertNotIn(100, gui.anchor_actions.batteries)
        gui.complete(status=7)
        gui.pump()
        gui.complete()
        gui.pump()
        self.assertEqual(gui.anchor_actions.battery_outcomes, {100:'timeout', 101:'failed', 102:'complete'})
        self.assertEqual(set(gui.anchor_actions.batteries), {102})

    def test_identification_and_failed_refresh_preserve_cached_battery(self):
        gui = BatchGui(1)
        gui._read_all_batteries(); gui.complete(); gui.pump()
        battery = gui.anchor_actions.batteries[100]
        gui._send_anchor_action(CMD_IDENTIFY_ANCHOR, anchor_id=100)
        gui.complete()
        self.assertIs(gui.anchor_actions.batteries[100], battery)
        gui._read_all_batteries(); gui.complete(status=7); gui.pump()
        self.assertIs(gui.anchor_actions.batteries[100], battery)
        self.assertIn('3.700 V', gui._anchor_hover_text('0000000000000064'))
        self.assertIn('sample age', gui._anchor_hover_text('0000000000000064'))
        self.assertEqual(len(gui.sent), 3)  # Hover did not request a conversion.

    def test_no_enumeration_survey_or_disconnection_sends_no_batch(self):
        for state in ('no-roster', 'survey', 'disconnected'):
            gui = BatchGui()
            if state == 'no-roster': gui.anchor_actions.reset()
            if state == 'survey': gui._survey_phase = 'ranging'
            if state == 'disconnected': gui.connected = False
            gui._read_all_batteries()
            self.assertFalse(gui.sent)
            self.assertFalse(gui.anchor_actions.battery_batch_active)
            self.assertFalse(gui._anchor_can_identify('0000000000000064'))

    def test_survey_or_reset_between_requests_cancels_queue(self):
        for reset in (False, True):
            gui = BatchGui()
            gui._read_all_batteries(); gui.complete()
            if reset: gui.anchor_actions.reset()
            else: gui._survey_phase = 'ranging'
            gui.pump()
            self.assertEqual(len(gui.sent), 1)
            self.assertFalse(gui.anchor_actions.battery_batch_active)

    def test_manual_or_double_batch_actions_cannot_mutate_running_batch(self):
        gui = BatchGui()
        gui._read_all_batteries()
        pending = gui.anchor_actions.pending
        gui._read_all_batteries()
        gui._send_anchor_action(CMD_IDENTIFY_ANCHOR, anchor_id=100)
        self.assertIs(gui.anchor_actions.pending, pending)
        self.assertEqual(gui.anchor_actions.battery_queue, [100,101,102])
        self.assertEqual(len(gui.sent), 1)
        gui.complete()
        self.assertFalse(gui._anchor_can_identify('0000000000000064'))
        gui._send_anchor_action(CMD_IDENTIFY_ANCHOR, anchor_id=101)
        self.assertEqual(gui.anchor_actions.battery_queue, [101,102])
        gui.pump()
        self.assertEqual(len(gui.sent), 2)
