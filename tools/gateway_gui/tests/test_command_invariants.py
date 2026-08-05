import itertools
import random
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.gateway_gui.command_telemetry import GatewayCommandRequestTracker
from tools.gateway_gui.diagnostic_models import (
    AnchorBaseline, CommandTimelineModel, TopologyBaselineModel, command_run_status,
)
from tools.gateway_gui.protocol import build_anchor_discovery_command
from tools.gateway_gui.tests.test_diagnostic_models import event


class CommandLifecycleProperties(unittest.TestCase):
    """INVARIANT command_lifecycle_single_owner_eventual_release."""

    def test_exhaustive_terminal_paths_release_and_later_submit_succeeds(self):
        for status, reason, expected in (
            (0, 0, "complete"), (2, 1, "failed"), (3, 2, "failed"),
            (5, 6, "failed"), (8, 13, "failed"),
        ):
            tracker = GatewayCommandRequestTracker(timeout_s=10)
            self.assertTrue(tracker.begin(1, 100, 1, now=0))
            for stale_session, stale_sequence in itertools.product((99, 100, 101), (0, 1, 2)):
                stale = replace(event(stage=12, flags=1, status=status, reason=reason),
                                host_session_id=stale_session, host_sequence=stale_sequence)
                released = tracker.observe_event(stale)
                self.assertEqual(released, (stale_session, stale_sequence) == (100, 1))
                if released:
                    break
            self.assertIsNone(tracker.pending)
            self.assertEqual(tracker.last_outcome, expected)
            self.assertTrue(tracker.begin(1, 101, 2, now=1))

    def test_generated_submit_disconnect_timeout_duplicate_stale_sequences(self):
        randomizer = random.Random(0x1A2B3C4D)
        for _case in range(200):
            tracker = GatewayCommandRequestTracker(timeout_s=5)
            active = None
            for step in range(30):
                operation = randomizer.choice(("submit", "terminal", "stale", "expire", "disconnect"))
                if operation == "submit":
                    identity = (step + 1, step + 10)
                    accepted = tracker.begin(1, *identity, now=float(step))
                    if accepted:
                        active = identity
                    self.assertEqual(tracker.pending is not None, active is not None)
                elif operation == "terminal" and active:
                    terminal = replace(event(stage=12, flags=1), host_session_id=active[0], host_sequence=active[1])
                    self.assertTrue(tracker.observe_event(terminal)); active = None
                elif operation == "stale":
                    stale = replace(event(stage=12, flags=1), host_session_id=0xDEAD, host_sequence=0xBEEF)
                    self.assertFalse(tracker.observe_event(stale))
                elif operation == "expire":
                    if tracker.expire(now=float(step + 10)):
                        active = None
                else:
                    tracker.disconnect(); active = None
                self.assertEqual(tracker.pending is not None, active is not None)


class TelemetryConsistencyProperties(unittest.TestCase):
    """INVARIANT telemetry_dedup_totals_and_order_converge."""

    def test_generated_order_duplicate_terminal_convergence(self):
        randomizer = random.Random(0x56)
        for count in range(1, 9):
            details = [event(anchor=index + 1, event_seq=100 + index, total=0, lost=4)
                       for index in range(count)]
            terminal = event(stage=12, flags=1, event_seq=200, total=count, lost=4)
            terminal = replace(terminal, progress_count=count, success_count=count)
            records = details + details[: count // 2] + [terminal]
            randomizer.shuffle(records)
            timeline = CommandTimelineModel()
            with TemporaryDirectory() as temporary:
                topology = TopologyBaselineModel(Path(temporary) / "baseline.json")
                result = None
                for record in records:
                    timeline.observe(record); result = topology.observe(record) or result
                self.assertTrue(result.complete)
                self.assertEqual(len(result.actual), count)
                self.assertEqual(command_run_status(timeline.runs()[0][1])[0], "Succeeded")

    def test_missing_detail_and_run_local_loss_never_report_success(self):
        for missing, loss_delta in itertools.product((0, 1), (0, 1)):
            with TemporaryDirectory() as temporary:
                model = TopologyBaselineModel(Path(temporary) / "baseline.json")
                model.observe(event(stage=1, lost=8))
                if not missing:
                    model.observe(event(anchor=1, event_seq=2, lost=8))
                result = model.observe(event(stage=12, flags=1, total=1, event_seq=3, lost=8 + loss_delta))
                self.assertEqual(result.complete, not missing and not loss_delta)


class EnumerationTopologyAlgebraProperties(unittest.TestCase):
    """INVARIANT topology_is_set_algebra_and_explicitly_persisted."""

    def test_exhaustive_small_set_partitions_are_order_and_duplicate_independent(self):
        universe = (1, 2, 3, 4)
        subsets = [set(items) for size in range(5) for items in itertools.combinations(universe, size)]
        for expected in subsets:
            for actual in subsets:
                if not actual:
                    continue
                with TemporaryDirectory() as temporary:
                    path = Path(temporary) / "baseline.json"
                    model = TopologyBaselineModel(path)
                    model.baseline = AnchorBaseline(tuple(sorted(expected)), "then", "generated")
                    ordered = list(reversed(sorted(actual))) + list(sorted(actual))
                    for sequence, anchor in enumerate(ordered, 1):
                        model.observe(event(anchor=anchor, event_seq=sequence))
                    result = model.observe(event(stage=12, flags=1, total=len(actual), event_seq=99))
                    self.assertEqual(set(result.added), actual - expected)
                    self.assertEqual(set(result.missing), expected - actual)
                    self.assertFalse(path.exists())
                    model.accept_latest(); self.assertTrue(path.exists())
                    self.assertEqual(TopologyBaselineModel(path).baseline.anchor_ids, tuple(sorted(actual)))


class SurveyParameterPartitionProperties(unittest.TestCase):
    """INVARIANT survey_builder_accepts_exact_valid_partition_only."""

    def test_generated_boundary_partitions_have_specific_validation_reasons(self):
        base = dict(host_id=1, gateway_id=2, session_id=3, seq=4,
                    survey_id=5, duration_ms=250, discovery_slot_count=6, sample_count=1)
        partitions = {
            "survey ID": ("survey_id", (0, 0x1_0000_0000)),
            "duration": ("duration_ms", (0, 0x1_0000_0000)),
            "discovery slot count": ("discovery_slot_count", (0, 51)),
            "sample count": ("sample_count", (0, 5)),
        }
        for phrase, (field, invalid_values) in partitions.items():
            for value in invalid_values:
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, phrase):
                    build_anchor_discovery_command(**(base | {field: value}))
        for slots, samples in itertools.product((1, 6, 50), (1, 2, 4)):
            command = build_anchor_discovery_command(**(base | {
                "discovery_slot_count": slots, "sample_count": samples}))
            self.assertGreater(len(command.packet.payload), 0)


if __name__ == "__main__":
    unittest.main()
