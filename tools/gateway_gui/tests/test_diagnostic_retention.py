from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock

from tools.gateway_gui.cir_reassembly import CIR_MAX_ASSEMBLY_ERRORS, CirReassembler
from tools.gateway_gui.diagnostic_models import (
    CommandTimelineModel,
    PendingWakeAttemptAdapter,
    TopologyBaselineModel,
    WakeTrainMonitor,
)
from tools.gateway_gui.diagnostics_integration import GatewayDiagnosticsMixin
from tools.gateway_gui.protocol import GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES
from tools.gateway_gui.tests.test_cir_reassembly import fragment_packet
from tools.gateway_gui.tests.test_diagnostic_models import click, event


class DiagnosticGui(GatewayDiagnosticsMixin):
    def __init__(self):
        self.wake_monitor = WakeTrainMonitor(max_recent=8)
        self.wake_attempt_adapter = PendingWakeAttemptAdapter()
        self.click_location_model = Mock()
        self.click_location_model.observe.return_value = None
        self._wake_by_packet_key = {}
        self._wake_row_iids = {}
        self.packet_by_iid = {}
        self.cir_reassembler = CirReassembler()
        self.cir_key_by_packet_id = {}

    def _refresh_wake_row(self, key):
        pass

    def add_row(self, packet, iid):
        self._observe_diagnostic_packet(packet)
        self.packet_by_iid[iid] = packet
        self._register_diagnostic_packet_row(packet, iid)
        result = self.cir_reassembler.ingest(packet)
        if result is not None and result.key is not None:
            self.cir_key_by_packet_id[id(packet)] = result.key

    def remove_row(self, iid):
        packet = self.packet_by_iid.pop(iid)
        self._forget_diagnostic_packet_row(packet)
        self.cir_key_by_packet_id.pop(id(packet), None)


