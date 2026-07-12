import math
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.gateway_gui.diagnostic_models import (
    AnchorBaseline, ClickLocationModel, CommandTimelineModel, SurveyGeometryModel,
    TopologyBaselineModel, WakeEvidence, WakeTrainMonitor,
    WAKE_COLLISION, WAKE_LATE, WAKE_NORMAL, WAKE_UNKNOWN, COLLISION_WINDOW_MS,
)
from tools.gateway_gui.command_telemetry import GatewayCommandEvent
from tools.gateway_gui.protocol import Packet, parse_tlvs


def tlv(kind, value):
    return bytes((kind, len(value))) + value


def packet(msg_type, payload, *, session=7, sequence=1, age=0):
    return Packet("test", payload, None, msg_type, 0, 1, 2, session, sequence, None, age, "gateway_queue_age_ms", payload, parse_tlvs(payload))


def click(anchor, distance_m, *, event=1, session=7, sequence=1, clicker=0xC1):
    payload = b"".join((
        tlv(0x06, event.to_bytes(4, "little")), tlv(0x0B, clicker.to_bytes(8, "little")),
        tlv(0x0A, anchor.to_bytes(8, "little")), tlv(0x0C, round(distance_m * 1000).to_bytes(4, "little", signed=True)),
        tlv(0x21, b"\x00"),
    ))
    return packet(0x20, payload, session=session, sequence=sequence)


def event(*, kind=1, stage=6, flags=0, status=0, reason=0, event_seq=1, anchor=0, total=0, lost=0, correlation=9, gateway_sequence=5):
    return GatewayCommandEvent(kind, stage, flags, 1, status, reason, 0x104, 2, correlation, gateway_sequence, 7, 8, event_seq, anchor, 0, 0, 0, 1, total, 0, 0, 0, lost, 0, 255)


class SurveyAndClickTests(unittest.TestCase):
    def test_survey_success_only_and_missing_requires_complete_opportunity(self):
        model = SurveyGeometryModel()
        def pair_result(a, b, distance, status, seq):
            payload = b"".join((tlv(0x15, (10).to_bytes(4, "little")), tlv(0x1F, a.to_bytes(8, "little")), tlv(0x20, b.to_bytes(8, "little")), tlv(0x0C, distance.to_bytes(4, "little", signed=True)), tlv(0x21, bytes((status,)))))
            return packet(0x53, payload, sequence=seq)
        model.observe_pair_packet(pair_result(1, 2, 3000, 0, 1))
        model.observe_pair_packet(pair_result(1, 3, 0, 2, 2))
        self.assertEqual(len(model.pairs), 1)
        self.assertEqual(model.missing_pairs, frozenset())
        model.observe_command_event(event(kind=2, stage=8, total=2, gateway_sequence=10))
        model.observe_command_event(event(kind=2, stage=12, flags=1, total=2, event_seq=2, gateway_sequence=10))
        self.assertEqual(len(model.missing_pairs), 1)

    def test_exact_noisy_rapid_and_degenerate_clicks(self):
        positions = {f"0x{value:016x}": point for value, point in {1:(0,0), 2:(5,0), 3:(0,4), 4:(5,4)}.items()}
        model = ClickLocationModel(); model.set_geometry(positions, 1)
        target = (2.0, 1.5)
        for sequence, (anchor, point) in enumerate({1:(0,0), 2:(5,0), 3:(0,4), 4:(5,4)}.items(), 1):
            state = model.observe(click(anchor, math.dist(target, point), sequence=sequence))
        self.assertEqual(state.status, "solved")
        self.assertAlmostEqual(state.result.x_m, 2.0, places=3)
        model.observe(click(1, 2, event=2, sequence=10))
        model.observe(click(2, 3, event=2, sequence=11))
        state = model.observe(click(3, math.dist((1,1),(0,4)), event=3, sequence=12))
        self.assertEqual(state.status, "pending")
        self.assertEqual(set(state.ranges_m), {"0x0000000000000003"})
        model.set_geometry({"0x0000000000000001":(0,0), "0x0000000000000002":(1,0), "0x0000000000000003":(2,0)}, 2)
        for anchor in (1,2,3): state = model.observe(click(anchor, 1, event=4, sequence=anchor))
        self.assertEqual(state.status, "invalid")
        self.assertIn("collinear", state.message)


