from __future__ import annotations

import unittest

from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    DISCOVERY_DEFAULT_BUDGET_MS,
    DISCOVERY_DEFAULT_REPORT_GRACE_MS,
    DISCOVERY_DEFAULT_ROUND_COUNT,
    DISCOVERY_DEFAULT_SLOT_COUNT,
    DISCOVERY_DEFAULT_SLOT_MS,
    DISCOVERY_DEFAULT_START_DELAY_MS,
    DISCOVERY_REPORT_CUSTODY_MAX_MS,
    DiscoveryOperationPolicy,
    OperationPolicyProfile,
    PairOperationPolicy,
    assignment_required_budget_ms,
    decode_operation_policy_value,
    discovery_required_budget_ms,
    discovery_required_start_delay_ms,
    survey_estimated_duration_ms,
)


class OperationPolicyTests(unittest.TestCase):
    def test_full_survey_estimate_scales_with_n_d_s_and_live_pair_plan(self) -> None:
        self.assertEqual(246_100, survey_estimated_duration_ms(3, 1, 3))
        self.assertEqual(255_180, survey_estimated_duration_ms(3, 2, 3))
        self.assertEqual(264_260, survey_estimated_duration_ms(3, 3, 3))
        self.assertEqual(466_060, survey_estimated_duration_ms(4, 2, 4))
        self.assertEqual(
            179_100,
            survey_estimated_duration_ms(3, 1, 3, pair_count=2),
        )
        with self.assertRaises(ValueError):
            survey_estimated_duration_ms(4, 2, 3)

    def test_default_v1_profile_matches_exact_firmware_wire_shapes(self) -> None:
        profile = OperationPolicyProfile()
        assignment, discovery, pair = profile.encoded_values()

        self.assertEqual(
            assignment.hex(),
            "010100000040771b00e803",
        )
        self.assertEqual(
            discovery.hex(),
            "01020010620000c8000604fa000000b5f80300",
        )
        self.assertEqual(pair.hex(), "0103000219")
        self.assertEqual(
            tuple(len(value) for value in (assignment, discovery, pair)),
            (11, 19, 5),
        )

    def test_each_family_round_trips_to_named_fields(self) -> None:
        profile = OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(5, 1_600_000, 750),
            discovery=DiscoveryOperationPolicy(
                25_104, 200, 12, 4, 1_500, 500_000
            ),
            pair=PairOperationPolicy(1, 8),
        )

        assignment, discovery, pair = map(
            decode_operation_policy_value, profile.encoded_values()
        )
        self.assertEqual(assignment["family"], "assignment")
        self.assertEqual(assignment["expected_anchor_count"], 5)
        self.assertEqual(assignment["response_spread_ms"], 750)
        self.assertEqual(discovery["family"], "survey_discovery")
        self.assertEqual(discovery["round_count"], 4)
        self.assertEqual(discovery["operation_budget_ms"], 500_000)
        self.assertEqual(pair, {
            "version": 1,
            "family": "survey_pair",
            "flags": 0,
            "max_reruns": 1,
            "max_parallel_pairs": 8,
        })

    def test_invalid_bounds_versions_flags_and_lengths_fail_closed(self) -> None:
        invalid_constructors = (
            lambda: AssignmentOperationPolicy(51, 1_591_204, 1_000),
            lambda: AssignmentOperationPolicy(0, 999, 1_000),
            lambda: AssignmentOperationPolicy(0, 1_799_999, 1_000),
            lambda: AssignmentOperationPolicy(3, 418_523, 1_000),
            lambda: DiscoveryOperationPolicy(1_999, 40, 6, 4, 250, 600_000),
            lambda: DiscoveryOperationPolicy(20_000, 29, 6, 4, 250, 600_000),
            lambda: DiscoveryOperationPolicy(20_000, 40, 6, 5, 250, 600_000),
            lambda: DiscoveryOperationPolicy(20_000, 40, 6, 4, 250, 139_992),
            lambda: PairOperationPolicy(3, 1),
            lambda: PairOperationPolicy(2, 26),
        )
        for constructor in invalid_constructors:
            with self.subTest(constructor=constructor), self.assertRaises(ValueError):
                constructor()

        self.assertEqual(1_800_000, assignment_required_budget_ms(20))
        self.assertEqual(1_800_000, assignment_required_budget_ms(1_000))
        self.assertEqual(1_800_000, assignment_required_budget_ms(10_000))
        self.assertEqual(418_524, assignment_required_budget_ms(1_000, 3))
        # Enumeration budgets the pre-CLAIM rectangular-chain bound D=N;
        # a stored live depth may size survey, but cannot shrink this request.
        self.assertEqual(418_524, assignment_required_budget_ms(1_000, 3, 1))
        self.assertEqual(418_524, assignment_required_budget_ms(1_000, 3, 2))
        self.assertEqual(418_524, assignment_required_budget_ms(1_000, 3, 3))
        self.assertEqual(421_064, assignment_required_budget_ms(10_000, 3))
        self.assertEqual(1_800_000, assignment_required_budget_ms(1_000, 50))
        self.assertLessEqual(assignment_required_budget_ms(10_000), 1_800_000)
        self.assertEqual(
            260_277,
            discovery_required_budget_ms(25_104, 200, 6, 4, 250),
        )
        self.assertEqual(
            131_833,
            discovery_required_budget_ms(20_000, 200, 6, 4, 250, 1),
        )
        self.assertEqual(
            167_073,
            discovery_required_budget_ms(20_000, 200, 6, 4, 250, 3),
        )
        self.assertEqual(58_000, DISCOVERY_REPORT_CUSTODY_MAX_MS)

        for value in (b"\x01\x01", b"\x02\x01\x00" + b"\x00" * 8,
                      b"\x01\x01\x01" + b"\x00" * 8,
                      b"\x01\x04\x00"):
            with self.subTest(value=value.hex()), self.assertRaises(ValueError):
                decode_operation_policy_value(value)

    def test_discovery_start_delay_tracks_known_depth_and_unknown_is_safe(self) -> None:
        expected_by_depth = {
            1: 11_104,
            2: 13_104,
            3: 15_104,
            4: 17_104,
            5: 19_104,
            6: 21_104,
            7: 23_104,
            8: 25_104,
            0: 25_104,
        }
        for deepest_hop, expected_ms in expected_by_depth.items():
            with self.subTest(deepest_hop=deepest_hop):
                self.assertEqual(
                    expected_ms,
                    discovery_required_start_delay_ms(deepest_hop),
                )

        with self.assertRaisesRegex(ValueError, "deepest hop"):
            discovery_required_start_delay_ms(9)

    def test_default_discovery_budget_covers_eight_hop_custody_exactly(self) -> None:
        self.assertEqual(25_104, DISCOVERY_DEFAULT_START_DELAY_MS)
        self.assertEqual(58_000, DISCOVERY_REPORT_CUSTODY_MAX_MS)
        self.assertEqual(
            260_277,
            discovery_required_budget_ms(
                DISCOVERY_DEFAULT_START_DELAY_MS,
                DISCOVERY_DEFAULT_SLOT_MS,
                DISCOVERY_DEFAULT_SLOT_COUNT,
                DISCOVERY_DEFAULT_ROUND_COUNT,
                DISCOVERY_DEFAULT_REPORT_GRACE_MS,
                deepest_hop=8,
            ),
        )
        self.assertEqual(260_277, DISCOVERY_DEFAULT_BUDGET_MS)


if __name__ == "__main__":
    unittest.main()
