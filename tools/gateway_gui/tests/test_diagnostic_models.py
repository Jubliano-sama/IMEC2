import math
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.gateway_gui.diagnostic_models import (
    AnchorBaseline, ClickLocationModel, CommandTimelineModel, SurveyGeometryModel,
    TopologyBaselineModel, WakeEvidence, WakeTrainMonitor,
    WAKE_COLLISION, WAKE_LATE, WAKE_NORMAL, WAKE_UNKNOWN, COLLISION_WINDOW_MS,
    command_run_status, command_step_sentence,
)
from tools.gateway_gui.diagnostic_views import MeshDiagnosticsView
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


def pair_result(
    a, b, distance, status, sequence, *, survey=10, sample_index=0,
    sample_count=1, reporter=None, operation_generation=None,
    round_id=None, round_commitment=None, packet_session=None,
):
    if operation_generation is None:
        operation_generation = survey
    if round_id is None:
        round_id = 1
    values = [
        tlv(0x15, survey.to_bytes(4, "little")),
        tlv(0x1F, a.to_bytes(8, "little")),
        tlv(0x20, b.to_bytes(8, "little")),
        tlv(0x0F, sample_count.to_bytes(2, "little")),
        tlv(0x0E, sample_index.to_bytes(2, "little")),
        tlv(0x0C, distance.to_bytes(4, "little", signed=True)),
        tlv(0x21, bytes((status,))),
    ]
    if operation_generation is not None:
        values.append(tlv(0xB6, operation_generation.to_bytes(8, "little")))
    if round_id is not None:
        values.append(tlv(0xAF, round_id.to_bytes(2, "little")))
    if round_commitment is not None:
        values.append(tlv(0xB7, round_commitment))
    payload = b"".join(values)
    session = (
        packet_session
        if packet_session is not None
        else (
            operation_generation & 0xFFFFFFFF
            if operation_generation is not None
            else survey
        )
    )
    result = packet(0x53, payload, session=session, sequence=sequence)
    return replace(result, src_id=a if reporter is None else reporter)


def event(*, kind=1, stage=6, flags=0, status=0, reason=0, event_seq=1, anchor=0, total=0, lost=0, correlation=9, gateway_sequence=5):
    success = total if stage == 12 and status == 0 and reason == 0 else 0
    return GatewayCommandEvent(kind, stage, flags, 1, status, reason, 0x104, 2, correlation, gateway_sequence, 7, 8, event_seq, anchor, 0, 0, 0, 1, total, success, 0, 0, lost, 0, 255)


def survey_pair_event(stage, a, b, survey, event_seq, *, host_session=1, host_sequence=2):
    return replace(
        event(
            kind=2, stage=stage, gateway_sequence=survey,
            event_seq=event_seq,
        ),
        command_id=0x0102,
        host_session_id=host_session,
        host_sequence=host_sequence,
        pair_initiator_id=a,
        pair_responder_id=b,
    )


