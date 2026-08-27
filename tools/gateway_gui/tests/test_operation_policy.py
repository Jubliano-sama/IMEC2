from __future__ import annotations

import unittest

from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    OperationPolicyProfile,
    assignment_required_budget_ms,
    decode_operation_policy_value,
)


class OperationPolicyTests(unittest.TestCase):
    def test_default_profile_matches_firmware_wire_shape(self) -> None:
        (assignment,) = OperationPolicyProfile().encoded_values()

        self.assertEqual(assignment.hex(), "010100000040771b00e803")
        self.assertEqual(len(assignment), 11)

    def test_assignment_round_trips_to_named_fields(self) -> None:
        policy = AssignmentOperationPolicy(5, 1_600_000, 750)
        decoded = decode_operation_policy_value(policy.encode_value())

        self.assertEqual(decoded["family"], "assignment")
        self.assertEqual(decoded["expected_anchor_count"], 5)
        self.assertEqual(decoded["response_spread_ms"], 750)
        self.assertFalse(decoded["ram_only_iteration"])

    def test_ram_only_iteration_reuses_flags_byte(self) -> None:
        durable = AssignmentOperationPolicy(expected_anchor_count=3)
        bench = AssignmentOperationPolicy(
            expected_anchor_count=3,
            ram_only_iteration=True,
        )

        self.assertEqual(len(durable.encode_value()), len(bench.encode_value()))
        self.assertEqual(0, durable.encode_value()[2])
        self.assertEqual(1, bench.encode_value()[2])
        self.assertTrue(
            decode_operation_policy_value(bench.encode_value())[
                "ram_only_iteration"
            ]
        )

    def test_invalid_bounds_versions_flags_and_lengths_fail_closed(self) -> None:
        invalid_constructors = (
            lambda: AssignmentOperationPolicy(51, 1_591_204, 1_000),
            lambda: AssignmentOperationPolicy(0, 999, 1_000),
            lambda: AssignmentOperationPolicy(0, 1_799_999, 1_000),
            lambda: AssignmentOperationPolicy(3, 418_523, 1_000),
        )
        for constructor in invalid_constructors:
            with self.subTest(constructor=constructor), self.assertRaises(ValueError):
                constructor()

        for value in (
            b"\x01\x01",
            b"\x02\x01\x00" + b"\x00" * 8,
            b"\x01\x01\x02" + b"\x00" * 8,
            b"\x01\x04\x00",
        ):
            with self.subTest(value=value.hex()), self.assertRaises(ValueError):
                decode_operation_policy_value(value)

    def test_assignment_budget_uses_safe_preclaim_roster_bound(self) -> None:
        self.assertEqual(1_800_000, assignment_required_budget_ms(1_000))
        self.assertEqual(418_524, assignment_required_budget_ms(1_000, 3))
        self.assertEqual(421_064, assignment_required_budget_ms(10_000, 3))
        self.assertEqual(1_800_000, assignment_required_budget_ms(1_000, 50))


if __name__ == "__main__":
    unittest.main()
