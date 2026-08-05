#!/usr/bin/env python3
"""Keep gateway survey collection timing tied to the remembered route depth."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text()
CONTROL = (ROOT / "app/src/app_anchor_gateway_control.inc").read_text()


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
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn("survey_extract_expected_node_count_tlv(", route)
        self.assertIn(
            "gateway_survey_collection_emission_deadline_ms", route
        )
        self.assertIn("gateway_survey_expected_node_count =", route)
        self.assertIn(
            "gateway_survey_expected_node_count_present =", route
        )

        decision = wait.index("survey_gateway_collection_decide(")
        for policy_input in (
            "gateway_survey_collection_emission_deadline_ms",
            "gateway_survey_collection_deadline_ms",
            "gateway_survey_context.report_count",
            "gateway_survey_expected_node_count",
            "gateway_survey_expected_node_count_present",
        ):
            self.assertIn(policy_input, wait[decision:])
        mismatch = wait.index(
            "SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH", decision
        )
        self.assertIn("gateway_survey_auto_finish_status(", wait[mismatch:])
        self.assertIsNone(
            re.search(r"gateway_survey_context\.report_count\s*=(?!=)", wait),
            "an expected-count mismatch must fail explicitly, never truncate",
        )

    def test_collection_safety_deadline_is_frozen_at_command_origin(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn(
            "gateway_survey_operation_deadline_ms = "
            "command_origin_ms + command_budget_ms",
            " ".join(route.split()),
        )
        self.assertIn(
            "gateway_survey_collection_deadline_ms = "
            "command_origin_ms + collection_delay_ms",
            " ".join(route.split()),
        )
        self.assertIn(
            ".terminal_scheduling_guard_ms = "
            "GATEWAY_SURVEY_OPERATION_TERMINAL_SCHEDULING_GUARD_MS",
            " ".join(route.split()),
        )
        self.assertIn(
            "gateway_survey_collection_window_armed = true", wait
        )
        self.assertNotIn(
            "k_uptime_get_32() + gateway_survey_collection_duration_ms",
            wait,
        )

    def test_post_rf_terminal_keeps_delayed_report_horizon_alive(self) -> None:
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )
        survival = re.search(
            r"survey_gateway_discovery_collection_survives_terminal\s*\("
            r"\s*event\.reason\s*==\s*NODE_COMM_TERMINAL_DELIVERED\s*,"
            r"\s*event\.attempts_started\s*\)",
            wait,
        )

        self.assertIsNotNone(survival)
        self.assertLess(
            survival.start(),
            wait.index("gateway_survey_auto_finish_status(", survival.start()),
        )
        self.assertIn(
            "gateway_survey_collection_window_armed = true",
            wait[survival.start() :],
        )

    def test_pair_plan_control_floor_is_checked_before_remote_state(self) -> None:
        worker = function_body(CONTROL, "gateway_survey_work_handler")
        plan = worker.index("survey_gateway_plan_pairs(")
        remaining = worker.index("uptime_ms_until_deadline(", plan)
        budget = worker.index(
            "survey_gateway_transaction_pair_plan_fits_minimum_budget(",
            remaining,
        )
        terminal = worker.index("gateway_survey_auto_finish_status(", budget)
        telemetry = worker.index("gateway_survey_emit_collection_telemetry()")
        next_action = worker.index("survey_gateway_auto_next_action(")

        self.assertLess(plan, remaining)
        self.assertLess(remaining, budget)
        self.assertLess(budget, terminal)
        self.assertLess(terminal, telemetry)
        self.assertLess(telemetry, next_action)
        self.assertIn("SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS", worker[budget:])


if __name__ == "__main__":
    unittest.main()
