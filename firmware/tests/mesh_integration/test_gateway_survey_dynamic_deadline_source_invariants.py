#!/usr/bin/env python3
"""Keep gateway survey collection timing tied to the remembered route depth."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text()
MACHINE = (ROOT / "src/gateway_survey_machine.c").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end()
    depth = 1
    index = start
    while index < len(source) and depth != 0:
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        index += 1
    if depth != 0:
        raise AssertionError(f"unterminated function {name}")
    return source[start : index - 1]


class GatewaySurveyDynamicDeadlineTests(unittest.TestCase):
    def test_route_depth_scan_is_bounded_by_downlink_capacity(self) -> None:
        body = function_body(SURVEY, "gateway_survey_known_max_report_hops")
        self.assertIn("mesh_relay_downlink_capacity(&mesh_runtime)", body)
        self.assertIn("mesh_relay_downlink_at(&mesh_runtime", body)
        self.assertIn("route->hop_count > max_hops", body)

    def test_collection_uses_hop_scaled_custody(self) -> None:
        body = function_body(SURVEY, "gateway_route_survey_reachability")
        lookup = body.index("gateway_survey_known_max_report_hops()")
        scale = body.index("survey_discovery_report_custody_ms(")
        deadline = body.index("collection_delay_ms =", lookup)
        self.assertLess(scale, lookup)
        self.assertLess(lookup, deadline)
        self.assertIn("report_custody_ms +", body[deadline:])
        self.assertNotIn(
            "SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS +",
            body[deadline:],
        )

    def test_expected_count_is_bound_to_the_gateway_collection_policy(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        drive = function_body(
            MACHINE, "gateway_survey_machine_collection_drive"
        )

        self.assertIn("survey_extract_expected_node_count_tlv(", route)
        machine_begin = route.index("gateway_survey_machine_begin(")
        for policy_input in (
            "emission_horizon_delay_ms",
            "collection_delay_ms",
            "expected_node_count",
            "expected_node_count_present",
        ):
            self.assertIn(policy_input, route[machine_begin:])

        decision = drive.index("survey_gateway_collection_decide(")
        for policy_input in (
            "emission_horizon_elapsed",
            "safety_deadline_elapsed",
            "report_count",
            "machine->expected_count",
            "machine->expected_count_present",
        ):
            self.assertIn(policy_input, drive[decision:])
        self.assertIn(
            "GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING",
            drive[decision:],
        )
        self.assertIn(
            "GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED",
            drive[decision:],
        )
        self.assertIsNone(
            re.search(r"\breport_count\s*=(?!=)", drive),
            "an expected-count mismatch must fail explicitly, never truncate",
        )


if __name__ == "__main__":
    unittest.main()
