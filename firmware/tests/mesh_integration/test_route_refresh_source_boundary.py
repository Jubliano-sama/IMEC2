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

    def test_route_refresh_owns_gateway_rx_control_through_every_rf_exit(self):
        worker = function_body(self.refresh, "refresh_work_handler")
        begin = function_body(self.report, "mesh_route_refresh_begin_radio_control")
        end = function_body(self.report, "mesh_route_refresh_end_radio_control")

        self.assertIn("int (*begin_radio_control)(void *ctx)", self.header)
        self.assertIn("void (*end_radio_control)(void *ctx)", self.header)
        self.assertIn(
            ".begin_radio_control = mesh_route_refresh_begin_radio_control",
            self.report,
        )
        self.assertIn(
            ".end_radio_control = mesh_route_refresh_end_radio_control",
            self.report,
        )

        self.assertIn("mesh_rx_handoff_begin_control(&abort_scan)", begin)
        self.assertIn("mesh_stop_role_scan()", begin)
        self.assertIn("mesh_rx_handoff_wait_for_control()", begin)
        wait_failure = begin.index("if (ret < 0)")
        wait_failure_end = begin.index("return ret;", wait_failure)
        self.assertIn(
            "mesh_rx_handoff_end_control()",
            begin[wait_failure:wait_failure_end],
        )
        self.assertIn("mesh_rx_handoff_end_control()", end)

        self.assertEqual(1, worker.count("config->begin_radio_control("))
        self.assertEqual(1, worker.count("config->end_radio_control("))
        acquire = worker.index("ret = config->begin_radio_control(config->ctx)")
        acquired = worker.index("radio_control_started = true", acquire)
        stop_scan = worker.index("config->stop_role_scan(config->ctx)", acquired)
        wake = worker.index("config->send_wake(config->ctx", stop_scan)
        flood = worker.index("app_mesh_flood_send_bounded_resume(", wake)
        finish = worker.index("\nfinish:", flood)
        self.assertLess(acquire, acquired)
        self.assertLess(acquired, stop_scan)
        self.assertLess(stop_scan, wake)
        self.assertLess(wake, flood)
        self.assertLess(flood, finish)
        self.assertIn("if (ret < 0)", worker[acquire:acquired])
        self.assertIn("goto finish;", worker[acquire:acquired])

        owned_rf_region = worker[acquired:finish]
        self.assertNotIn(
            "return;",
            owned_rf_region,
            "an acquired route-refresh control handoff must exit through cleanup",
        )
        pause_checks = owned_rf_region.split(
            "refresh_operation_pause_requested()"
        )[1:]
        self.assertGreaterEqual(len(pause_checks), 2)
        for pause_exit in pause_checks:
            self.assertIn("goto finish;", pause_exit[:500])

        cleanup = worker[finish:]
        release = cleanup.index(
            "config->end_radio_control(config->ctx)"
        )
        restart = cleanup.index("config->restart_role_scan(config->ctx)")
        state_lock = cleanup.index("app_node_comm_sync_lock()")
        self.assertIn("radio_control_started", cleanup[:release])
        self.assertLess(release, restart)
        self.assertLess(release, state_lock)

    def test_protocol_clients_use_public_route_refresh_facade(self):
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

        self.assertNotIn("app_node_comm_start_route_refresh", self.adapter)
        self.assertNotIn(
            "app_node_comm_gateway_route_refresh_start", self.header
        )
        self.assertNotIn(
            "app_node_comm_start_route_refresh", (APP_SRC / "main.c").read_text(
                encoding="utf-8"
            )
        )

    def test_firmware_has_no_hidden_route_refresh_readiness_gate(self):
        route_host = function_body(self.anchor, "gateway_route_host_packet")

        self.assertNotIn(
            "gateway_host_command_requires_route_refresh_ready", self.anchor
        )
        self.assertNotIn("app_node_comm_gateway_route_refresh_ready", route_host)
        self.assertIn("gateway_start_discovery_assignment(", route_host)
        self.assertIn("gateway_route_survey_command(", route_host)
        self.assertIn(
            "app_node_comm_request_route_refresh_correlated_bounded(", route_host
        )


if __name__ == "__main__":
    unittest.main()