class SurveyAndClickTests(unittest.TestCase):
    def test_survey_success_only_and_missing_requires_complete_opportunity(self):
        model = SurveyGeometryModel()
        model.observe_pair_packet(pair_result(1, 2, 3000, 0, 1))
        model.observe_pair_packet(pair_result(1, 3, 0, 2, 2))
        self.assertEqual(len(model.pairs), 1)
        self.assertEqual(model.missing_pairs, frozenset())
        model.observe_command_event(event(kind=2, stage=8, total=2, gateway_sequence=10))
        model.observe_command_event(event(kind=2, stage=12, flags=1, total=2, event_seq=2, gateway_sequence=10))
        self.assertEqual(len(model.missing_pairs), 1)

    def test_live_pair_payloads_parse_and_provisional_event_cannot_erase_them(self):
        model = SurveyGeometryModel()
        survey_id = 0x60572B4F
        model.begin_survey(survey_id, host_session_id=7, host_sequence=8)
        live_payloads = (
            (
                0xC1E090585A85AB56,
                "15044f2b57601f08b319ddd561dddda7200856ab855a5890e0c"
                "10f0201000e0200000c042c0400000d0164210100070884a44400"
                "000000002401a34d0221004e0463030000531801000c2044cf86"
                "fb011aff197c4d843e0198fc5c57657738",
            ),
            (
                0x6E8E6A1C97671F6F,
                "15044f2b57601f086f1f67971c6a8e6e200856ab855a5890e0c"
                "10f0201000e0200000c04aa0700000d01642101000708cecd4400"
                "000000002401a34d020e004e04a501000053180130b01e0708c0"
                "eb0154380ab77f283d01cca05b19a3b028",
            ),
        )
        for sequence, (reporter, raw) in enumerate(live_payloads, 1):
            # Preserve the captured range diagnostics while applying the
            # current production survey-run identity envelope.
            payload = (
                bytes.fromhex(raw)
                + tlv(0xB6, survey_id.to_bytes(8, "little"))
                + tlv(0xAF, (1).to_bytes(2, "little"))
            )
            live_packet = replace(
                packet(0x53, payload, session=survey_id, sequence=sequence),
                src_id=reporter,
            )
            self.assertTrue(model.observe_pair_packet(live_packet).successful)

        distances = sorted(pair.distance_m for pair in model.pairs.values())
        self.assertEqual(distances, [1.068, 1.962])
        generation = model.generation
        provisional = event(
            kind=2, stage=2, correlation=40, gateway_sequence=0,
            event_seq=99,
        )
        provisional = replace(provisional, host_session_id=7, host_sequence=8)
        model.observe_command_event(provisional)
        self.assertEqual(model.survey_id, survey_id)
        self.assertEqual(len(model.pairs), 2)
        self.assertEqual(model.generation, generation)

    def test_explicit_survey_binding_rejects_stale_pairs_and_events(self):
        model = SurveyGeometryModel()
        model.begin_survey(20, host_session_id=100, host_sequence=4)
        for sequence, pair_ids in enumerate(((1, 2), (1, 3), (2, 3)), 1):
            model.observe_command_event(survey_pair_event(
                9, *pair_ids, 20, 10 + sequence,
                host_session=100, host_sequence=4,
            ))
            model.observe_pair_packet(
                pair_result(*pair_ids, 1000 + sequence, 0, sequence, survey=20)
            )
            model.observe_command_event(survey_pair_event(
                10, *pair_ids, 20, 20 + sequence,
                host_session=100, host_sequence=4,
            ))
        model.observe_command_event(replace(
            event(kind=2, stage=12, flags=1, total=3, gateway_sequence=20),
            host_session_id=100, host_sequence=4,
        ))
        self.assertEqual(len(model.pairs), 3)
        self.assertTrue(model.terminal_complete)
        self.assertTrue(model.solve_readiness()[0])

        model.observe_pair_packet(pair_result(4, 5, 9000, 0, 10, survey=19))
        model.observe_command_event(event(
            kind=2, stage=12, flags=1, total=0, gateway_sequence=19,
            correlation=8, event_seq=200,
        ))
        self.assertEqual(model.survey_id, 20)
        self.assertEqual(len(model.pairs), 3)

        model.begin_survey(21, host_session_id=101, host_sequence=5)
        self.assertEqual(model.survey_id, 21)
        self.assertEqual(model.pairs, {})
        self.assertFalse(model.terminal_complete)

    def test_pair_results_bind_generation_round_and_commitment(self):
        model = SurveyGeometryModel()
        survey_id = 20
        generation = 0x1122334400000051
        commitment = bytes(range(32))
        model.begin_survey(survey_id, host_session_id=100, host_sequence=4)

        accepted = model.observe_pair_packet(pair_result(
            1,
            2,
            1000,
            0,
            1,
            survey=survey_id,
            operation_generation=generation,
            round_id=1,
            round_commitment=commitment,
        ))
        self.assertIsNotNone(accepted)
        self.assertEqual(len(model.pairs), 1)

        # Reusing the host survey ID and projected packet session must not let
        # an unrelated durable operation enter the active geometry run.
        stale_generation = generation - (1 << 32)
        self.assertIsNone(model.observe_pair_packet(pair_result(
            2,
            3,
            2000,
            0,
            2,
            survey=survey_id,
            operation_generation=stale_generation,
            round_id=1,
            round_commitment=commitment,
        )))

        # One synchronized round has one immutable plan commitment.
        conflicting_commitment = bytes(reversed(range(32)))
        self.assertIsNone(model.observe_pair_packet(pair_result(
            2,
            3,
            2000,
            0,
            3,
            survey=survey_id,
            operation_generation=generation,
            round_id=1,
            round_commitment=conflicting_commitment,
        )))

        # The packet header is the projected generation identity, so it must
        # agree with the full-width operation-generation TLV.
        self.assertIsNone(model.observe_pair_packet(pair_result(
            2,
            3,
            2000,
            0,
            4,
            survey=survey_id,
            operation_generation=generation,
            round_id=2,
            round_commitment=conflicting_commitment,
            packet_session=(generation + 1) & 0xFFFFFFFF,
        )))
        self.assertEqual(len(model.pairs), 1)

        # A later synchronized round may legitimately commit another plan.
        accepted_next_round = model.observe_pair_packet(pair_result(
            2,
            3,
            2000,
            0,
            5,
            survey=survey_id,
            operation_generation=generation,
            round_id=2,
            round_commitment=conflicting_commitment,
        ))
        self.assertIsNotNone(accepted_next_round)
        self.assertEqual(len(model.pairs), 2)

    def test_newer_generation_evicts_stale_first_pair_results(self):
        model = SurveyGeometryModel()
        survey_id = 20
        current_generation = 0x1122334400000051
        stale_generation = current_generation - (1 << 32)
        model.begin_survey(survey_id, host_session_id=100, host_sequence=4)

        # A replayed host packet can be delivered after a new host
        # command starts even though it belongs to the previous use of the
        # same 32-bit survey/session identity.
        self.assertIsNotNone(model.observe_pair_packet(pair_result(
            1,
            2,
            9000,
            0,
            1,
            survey=survey_id,
            operation_generation=stale_generation,
            round_id=1,
        )))

        accepted = model.observe_pair_packet(pair_result(
            2,
            3,
            2000,
            0,
            2,
            survey=survey_id,
            operation_generation=current_generation,
            round_id=1,
        ))
        self.assertIsNotNone(accepted)
        self.assertEqual(
            set(model.pairs),
            {("0x0000000000000002", "0x0000000000000003")},
        )

    def test_multi_sample_result_is_complete_and_reporter_order_independent(self):
        for reverse in (False, True):
            model = SurveyGeometryModel()
            model.begin_survey(30, host_session_id=1, host_sequence=2)
            records = [
                pair_result(1, 2, 1000, 0, 1, survey=30, sample_index=0,
                            sample_count=2, reporter=2),
                pair_result(1, 2, 1200, 0, 2, survey=30, sample_index=1,
                            sample_count=2, reporter=2),
                pair_result(1, 2, 1000, 0, 3, survey=30, sample_index=0,
                            sample_count=2, reporter=1),
                pair_result(1, 2, 1200, 0, 4, survey=30, sample_index=1,
                            sample_count=2, reporter=1),
            ]
            for record in reversed(records) if reverse else records:
                model.observe_pair_packet(record)
            self.assertEqual(len(model.pairs), 1)
            self.assertAlmostEqual(next(iter(model.pairs.values())).distance_m, 1.1)
            self.assertTrue(model.solve_readiness()[0])

            duplicate_failure = pair_result(
                1, 2, 0, 2, 5, survey=30, sample_index=1,
                sample_count=2, reporter=2,
            )
            model.observe_pair_packet(duplicate_failure)
            self.assertEqual(len(model.pairs), 1)

    def test_multi_sample_rounds_never_mix_and_latest_round_wins(self):
        records = (
            pair_result(
                1, 2, 1000, 0, 1, survey=30, sample_index=0,
                sample_count=2, round_id=1,
            ),
            pair_result(
                1, 2, 1000, 0, 2, survey=30, sample_index=1,
                sample_count=2, round_id=1,
            ),
            pair_result(
                1, 2, 9000, 0, 3, survey=30, sample_index=0,
                sample_count=2, round_id=2,
            ),
            pair_result(
                1, 2, 3000, 0, 4, survey=30, sample_index=1,
                sample_count=2, round_id=2,
            ),
        )
        for ordered_records in (records, tuple(reversed(records))):
            model = SurveyGeometryModel()
            model.begin_survey(30, host_session_id=1, host_sequence=2)
            for record in ordered_records:
                model.observe_pair_packet(record)

            distance = next(iter(model.pairs.values()))
            self.assertAlmostEqual(distance.distance_m, 6.0)
            self.assertIn("round 2", distance.source)
            self.assertEqual(
                model.observed_opportunities,
                {("0x0000000000000001", "0x0000000000000002")},
            )

        incomplete_new_round = SurveyGeometryModel()
        for record in records[:2] + records[2:3]:
            incomplete_new_round.observe_pair_packet(record)
        self.assertEqual(incomplete_new_round.pairs, {})
        self.assertEqual(incomplete_new_round.observed_opportunities, set())

    def test_usable_report_wins_over_unusable_reporter_priority(self):
        for records in (
            (
                pair_result(1, 2, -4726, 0, 1, reporter=1),
                pair_result(1, 2, 1250, 0, 2, reporter=2),
            ),
            (
                pair_result(1, 2, 1250, 0, 1, reporter=2),
                pair_result(1, 2, -4726, 0, 2, reporter=1),
            ),
        ):
            model = SurveyGeometryModel()
            for record in records:
                model.observe_pair_packet(record)

            self.assertEqual(len(model.pairs), 1)
            self.assertAlmostEqual(
                next(iter(model.pairs.values())).distance_m, 1.25
            )
            self.assertEqual(model.failures, set())

    def test_positive_short_range_is_usable_but_zero_is_not(self):
        for distance in (1, 12, 50):
            model = SurveyGeometryModel()
            observation = model.observe_pair_packet(
                pair_result(1, 2, distance, 0, 1)
            )

            self.assertIsNotNone(observation)
            self.assertTrue(observation.successful)
            self.assertAlmostEqual(
                next(iter(model.pairs.values())).distance_m,
                distance / 1000.0,
            )

        model = SurveyGeometryModel()
        observation = model.observe_pair_packet(pair_result(1, 2, 0, 0, 1))
        self.assertIsNotNone(observation)
        self.assertFalse(observation.successful)
        self.assertEqual(model.pairs, {})
        self.assertEqual(len(model.failures), 1)

    def test_pair_mutation_invalidates_existing_solution_generation(self):
        model = SurveyGeometryModel()
        model.begin_survey(40, host_session_id=1, host_sequence=2)
        model.observe_pair_packet(
            pair_result(1, 2, 1000, 0, 1, survey=40, reporter=2)
        )
        model.positions_m = {"old": (0.0, 0.0)}
        generation = model.generation
        model.observe_pair_packet(
            pair_result(1, 2, 1500, 0, 2, survey=40, reporter=1)
        )
        self.assertEqual(model.positions_m, {})
        self.assertGreater(model.generation, generation)

    def test_three_anchor_geometry_waits_for_rigid_distance_count(self):
        model = SurveyGeometryModel()
        model.begin_survey(50, host_session_id=1, host_sequence=2)
        model.observe_command_event(replace(
            event(kind=2, stage=8, total=3, gateway_sequence=50),
            host_session_id=1, host_sequence=2,
        ))
        model.observe_pair_packet(pair_result(1, 2, 1000, 0, 1, survey=50))
        model.observe_command_event(survey_pair_event(9, 1, 2, 50, 10))
        model.observe_command_event(survey_pair_event(10, 1, 2, 50, 11))
        model.observe_pair_packet(pair_result(2, 3, 1000, 0, 2, survey=50))
        model.observe_command_event(survey_pair_event(9, 2, 3, 50, 12))
        model.observe_command_event(survey_pair_event(10, 2, 3, 50, 13))
        ready, reason = model.solve_readiness()
        self.assertFalse(ready)
        self.assertIn("2/3 pair results", reason)
        model.observe_pair_packet(pair_result(1, 3, 1400, 0, 3, survey=50))
        model.observe_command_event(survey_pair_event(9, 1, 3, 50, 14))
        model.observe_command_event(survey_pair_event(10, 1, 3, 50, 15))
        model.observe_command_event(replace(
            event(
                kind=2, stage=12, flags=1, total=3,
                gateway_sequence=50, event_seq=16,
            ),
            host_session_id=1, host_sequence=2,
        ))
        self.assertTrue(model.solve_readiness()[0])

    def test_foreign_reporter_and_incomplete_multi_sample_pair_do_not_count(self):
        model = SurveyGeometryModel()
        model.begin_survey(60, host_session_id=1, host_sequence=2)
        model.observe_command_event(replace(
            event(kind=2, stage=8, total=1, gateway_sequence=60),
            host_session_id=1, host_sequence=2,
        ))
        foreign = pair_result(
            1, 2, 1000, 0, 1, survey=60, reporter=3,
        )
        self.assertIsNone(model.observe_pair_packet(foreign))
        self.assertEqual(model.observed_opportunities, set())
        partial = pair_result(
            1, 2, 1000, 0, 2, survey=60, sample_index=0,
            sample_count=2, reporter=1,
        )
        model.observe_pair_packet(partial)
        self.assertEqual(model.observed_opportunities, set())
        ready, reason = model.solve_readiness()
        self.assertFalse(ready)
        self.assertIn("0/1 pair results", reason)

    def test_locally_rigid_flip_ambiguity_is_not_ready_to_solve(self):
        model = SurveyGeometryModel()
        model.begin_survey(70, host_session_id=1, host_sequence=2)
        edges = (
            (1, 2), (1, 3), (2, 3), (1, 4), (2, 4),
        )
        model.observe_command_event(replace(
            event(kind=2, stage=8, total=len(edges), gateway_sequence=70),
            host_session_id=1, host_sequence=2,
        ))
        for sequence, edge in enumerate(edges, 1):
            model.observe_command_event(survey_pair_event(
                9, *edge, 70, 20 + sequence * 2,
            ))
            model.observe_pair_packet(
                pair_result(*edge, 1000 + sequence, 0, sequence, survey=70)
            )
            model.observe_command_event(survey_pair_event(
                10, *edge, 70, 21 + sequence * 2,
            ))
        model.observe_command_event(replace(
            event(
                kind=2, stage=12, flags=1, total=len(edges),
                gateway_sequence=70, event_seq=100,
            ),
            host_session_id=1, host_sequence=2,
        ))
        ready, reason = model.solve_readiness()
        self.assertFalse(ready)
        self.assertIn("reflected layouts", reason)

    def test_rigid_survey_graph_readiness_sweeps_two_to_fifty_anchors(self):
        for anchor_count in (2, 3, 10, 50):
            with self.subTest(anchor_count=anchor_count):
                survey_id = 100 + anchor_count
                model = SurveyGeometryModel()
                model.begin_survey(
                    survey_id, host_session_id=1, host_sequence=2
                )
                if anchor_count == 2:
                    edges = [(1, 2)]
                elif anchor_count == 3:
                    edges = [(1, 2), (1, 3), (2, 3)]
                else:
                    outer = list(range(2, anchor_count + 1))
                    edges = [(1, anchor) for anchor in outer]
                    edges.extend(
                        (outer[index], outer[(index + 1) % len(outer)])
                        for index in range(len(outer))
                    )
                model.observe_command_event(replace(
                    event(
                        kind=2, stage=8, total=len(edges),
                        gateway_sequence=survey_id,
                    ),
                    host_session_id=1, host_sequence=2,
                ))
                for sequence, edge in enumerate(edges, 1):
                    model.observe_command_event(survey_pair_event(
                        9, *edge, survey_id, 1000 + sequence * 2,
                    ))
                    model.observe_pair_packet(pair_result(
                        *edge, 1000 + sequence, 0, sequence,
                        survey=survey_id,
                    ))
                    model.observe_command_event(survey_pair_event(
                        10, *edge, survey_id, 1001 + sequence * 2,
                    ))
                model.observe_command_event(replace(
                    event(
                        kind=2, stage=12, flags=1, total=len(edges),
                        gateway_sequence=survey_id, event_seq=5000,
                    ),
                    host_session_id=1, host_sequence=2,
                ))
                ready, reason = model.solve_readiness()
                self.assertTrue(ready, reason)
                self.assertEqual(len(model.observed_opportunities), len(edges))

    def test_expected_pair_identity_set_rejects_unscheduled_replacement(self):
        model = SurveyGeometryModel()
        model.begin_survey(75, host_session_id=1, host_sequence=2)
        planned = ((1, 2), (1, 3), (2, 3))
        model.observe_command_event(replace(
            event(kind=2, stage=8, total=3, gateway_sequence=75),
            host_session_id=1, host_sequence=2,
        ))
        for sequence, edge in enumerate(planned, 1):
            model.observe_command_event(survey_pair_event(
                9, *edge, 75, 10 + sequence,
            ))
            if edge != (2, 3):
                model.observe_command_event(survey_pair_event(
                    10, *edge, 75, 20 + sequence,
                ))
        for sequence, edge in enumerate(((1, 2), (1, 3), (3, 4)), 1):
            model.observe_pair_packet(pair_result(
                *edge, 1000, 0, sequence, survey=75,
            ))
        model.observe_command_event(replace(
            event(
                kind=2, stage=12, flags=1, total=3,
                gateway_sequence=75, event_seq=30,
            ),
            host_session_id=1, host_sequence=2,
        ))
        ready, reason = model.solve_readiness()
        self.assertFalse(ready)
        self.assertIn("do not match", reason)

    def test_first_authoritative_sample_wins_and_malformed_index_is_rejected(self):
        model = SurveyGeometryModel()
        model.begin_survey(80, host_session_id=1, host_sequence=2)
        model.observe_pair_packet(pair_result(1, 2, 1000, 0, 1, survey=80))
        model.observe_pair_packet(pair_result(1, 2, 0, 2, 2, survey=80))
        self.assertEqual(next(iter(model.pairs.values())).distance_m, 1.0)

        payload = b"".join((
            tlv(0x15, (80).to_bytes(4, "little")),
            tlv(0x1F, (2).to_bytes(8, "little")),
            tlv(0x20, (3).to_bytes(8, "little")),
            tlv(0x0C, (1000).to_bytes(4, "little", signed=True)),
            tlv(0x21, b"\x00"),
        ))
        malformed = replace(packet(0x53, payload, session=80), src_id=2)
        self.assertIsNone(model.observe_pair_packet(malformed))
        self.assertEqual(len(model.pairs), 1)

        count_model = SurveyGeometryModel()
        count_model.begin_survey(81, host_session_id=1, host_sequence=2)
        count_model.observe_pair_packet(pair_result(
            1, 2, 900, 0, 1, survey=81, sample_count=2,
            sample_index=0, reporter=2,
        ))
        count_model.observe_pair_packet(pair_result(
            1, 2, 1100, 0, 2, survey=81, sample_count=1,
            sample_index=0, reporter=1,
        ))
        self.assertEqual(next(iter(count_model.pairs.values())).distance_m, 1.1)

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

    def test_interleaved_clicks_keep_independent_bounded_range_sets(self):
        positions = {
            f"0x{anchor:016x}": point
            for anchor, point in {
                1: (0, 0), 2: (5, 0), 3: (0, 4), 4: (5, 4)
            }.items()
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

        for event_id in range(100, 100 + model.MAX_TRACKED_EVENTS + 1):
            model.observe(click(1, 1.0, event=event_id, sequence=sequence))
            sequence += 1
        self.assertEqual(len(model._ranges_by_key), model.MAX_TRACKED_EVENTS)
        self.assertNotIn((7, 100, 0xC1), model._ranges_by_key)


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

    def test_topology_zero_terminal_is_incomplete_and_slot_updates_merge(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            zero = model.observe(event(stage=12, flags=1, total=0, event_seq=1))
            self.assertEqual(zero.status, "incomplete")
            self.assertFalse(zero.complete)

            first = event(anchor=0xA7DDDD61D5DD19B3, correlation=40, event_seq=2)
            slot = replace(first, discovery_slot=0, hop_count=2, event_sequence=3)
            model.observe(first)
            model.observe(first)
            model.observe(slot)
            terminal = event(
                stage=12, flags=1, correlation=40, event_seq=4, total=1
            )
            terminal = replace(terminal, success_count=1, progress_count=1)
            result = model.observe(terminal)
            self.assertEqual(result.actual, (0xA7DDDD61D5DD19B3,))
            merged = model.current_ids
            self.assertIn(0xA7DDDD61D5DD19B3, merged)

    def test_topology_missing_added_and_exact_sets(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            model.baseline = AnchorBaseline((1, 2), "then", "deployment")
            cases = (
                (20, (1,), "missing"),
                (21, (1, 2, 3), "added"),
                (22, (1, 2), "exact"),
            )
            for run_index, (correlation, ids, expected) in enumerate(cases):
                first_sequence = run_index * 10 + 1
                for offset, anchor in enumerate(ids):
                    model.observe(event(
                        anchor=anchor,
                        event_seq=first_sequence + offset,
                        correlation=correlation,
                    ))
                result = model.observe(event(
                    stage=12,
                    flags=1,
                    total=len(ids),
                    event_seq=first_sequence + len(ids),
                    correlation=correlation,
                ))
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

    def test_terminal_before_anchor_settles_and_historical_loss_does_not_poison_baseline(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            accepted = event(stage=1, event_seq=10, correlation=80, lost=7)
            terminal = event(stage=12, flags=1, event_seq=12, correlation=80, total=1, lost=7)
            model.observe(accepted)
            waiting = model.observe(terminal)
            self.assertFalse(waiting.complete)
            self.assertIn("received 0 of 1", waiting.eligibility_reason)
            complete = model.observe(event(anchor=0xA7DDDD61D5DD19B3, event_seq=11, correlation=80, lost=7))
            self.assertTrue(complete.complete)
            self.assertEqual(complete.actual, (0xA7DDDD61D5DD19B3,))
            self.assertEqual(model.accept_latest().anchor_ids, complete.actual)

            lossy = TopologyBaselineModel(Path(temporary) / "lossy.json")
            lossy.observe(event(stage=1, correlation=81, lost=7))
            lossy.observe(event(anchor=1, correlation=81, event_seq=2, lost=7))
            result = lossy.observe(event(stage=12, flags=1, correlation=81, event_seq=3, total=1, lost=8))
            self.assertFalse(result.complete)
            self.assertIn("1 telemetry event", result.eligibility_reason)

    def test_topology_stale_replay_cannot_replace_newer_live_run(self):
        stale_run = (
            event(anchor=9, event_seq=10, correlation=100, flags=0x04),
            event(
                stage=12,
                flags=0x07,
                total=1,
                event_seq=11,
                correlation=100,
            ),
        )
        current_run = (
            event(anchor=1, event_seq=100, correlation=200),
            event(anchor=2, event_seq=101, correlation=200),
            event(
                stage=12,
                flags=0x01,
                total=2,
                event_seq=102,
                correlation=200,
            ),
        )
        for records in (stale_run + current_run, current_run + stale_run):
            with TemporaryDirectory() as temporary:
                model = TopologyBaselineModel(
                    Path(temporary) / "baseline.json"
                )
                for record in records:
                    model.observe(record)

                self.assertEqual(model.current_key, current_run[0].correlation_key)
                self.assertIsNotNone(model.latest)
                self.assertTrue(model.latest.complete)
                self.assertEqual(model.latest.actual, (1, 2))
                self.assertEqual(model.accept_latest().anchor_ids, (1, 2))

    def test_topology_loss_delta_is_arrival_order_independent(self):
        records = (
            event(stage=1, event_seq=10, correlation=90, lost=7),
            event(anchor=1, event_seq=11, correlation=90, lost=7),
            event(
                stage=12,
                flags=0x01,
                total=1,
                event_seq=12,
                correlation=90,
                lost=8,
            ),
        )
        for ordered_records in (records, tuple(reversed(records))):
            with TemporaryDirectory() as temporary:
                model = TopologyBaselineModel(
                    Path(temporary) / "baseline.json"
                )
                for record in ordered_records:
                    result = model.observe(record)

                self.assertIsNotNone(result)
                self.assertFalse(result.complete)
                self.assertIn("1 telemetry event", result.eligibility_reason)

    def test_topology_replay_only_and_superseded_runs_are_not_eligible(self):
        with TemporaryDirectory() as temporary:
            model = TopologyBaselineModel(Path(temporary) / "baseline.json")
            model.observe(event(
                anchor=9,
                event_seq=10,
                correlation=100,
                flags=0x04,
            ))
            replay = model.observe(event(
                stage=12,
                flags=0x07,
                total=1,
                event_seq=11,
                correlation=100,
            ))
            self.assertFalse(replay.complete)
            self.assertIn("replay", replay.eligibility_reason.lower())
            with self.assertRaisesRegex(ValueError, "replay"):
                model.accept_latest()

            model.observe(event(
                anchor=1,
                event_seq=20,
                correlation=200,
            ))
            current = model.observe(event(
                stage=12,
                flags=0x01,
                total=1,
                event_seq=21,
                correlation=200,
            ))
            self.assertTrue(current.complete)

            self.assertIsNone(model.observe(event(
                stage=1,
                event_seq=30,
                correlation=300,
            )))
            self.assertIsNone(model.latest)
            with self.assertRaisesRegex(ValueError, "terminal result"):
                model.accept_latest()

    def test_human_facing_run_statuses_steps_and_visible_columns(self):
        expected_steps = {
            1: "accepted", 2: "queued", 3: "preparing", 4: "Broadcast attempt",
            5: "Retrying", 6: "Anchor", 7: "collection finished", 8: "schedule",
            9: "started", 10: "succeeded", 11: "failed", 12: "Completed",
        }
        for stage, phrase in expected_steps.items():
            value = event(stage=stage, flags=1 if stage == 12 else 0, total=1 if stage == 12 else 0)
            self.assertIn(phrase.lower(), command_step_sentence(value).lower())
        self.assertEqual(command_run_status((event(stage=12, flags=1, total=0),))[0], "Incomplete")
        self.assertEqual(command_run_status((event(stage=1, lost=2), event(stage=12, flags=1, event_seq=2, total=1, lost=3)))[0], "Succeeded with warnings")
        for reason, expected in ((2, "Rejected"), (6, "Timed out"), (9, "Timed out"), (1, "Failed")):
            terminal = event(stage=12, flags=1, status=2, reason=reason)
            self.assertEqual(command_run_status((terminal,))[0], expected)
        survey_no_reports = event(kind=2, stage=12, flags=1, status=5, reason=3)
        self.assertEqual(
            command_run_status((survey_no_reports,)),
            ("Failed", "No survey reports were received before the collection deadline."),
        )
        self.assertEqual(
            command_step_sentence(survey_no_reports),
            "Command ended: no survey reports were received.",
        )
        detailed_failure = replace(
            event(kind=2, stage=11, status=5, reason=9, total=6),
            command_id=0x0102,
            attempt=4,
            anchor_id=0x1111,
            pair_initiator_id=0x1111,
            pair_responder_id=0x2222,
            success_count=1,
            failure_count=2,
        )
        self.assertEqual(
            command_step_sentence(detailed_failure),
            "Pair 0x0000000000001111 -> 0x0000000000002222 failed during "
            "START to initiator 0x0000000000001111 after 4 gateway control "
            "attempts: retry exhausted.",
        )
        detailed_terminal = replace(
            event(kind=2, stage=12, flags=1, status=5, reason=9, total=6,
                  event_seq=2),
            success_count=1,
            failure_count=5,
        )
        self.assertEqual(
            command_run_status((detailed_failure, detailed_terminal)),
            (
                "Timed out",
                "Survey ended with 1 pair(s) succeeded and 5 failed. Last "
                "failure: Pair 0x0000000000001111 -> 0x0000000000002222 "
                "failed during START to initiator 0x0000000000001111 after "
                "4 gateway control attempts: retry exhausted.",
            ),
        )
        self.assertEqual(MeshDiagnosticsView.RUN_COLUMNS,
                         ("Started", "Command", "Status", "Anchors / Pairs", "Attempts", "Result"))
        self.assertEqual(MeshDiagnosticsView.ANCHOR_COLUMNS,
                         ("Anchor ID", "Hop to gateway", "Discovery slot", "Reply status", "Last seen", "Baseline comparison"))


if __name__ == "__main__":
    unittest.main()
