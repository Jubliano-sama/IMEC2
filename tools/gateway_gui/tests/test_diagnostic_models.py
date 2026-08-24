import math
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.gateway_gui.command_telemetry import GatewayCommandEvent
from tools.gateway_gui.diagnostic_models import (
    AnchorBaseline,
    ClickLocationModel,
    CommandTimelineModel,
    TopologyBaselineModel,
    WakeEvidence,
    WakeTrainMonitor,
    WAKE_COLLISION,
    WAKE_LATE,
    WAKE_NORMAL,
    WAKE_UNKNOWN,
    COLLISION_WINDOW_MS,
    command_run_status,
    command_step_sentence,
)
from tools.gateway_gui.protocol import Packet, parse_tlvs


def tlv(kind, value):
    return bytes((kind, len(value))) + value


def packet(msg_type, payload, *, session=7, sequence=1, age=0):
    return Packet(
        "test", payload, None, msg_type, 0, 1, 2, session, sequence, None,
        age, "gateway_queue_age_ms", payload, parse_tlvs(payload),
    )


def click(anchor, distance_m, *, event=1, session=7, sequence=1, clicker=0xC1):
    payload = b"".join((
        tlv(0x06, event.to_bytes(4, "little")),
        tlv(0x0B, clicker.to_bytes(8, "little")),
        tlv(0x0A, anchor.to_bytes(8, "little")),
        tlv(0x0C, round(distance_m * 1000).to_bytes(4, "little", signed=True)),
        tlv(0x21, b"\x00"),
    ))
    return packet(0x20, payload, session=session, sequence=sequence)


def event(
    *, kind=1, stage=6, flags=0, status=0, reason=0, event_seq=1,
    anchor=0, total=0, lost=0, correlation=9, gateway_sequence=5,
):
    success = total if stage == 12 and status == 0 and reason == 0 else 0
    return GatewayCommandEvent(
        kind, stage, flags, 1, status, reason, 0x104, 2, correlation,
        gateway_sequence, 7, 8, event_seq, anchor, 0, 0, 0, 1, total,
        success, 0, 0, lost, 0, 255,
    )


class ClickLocationTests(unittest.TestCase):
    def test_exact_noisy_rapid_and_degenerate_clicks(self):
        coordinates = {1: (0, 0), 2: (5, 0), 3: (0, 4), 4: (5, 4)}
        positions = {
            f"0x{anchor:016x}": point
            for anchor, point in coordinates.items()
        }
        model = ClickLocationModel()
        model.set_geometry(positions, 1)
        target = (2.0, 1.5)
        for sequence, (anchor, point) in enumerate(coordinates.items(), 1):
            state = model.observe(
                click(anchor, math.dist(target, point), sequence=sequence)
            )
        self.assertEqual(state.status, "solved")
        self.assertAlmostEqual(state.result.x_m, 2.0, places=3)

        model.observe(click(1, 2, event=2, sequence=10))
        model.observe(click(2, 3, event=2, sequence=11))
        state = model.observe(
            click(3, math.dist((1, 1), (0, 4)), event=3, sequence=12)
        )
        self.assertEqual(state.status, "pending")
        self.assertEqual(set(state.ranges_m), {"0x0000000000000003"})

        model.set_geometry({
            "0x0000000000000001": (0, 0),
            "0x0000000000000002": (1, 0),
            "0x0000000000000003": (2, 0),
        }, 2)
        for anchor in (1, 2, 3):
            state = model.observe(click(anchor, 1, event=4, sequence=anchor))
        self.assertEqual(state.status, "invalid")
        self.assertIn("collinear", state.message)

    def test_interleaved_clicks_keep_independent_bounded_range_sets(self):
        coordinates = {1: (0, 0), 2: (5, 0), 3: (0, 4), 4: (5, 4)}
        positions = {
            f"0x{anchor:016x}": point
            for anchor, point in coordinates.items()
        }
        model = ClickLocationModel()
        model.set_geometry(positions, 1)
        targets = {10: (2.0, 1.5), 20: (3.0, 2.5)}
        solved = {}
        sequence = 1
        for anchor in (1, 2, 3):
            for event_id, target in targets.items():
                state = model.observe(click(
                    anchor,
                    math.dist(target, positions[f"0x{anchor:016x}"]),
                    event=event_id,
                    sequence=sequence,
                ))
                sequence += 1
                if state is not None and state.status == "solved":
                    solved[event_id] = state.result

        self.assertEqual(set(solved), set(targets))
        for event_id, target in targets.items():
            self.assertAlmostEqual(solved[event_id].x_m, target[0], places=3)
            self.assertAlmostEqual(solved[event_id].y_m, target[1], places=3)


