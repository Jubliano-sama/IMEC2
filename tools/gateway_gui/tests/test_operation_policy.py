from __future__ import annotations

import unittest

from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    DiscoveryOperationPolicy,
    OperationPolicyProfile,
    PairOperationPolicy,
    decode_operation_policy_value,
)


class OperationPolicyTests(unittest.TestCase):
    def test_default_v1_profile_matches_exact_firmware_wire_shapes(self) -> None:
        profile = OperationPolicyProfile()
        assignment, discovery, pair = profile.encoded_values()

        self.assertEqual(
            assignment.hex(),
            "0101000000c9960300e803",
        )
        self.assertEqual(
            discovery.hex(),
            "0102007017000028000604fa000000c0270900",
        )
        self.assertEqual(pair.hex(), "0103000219")
        self.assertEqual(
            tuple(len(value) for value in (assignment, discovery, pair)),
            (11, 19, 5),
        )

    def test_each_family_round_trips_to_named_fields(self) -> None:
        profile = OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(5, 300_000, 750),
            discovery=DiscoveryOperationPolicy(9_000, 80, 12, 3, 1_500, 500_000),
            pair=PairOperationPolicy(1, 8),
        )

        assignment, discovery, pair = map(
            decode_operation_policy_value, profile.encoded_values()
        )
        self.assertEqual(assignment["family"], "assignment")
        self.assertEqual(assignment["expected_anchor_count"], 5)
        self.assertEqual(assignment["response_spread_ms"], 750)
        self.assertEqual(discovery["family"], "survey_discovery")
        self.assertEqual(discovery["round_count"], 3)
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
            lambda: AssignmentOperationPolicy(51, 235_209, 1_000),
            lambda: AssignmentOperationPolicy(0, 999, 1_000),
            lambda: DiscoveryOperationPolicy(5_999, 40, 6, 4, 250, 600_000),
            lambda: DiscoveryOperationPolicy(6_000, 29, 6, 4, 250, 600_000),
            lambda: DiscoveryOperationPolicy(6_000, 40, 6, 5, 250, 600_000),
            lambda: PairOperationPolicy(3, 1),
            lambda: PairOperationPolicy(2, 26),
        )
        for constructor in invalid_constructors:
            with self.subTest(constructor=constructor), self.assertRaises(ValueError):
                constructor()

        for value in (b"\x01\x01", b"\x02\x01\x00" + b"\x00" * 8,
                      b"\x01\x01\x01" + b"\x00" * 8,
                      b"\x01\x04\x00"):
            with self.subTest(value=value.hex()), self.assertRaises(ValueError):
                decode_operation_policy_value(value)


if __name__ == "__main__":
    unittest.main()