class DiagnosticRetentionTests(unittest.TestCase):
    def test_excess_anchor_ids_are_bounded_and_never_form_a_complete_baseline(self):
        with TemporaryDirectory() as temporary:
            topology = TopologyBaselineModel(Path(temporary) / "baseline.json", max_runs=1)
            timeline = CommandTimelineModel(max_events=8)
            for sequence in range(1, 501):
                detail = event(anchor=sequence, event_seq=sequence)
                topology.observe(detail)
                timeline.observe(detail)
            terminal = event(stage=12, flags=1, total=50, event_seq=501)
            timeline.observe(terminal)
            result = topology.observe(terminal)
            self.assertEqual(len(topology.current_ids), GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES)
            self.assertEqual(len(timeline.enumerated_anchors[terminal.correlation_key]), GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES)
            self.assertFalse(result.complete)
            self.assertIn("exceeded", result.eligibility_reason)
            with self.assertRaises(ValueError):
                topology.accept_latest()
            # Existing anchors can still acquire their final assignment details.
            update = event(anchor=1, event_seq=502)
            timeline.observe(update)
            self.assertIs(timeline.enumerated_anchors[terminal.correlation_key][1], update)
            topology.observe(event(anchor=1, correlation=999, event_seq=503))
            self.assertFalse(topology._overflow_keys)
            topology.reset()
            self.assertFalse(topology._overflow_keys)

    def test_one_malformed_cir_identity_cannot_accumulate_unbounded_errors(self):
        model = CirReassembler()
        initial = model.ingest(fragment_packet(0, timestamp_ms=1000))
        for timestamp in range(1001, 1101):
            result = model.ingest(fragment_packet(1, timestamp_ms=timestamp))
            self.assertFalse(result.accepted)
            self.assertTrue(result.errors)
            self.assertEqual(result.view.state, "malformed")
            self.assertLessEqual(len(result.view.errors), CIR_MAX_ASSEMBLY_ERRORS + 1)
        self.assertEqual(len(model._assemblies), 1)
        self.assertIn("omitted", model.view(initial.key).errors[-1])
        self.assertIsNone(model.view(initial.key).raw)

    def test_completed_and_abandoned_cir_history_is_bounded(self):
        model = CirReassembler(max_assemblies=3)
        first = model.ingest(fragment_packet(0, event_seq=1))
        abandoned = model.ingest(fragment_packet(0, event_seq=2))
        model.ingest(fragment_packet(0, event_seq=3))
        self.assertTrue(model.ingest(fragment_packet(1, event_seq=1)).view.complete)
        model.ingest(fragment_packet(0, event_seq=4))
        self.assertTrue(model.view(first.key).complete)
        self.assertIsNone(model.view(abandoned.key))

        for sequence in range(5, 205):
            model.ingest(fragment_packet(0, event_seq=sequence))
            if sequence % 2:
                self.assertTrue(model.ingest(fragment_packet(1, event_seq=sequence)).view.complete)
            self.assertLessEqual(len(model._assemblies), 3)
        # Late tails cannot turn an abandoned window into a fabricated waveform.
        late = model.ingest(fragment_packet(1, event_seq=2))
        self.assertTrue(late.accepted)
        self.assertFalse(late.view.complete)
        self.assertIsNone(late.view.raw)
        self.assertEqual(late.view.missing_fragment_indices, (0,))

    def test_cir_bytes_live_until_the_last_visible_fragment_is_removed(self):
        gui = DiagnosticGui()
        first = fragment_packet(0)
        second = fragment_packet(1)
        gui.add_row(first, "first")
        gui.add_row(second, "second")
        key = gui.cir_key_by_packet_id[id(first)]
        self.assertTrue(gui.cir_reassembler.view(key).complete)
        gui.remove_row("first")
        self.assertTrue(gui.cir_reassembler.view(key).complete)
        gui.remove_row("second")
        self.assertIsNone(gui.cir_reassembler.view(key))

    def test_wake_secondary_history_follows_rows_and_bounded_monitor(self):
        gui = DiagnosticGui()
        for sequence in range(500):
            gui.add_row(click(1, 2.0, event=sequence, sequence=sequence), str(sequence))
            if sequence >= 8:
                gui.remove_row(str(sequence - 8))
            self.assertLessEqual(len(gui._wake_by_packet_key), 16)
            self.assertLessEqual(len(gui._wake_row_iids), 8)
        for iid in tuple(gui.packet_by_iid):
            gui.remove_row(iid)
        self.assertFalse(gui._wake_by_packet_key)
        self.assertFalse(gui._wake_row_iids)

    def test_visible_wake_diagnostics_survive_monitor_eviction_and_replays(self):
        gui = DiagnosticGui()
        first = click(1, 2.0)
        gui.add_row(first, "first")
        key = gui._wake_evidence(first).key
        for sequence in range(2, 40):
            gui.add_row(click(1, 2.0, event=sequence), str(sequence))
        self.assertNotIn(key, gui.wake_monitor.retained_keys)
        self.assertIn(key, gui._wake_by_packet_key)
        gui.add_row(first, "replay")
        gui.remove_row("first")
        self.assertEqual(gui._wake_row_iids[key], "replay")
        self.assertIn(key, gui._wake_by_packet_key)
        gui._clear_diagnostic_packet_rows()
        self.assertFalse(gui._wake_by_packet_key)
        self.assertFalse(gui._wake_row_iids)

    def test_timeline_indexes_never_outlive_their_retained_runs(self):
        model = CommandTimelineModel(max_events=8)
        for sequence in range(1, 201):
            model.observe(event(anchor=sequence, correlation=sequence, event_seq=2 * sequence))
            model.observe(event(stage=12, flags=1, total=1, correlation=sequence, event_seq=2 * sequence + 1))
            retained_keys = {item.correlation_key for item in model.events.values()}
            self.assertLessEqual(model.terminals.keys(), retained_keys)
            self.assertLessEqual(model.enumerated_anchors.keys(), retained_keys)
            self.assertLessEqual(sum(map(len, model.enumerated_anchors.values())), 8)

    def test_active_command_details_survive_more_than_one_event_window(self):
        model = CommandTimelineModel(max_events=8)
        for sequence in range(1, 51):
            model.observe(event(anchor=sequence, event_seq=sequence))
        for sequence in range(51, 501):
            model.observe(event(stage=5, event_seq=sequence))
        key = event().correlation_key
        self.assertEqual(len(model.events), 8)
        self.assertEqual(set(model.enumerated_anchors[key]), set(range(1, 51)))
        terminal = event(stage=12, flags=1, total=50, event_seq=501)
        model.observe(terminal)
        self.assertIs(model.terminal_for(key), terminal)
        for sequence in range(502, 510):
            model.observe(event(correlation=999, event_seq=sequence))
        self.assertNotIn(key, model.enumerated_anchors)
        self.assertIsNone(model.terminal_for(key))

    def test_timeline_retention_crosses_u32_sequence_wrap(self):
        model = CommandTimelineModel(max_events=2)
        for sequence in (0xFFFFFFFE, 0xFFFFFFFF, 0, 1):
            model.observe(event(event_seq=sequence))
        self.assertEqual({item.event_sequence for item in model.events.values()}, {0, 1})

    def test_topology_eviction_preserves_current_and_late_terminal_details(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json", max_runs=2)
            for sequence in range(1, 101):
                model.observe(event(anchor=sequence, correlation=sequence, event_seq=sequence * 2))
                result = model.observe(event(stage=12, flags=1, total=1, correlation=sequence, event_seq=sequence * 2 + 1))
                self.assertTrue(result.complete)
                keys = model._anchors_by_key.keys()
                self.assertLessEqual(len(keys), 2)
                for index in (model._terminals, model._first_sequence_by_key, model._first_loss_by_key):
                    self.assertLessEqual(index.keys(), keys)
                self.assertLessEqual(model._live_keys, keys)
                self.assertLessEqual(model._overflow_keys, keys)

            # A terminal received first retains its owner while stale run replays
            # churn the remaining history slot, then its late detail completes it.
            current = event(stage=12, flags=1, total=1, correlation=101, event_seq=300)
            self.assertFalse(model.observe(current).complete)
            for sequence in range(1, 20):
                self.assertIsNone(model.observe(event(anchor=sequence, flags=4, correlation=sequence, event_seq=sequence)))
                self.assertEqual(model.current_key, current.correlation_key)
                self.assertLessEqual(len(model._anchors_by_key), 2)
            result = model.observe(event(anchor=101, correlation=101, event_seq=299))
            self.assertTrue(result.complete)
            self.assertEqual(model.accept_latest().anchor_ids, (101,))
            model.reset()
            self.assertFalse(model._anchors_by_key)
            self.assertFalse(model._terminals)
            self.assertFalse(model._live_keys)


if __name__ == "__main__":
    unittest.main()