class WakeAndTopologyTests(unittest.TestCase):
    def test_wake_classification_and_bounded_history(self):
        monitor = WakeTrainMonitor(max_recent=8)
        normal = WakeEvidence(("normal",), 1, (1, 1, 1), "one", 1, 0)
        unknown = WakeEvidence(("unknown",), 1, (1, 2, 1), "two", None, 5000)
        late = WakeEvidence(("late",), 2, (2, 3, 1), "three", 2, 50_000)
        self.assertEqual(dict(monitor.observe(normal))[("normal",)].classification, WAKE_NORMAL)
        self.assertEqual(dict(monitor.observe(unknown))[("unknown",)].classification, WAKE_UNKNOWN)
        self.assertEqual(dict(monitor.observe(late))[("late",)].classification, WAKE_LATE)

        nearby = WakeEvidence(
            ("nearby",), 2, (2, 4, 1), "four", 1,
            50_000 + COLLISION_WINDOW_MS,
        )
        monitor.observe(nearby)
        self.assertEqual(monitor._diagnostics[("late",)].classification, WAKE_COLLISION)
        for index in range(50):
            monitor.observe(WakeEvidence(
                (index,), 3, (3, index, 1), str(index), 1, index * 5000,
            ))
        self.assertLessEqual(len(monitor._order), 8)

    def test_topology_persistence_requires_explicit_accept(self):
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "baseline.json"
            model = TopologyBaselineModel(path)
            model.observe(event(anchor=1))
            model.observe(event(anchor=2, event_seq=2))
            result = model.observe(event(stage=12, flags=1, total=2, event_seq=3))
            self.assertEqual(result.status, "no_baseline")
            self.assertFalse(path.exists())
            model.accept_latest()
            self.assertTrue(path.exists())

            reloaded = TopologyBaselineModel(path)
            reloaded.observe(event(anchor=1, correlation=10))
            reloaded.observe(event(anchor=3, event_seq=4, correlation=10))
            replacement = reloaded.observe(
                event(stage=12, flags=1, total=2, event_seq=5, correlation=10)
            )
            self.assertEqual(replacement.status, "replacement")

    def test_topology_deduplicates_anchor_records(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            model.baseline = AnchorBaseline((1, 2), "then", "deployment")
            model.observe(event(anchor=1))
            model.observe(event(anchor=1, event_seq=2))
            model.observe(event(anchor=2, event_seq=3))
            result = model.observe(event(stage=12, flags=1, total=2, event_seq=4))
            self.assertEqual(result.status, "exact")
            self.assertEqual(result.actual, (1, 2))

    def test_command_timeline_deduplicates_and_keeps_terminal(self):
        model = CommandTimelineModel(max_events=3)
        for sequence in (1, 2, 2, 3, 4):
            model.observe(event(event_seq=sequence))
        self.assertEqual(
            [value.event_sequence for value in model.ordered()],
            [2, 3, 4],
        )

        terminal = event(stage=12, flags=1, event_seq=5, total=1)
        model.observe(terminal)
        self.assertIs(model.terminal_for(terminal.correlation_key), terminal)

    def test_human_facing_assignment_statuses(self):
        expected_steps = {
            1: "accepted",
            2: "queued",
            3: "preparing",
            4: "broadcast attempt",
            5: "retrying",
            6: "anchor",
            7: "collection finished",
            12: "completed",
        }
        for stage, phrase in expected_steps.items():
            value = event(
                stage=stage,
                flags=1 if stage == 12 else 0,
                total=1 if stage == 12 else 0,
            )
            self.assertIn(phrase, command_step_sentence(value).lower())
        self.assertEqual(
            command_run_status((event(stage=12, flags=1, total=0),))[0],
            "Incomplete",
        )


if __name__ == "__main__":
    unittest.main()
