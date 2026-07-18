#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth != 0:
                continue
            next_index = index + 1
            while next_index < len(source) and source[next_index].isspace():
                next_index += 1
            if next_index >= len(source) or source[next_index] != "{":
                break
            brace = next_index
            depth = 0
            for end in range(brace, len(source)):
                depth += source[end] == "{"
                depth -= source[end] == "}"
                if depth == 0:
                    return source[candidate.start() : end + 1]
            raise AssertionError(f"unterminated function {name}")
    raise AssertionError(f"missing function {name}")


class RouteRefreshSourceBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.adapter = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        self.header = (
            APP_SRC / "app_node_comm_gateway_route_refresh.h"
        ).read_text(encoding="utf-8")
        self.refresh = (
            APP_SRC / "app_node_comm_gateway_route_refresh.c"
        ).read_text(encoding="utf-8")
        self.report = read_composed_source(APP_SRC / "app_mesh_report.c")
        self.anchor = read_composed_source(APP_SRC / "app_anchor.c")
        self.flood_header = (APP_SRC / "app_mesh_flood.h").read_text(
            encoding="utf-8"
        )

    def test_route_refresh_operation_has_one_state_owner(self):
        self.assertEqual(
            1,
            self.refresh.count("static struct route_refresh_state route_refresh"),
        )
        for legacy_owner in (
            "static struct k_work_delayable gateway_route_adv_work",
            "static uint32_t gateway_route_adv_due_ms",
            "static uint32_t gateway_route_adv_response_due_ms",
            "static uint8_t gateway_route_adv_retry_round",
            "static atomic_t gateway_route_adv_forced",
            "gateway_route_adv_work_handler",
            "gateway_route_adv_retry_delay_ms",
            "gateway_route_adv_schedule",
        ):
            with self.subTest(legacy_owner=legacy_owner):
                if legacy_owner in self.report:
                    self.fail(f"legacy route-refresh owner remains: {legacy_owner}")

    def test_node_comm_lifecycle_pauses_and_resumes_route_refresh(self):
        pause = function_body(self.adapter, "app_node_comm_pause_request")
        resume = function_body(self.adapter, "app_node_comm_resume_complete")

        self.assertTrue(
            "app_node_comm_gateway_route_refresh_pause(" in pause,
            "node-comm pause must pause route-refresh timing",
        )
        self.assertTrue(
            "app_node_comm_gateway_route_refresh_resume(" in resume,
            "node-comm resume must resume route-refresh timing",
        )
        self.assertTrue(
            "app_node_comm_gateway_route_refresh_pause(uint32_t now_ms)"
            in self.header,
            "route-refresh pause hook is missing from its public header",
        )
        self.assertTrue(
            "app_node_comm_gateway_route_refresh_resume(uint32_t now_ms)"
            in self.header,
            "route-refresh resume hook is missing from its public header",
        )

    def test_route_refresh_uses_resumable_flood_progress(self):
        self.assertIn("struct app_mesh_flood_progress flood", self.refresh)
        self.assertIn("app_mesh_flood_send_bounded_resume(", self.refresh)
        self.assertIn("app_mesh_flood_progress_rebase(", self.refresh)
        self.assertNotIn("app_mesh_flood_send_bounded(", self.refresh)
        self.assertIn("struct app_mesh_flood_progress", self.flood_header)
        self.assertIn("uint8_t next_opportunity", self.flood_header)

    def test_protocol_clients_use_public_route_refresh_facade(self):
        start = function_body(self.adapter, "app_node_comm_start_route_refresh")
        request = function_body(
            self.adapter, "app_node_comm_request_route_refresh"
        )
        correlated = function_body(
            self.adapter, "app_node_comm_request_route_refresh_correlated"
        )
        bounded = function_body(
            self.adapter,
            "app_node_comm_request_route_refresh_correlated_bounded",
        )

        self.assertIn("app_node_comm_gateway_route_refresh_start()", start)
        self.assertIn("app_node_comm_gateway_route_refresh_request(", request)
        self.assertIn(
            "app_node_comm_request_route_refresh_correlated_bounded(",
            correlated,
        )
        self.assertIn(
            "app_node_comm_gateway_route_refresh_request_bounded(",
            bounded,
        )

        legacy_pattern = re.compile(
            r"\bmesh_gateway_route_adv_(?:start|request|force_request)\s*\("
        )
        for name in ("app_anchor.c", "app_mesh_report.h", "main.c"):
            source = (APP_SRC / name).read_text(encoding="utf-8")
            with self.subTest(name=name):
                self.assertIsNone(
                    legacy_pattern.search(source),
                    f"{name} bypasses the node-communication route-refresh facade",
                )

    def test_assignment_and_survey_wait_for_successful_route_refresh(self):
        route_host = function_body(self.anchor, "gateway_route_host_packet")
        readiness_policy = function_body(
            self.anchor, "gateway_host_command_requires_route_refresh_ready"
        )

        self.assertIn("CMD_ASSIGN_DISCOVERY_SLOTS", readiness_policy)
        self.assertIn("gateway_command_uses_survey_mesh", readiness_policy)
        self.assertNotIn("CMD_FORCE_REDISCOVERY", readiness_policy)

        force_position = route_host.index(
            "app_node_comm_request_route_refresh_correlated_bounded("
        )
        readiness_position = route_host.index(
            "app_node_comm_gateway_route_refresh_ready()"
        )
        assignment_position = route_host.index(
            "gateway_start_discovery_assignment("
        )
        survey_position = route_host.index("gateway_route_survey_command(")
        self.assertLess(force_position, readiness_position)
        self.assertLess(readiness_position, assignment_position)
        self.assertLess(readiness_position, survey_position)
        self.assertIn("COMMAND_BUSY", route_host)
        self.assertIn("GATEWAY_COMMAND_EVENT_REASON_BUSY", route_host)
        self.assertIn("gateway_observe_host_terminal(", route_host)
        self.assertIn("return -EBUSY", route_host)


if __name__ == "__main__":
    unittest.main()