class WakeAndTopologyTests(unittest.TestCase):
    def test_wake_boundaries_orderings_missing_and_bounded_spam(self):
        for reverse in (False, True):
            monitor = WakeTrainMonitor()
            late = WakeEvidence((1,), 1, (1,1,1), "one", 3, 10_000)
            other = WakeEvidence((2,), 1, (1,2,1), "two", 1, 10_000 + COLLISION_WINDOW_MS)
            for evidence in ((other, late) if reverse else (late, other)): monitor.observe(evidence)
            updates = dict(monitor.observe(late))
            self.assertEqual(updates.get((1,), monitor._diagnostics[(1,)]).classification, WAKE_COLLISION)
        monitor = WakeTrainMonitor(max_recent=8)
        self.assertEqual(dict(monitor.observe(WakeEvidence(("normal",), 1, (1,1,1), "one", 1, 0)))[("normal",)].classification, WAKE_NORMAL)
        self.assertEqual(dict(monitor.observe(WakeEvidence(("unknown",), 1, (1,2,1), "two", None, 5000)))[("unknown",)].classification, WAKE_UNKNOWN)
        stale = WakeEvidence(("late",), 2, (2,3,1), "three", 2, 50_000)
        self.assertEqual(dict(monitor.observe(stale))[("late",)].classification, WAKE_LATE)
        for index in range(50): monitor.observe(WakeEvidence((index,), 3, (3,index,1), str(index), 1, index * 5000))
        self.assertLessEqual(len(monitor._order), 8)

    def test_topology_exact_changes_persistence_and_explicit_accept(self):
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "baseline.json"
            model = TopologyBaselineModel(path)
            model.observe(event(anchor=1)); model.observe(event(anchor=2, event_seq=2))
            result = model.observe(event(stage=12, flags=1, total=2, event_seq=3))
            self.assertEqual(result.status, "no_baseline")
            self.assertFalse(path.exists())
            model.accept_latest(); self.assertTrue(path.exists())
            reloaded = TopologyBaselineModel(path)
            reloaded.observe(event(anchor=1, correlation=10)); reloaded.observe(event(anchor=3, event_seq=4, correlation=10))
            replacement = reloaded.observe(event(stage=12, flags=1, total=2, event_seq=5, correlation=10))
            self.assertEqual(replacement.status, "replacement")
            incomplete = reloaded.observe(event(stage=12, flags=1, status=6, reason=6, event_seq=6, correlation=11))
            self.assertEqual(incomplete.status, "incomplete")

    def test_topology_deduplicates_anchor_records(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            model.baseline = AnchorBaseline((1, 2), "then", "deployment")
            model.observe(event(anchor=1)); model.observe(event(anchor=1, event_seq=2)); model.observe(event(anchor=2, event_seq=3))
            result = model.observe(event(stage=12, flags=1, total=2, event_seq=4))
            self.assertEqual(result.status, "exact")
            self.assertEqual(result.actual, (1, 2))

    def test_topology_missing_added_and_exact_sets(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            model.baseline = AnchorBaseline((1, 2), "then", "deployment")
            for correlation, ids, expected in ((20, (1,), "missing"), (21, (1,2,3), "added"), (22, (1,2), "exact")):
                for seq, anchor in enumerate(ids, 1): model.observe(event(anchor=anchor, event_seq=seq, correlation=correlation))
                result = model.observe(event(stage=12, flags=1, total=len(ids), event_seq=10, correlation=correlation))
                self.assertEqual(result.status, expected)

    def test_command_timeline_deduplicates_replays_and_is_bounded(self):
        model = CommandTimelineModel(max_events=3)
        for seq in (1,2,2,3,4): model.observe(event(event_seq=seq))
        self.assertEqual([value.event_sequence for value in model.ordered()], [2,3,4])

    def test_command_timeline_groups_runs_merges_slots_and_keeps_terminal(self):
        model = CommandTimelineModel()
        first = event(anchor=1, event_seq=1)
        slot_update = replace(first, event_sequence=2, discovery_slot=4)
        terminal = event(stage=12, flags=1, event_seq=3, total=1, lost=2)
        for value in (first, slot_update, terminal): model.observe(value)
        self.assertEqual(model.enumerated_anchors[first.correlation_key][1].discovery_slot, 4)
        self.assertIs(model.terminal_for(first.correlation_key), terminal)


if __name__ == "__main__":
    unittest.main()
