#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"

# Existing callers are migration debt, not precedent for new protocol modules.
LEGACY_REPORT_CLIENTS = {
    "app_anchor.c",
    "app_anchor_survey_discovery.c",
    "app_gateway_ble.c",
    "app_mesh_test.c",
    "main.c",
}
BOUNDARY_IMPLEMENTATION = {
    "app_mesh_report.c",
    "app_mesh_report_encode.c",
    "app_node_comm.c",
}
LEGACY_TRANSPORT_CLIENTS = {
    "app_anchor.c",
    "app_anchor_survey_discovery.c",
    "app_clicker.c",
    "app_config.h",
    "app_discovery_assignment_stack.h",
    "app_gateway_ble.c",
    "app_gateway_ble.h",
    "app_gateway_eack_policy.h",
    "app_mesh_c5_priority.h",
    "app_mesh_ch9_ack.h",
    "app_mesh_collection_deferral.h",
    "app_mesh_coordinator.h",
    "app_mesh_flood.h",
    "app_mesh_local_delivery.h",
    "app_mesh_persistence.h",
    "app_mesh_report.c",
    "app_mesh_report.h",
    "app_mesh_result_handoff.h",
    "app_mesh_test.c",
    "app_state.h",
    "main.c",
}


class NodeCommSourceBoundaryTests(unittest.TestCase):
    def test_new_protocol_clients_cannot_include_mesh_report_directly(self):
        direct_clients = set()
        include_pattern = re.compile(
            r'^\s*#\s*include\s+"app_mesh_report\.h"', re.MULTILINE
        )

        for path in APP_SRC.glob("*.[ch]"):
            if include_pattern.search(path.read_text(encoding="utf-8")):
                direct_clients.add(path.name)

        allowed = LEGACY_REPORT_CLIENTS | BOUNDARY_IMPLEMENTATION
        self.assertEqual(set(), direct_clients - allowed)

    def test_new_protocol_clients_cannot_include_transport_internals(self):
        direct_clients = set()
        include_pattern = re.compile(
            r'^\s*#\s*include\s+"(?:mesh|mesh_relay|route)\.h"',
            re.MULTILINE,
        )

        for path in APP_SRC.glob("*.[ch]"):
            if include_pattern.search(path.read_text(encoding="utf-8")):
                direct_clients.add(path.name)

        service_internals = {
            path.name for path in APP_SRC.glob("app_node_comm*.[ch]")
        }
        allowed = LEGACY_TRANSPORT_CLIENTS | service_internals
        self.assertEqual(set(), direct_clients - allowed)

    def test_facade_is_exact_compatibility_forwarder(self):
        header = (APP_SRC / "app_node_comm.h").read_text(encoding="utf-8")
        source = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        expected_calls = {
            "app_mesh_report_init(callbacks)",
            "mesh_stop_role_scan()",
            "mesh_restart_role_scan()",
            "mesh_send_outbound(envelope, reason)",
            "mesh_send_c5_flood(flood_envelope, purpose, reason, sent_now)",
            "mesh_try_send_control_response_view(",
            "mesh_request_route(target_id, reason)",
            "mesh_start_tracked_tx(envelope, reason)",
            "mesh_start_owned_tracked_tx(envelope, reason, rf_sent)",
            "queue_anchor_report(envelope)",
            "mesh_delivery_health_get(health)",
        }
        for call in expected_calls:
            with self.subTest(call=call):
                self.assertIn(call, source)

        self.assertNotRegex(source, r"MSG_[A-Z0-9_]+")
        self.assertNotIn("TLV_", source)
        self.assertNotIn('#include "app_mesh_report.h"', header)

    def test_gateway_control_state_has_single_new_owner(self):
        report = (APP_SRC / "app_mesh_report.c").read_text(encoding="utf-8")
        control = (
            APP_SRC / "app_node_comm_gateway_control.c"
        ).read_text(encoding="utf-8")

        state_definition = "static struct app_mesh_command_orchestrator"
        self.assertNotIn(state_definition, report)
        self.assertEqual(1, control.count(state_definition))
        self.assertIn(
            "return app_node_comm_gateway_control_send(orchestrator, reason, sent_now);",
            report,
        )
        self.assertIn(
            "return app_node_comm_gateway_control_priority_submit(work);", report
        )

    def test_gateway_control_cannot_interpret_protocol_payloads(self):
        source = (
            APP_SRC / "app_node_comm_gateway_control.c"
        ).read_text(encoding="utf-8")
        self.assertNotRegex(source, r"MSG_[A-Z0-9_]+")
        self.assertNotIn("TLV_", source)
        self.assertNotIn("dwm3000", source.lower())

    def test_policy_core_is_hardware_independent_and_static(self):
        header = (ROOT / "include" / "node_comm.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "node_comm.c").read_text(encoding="utf-8")
        combined = header + source

        for forbidden in (
            "zephyr/",
            "dwm3000",
            "mesh_outbound",
            "proto_packet",
            "MSG_",
            "TLV_",
            "malloc(",
            "calloc(",
            "realloc(",
            "free(",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, combined)
        self.assertIn(
            "struct node_comm_request_slot slots[NODE_COMM_MAX_REQUESTS]", header
        )

    def test_protocol_request_selects_profile_without_transport_tuning(self):
        header = (ROOT / "include" / "node_comm.h").read_text(encoding="utf-8")
        request_match = re.search(
            r"struct node_comm_request\s*\{(?P<body>.*?)\};",
            header,
            re.DOTALL,
        )
        self.assertIsNotNone(request_match)
        request_body = request_match.group("body")
        self.assertIn("enum node_comm_delivery_profile profile", request_body)
        for forbidden in (
            "retry_delay",
            "max_attempts",
            "backoff",
            "priority",
            "radio_channel",
            "ack_timeout",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, request_body)

        for path in APP_SRC.glob("*.[ch]"):
            if path.name.startswith("app_node_comm"):
                continue
            source = path.read_text(encoding="utf-8")
            if re.search(
                r"\bstruct\s+node_comm_request\b|\bnode_comm_submit\s*\(",
                source,
            ) is None:
                continue
            for forbidden in ("retry_delay_ms", "max_attempts", "backoff_shift"):
                with self.subTest(path=path.name, forbidden=forbidden):
                    self.assertNotIn(forbidden, source)

    def test_adapter_initializes_single_request_scheduler_core(self):
        source = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        self.assertEqual(
            1, source.count("static struct node_comm node_comm_policy")
        )
        self.assertNotIn("static struct node_comm_lifecycle node_comm_policy", source)
        self.assertIn("node_comm_init(&node_comm_policy)", source)
        self.assertIn("node_comm_start(&node_comm_policy", source)
        self.assertEqual(1, source.count("node_comm_submit("))
        self.assertIn("app_node_comm_submit_delivery(", source)
        self.assertIn("app_node_comm_take_delivery_event_for(", source)
        self.assertGreaterEqual(source.count("app_node_comm_require_running()"), 7)

        for name in ("app_anchor.c", "app_anchor_survey_discovery.c", "app_clicker.c"):
            with self.subTest(name=name):
                protocol_source = (APP_SRC / name).read_text(encoding="utf-8")
                self.assertNotIn("node_comm_submit(", protocol_source)

    def test_gateway_single_ack_uses_bounded_communication_response(self):
        report = (APP_SRC / "app_mesh_report.c").read_text(encoding="utf-8")
        facade = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")

        queue_start = report.index(
            "if (DEVICE_ROLE == ROLE_GATEWAY &&\n"
            "            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD)"
        )
        queue_end = report.index("app_mesh_gateway_ack_decide", queue_start)
        queue_path = report[queue_start:queue_end]

        self.assertIn("app_node_comm_submit_control_response(", queue_path)
        self.assertIn("goto after_gateway_ack", queue_path)
        self.assertNotIn("mesh_store_route_waiting_tx(", queue_path)
        self.assertNotIn("mesh_propose_event_after_channel5_contact(", queue_path)
        self.assertIn("NODE_COMM_PROFILE_CONTROL_RESPONSE", facade)
        self.assertIn("mesh_try_send_control_response_view(", facade)
        self.assertIn("app_node_comm_reap_auto_terminal_events_locked()", facade)

    def test_global_radio_admission_gate_closes_legacy_role_bypasses(self):
        state_header = (APP_SRC / "app_state.h").read_text(encoding="utf-8")
        state_source = (APP_SRC / "app_state.c").read_text(encoding="utf-8")
        report_source = (APP_SRC / "app_mesh_report.c").read_text(
            encoding="utf-8"
        )

        for declaration in (
            "void radio_guard_uwb_admission_pause(void);",
            "void radio_guard_uwb_admission_resume(void);",
            "bool radio_guard_uwb_admission_paused(void);",
        ):
            self.assertIn(declaration, state_header)

        start = re.search(
            r"int radio_guard_uwb_start\([^)]*\)\s*\{(?P<body>.*?)\n\}",
            state_source,
            re.DOTALL,
        )
        self.assertIsNotNone(start)
        body = start.group("body")
        pause_check = body.index("if (uwb_rf_admission_paused)")
        owner_claim = body.index("uwb_rf_active = true")
        self.assertLess(pause_check, owner_claim)
        self.assertIn("return -ESHUTDOWN", body[pause_check:owner_claim])

        pause = re.search(
            r"void radio_guard_uwb_admission_pause\(void\)\s*\{(?P<body>.*?)\n\}",
            state_source,
            re.DOTALL,
        )
        resume = re.search(
            r"void radio_guard_uwb_admission_resume\(void\)\s*\{(?P<body>.*?)\n\}",
            state_source,
            re.DOTALL,
        )
        self.assertIsNotNone(pause)
        self.assertIsNotNone(resume)
        self.assertIn("uwb_rf_admission_paused = true", pause.group("body"))
        self.assertIn("uwb_rf_admission_paused = false", resume.group("body"))
        self.assertIn("radio_guard_uwb_admission_pause();", report_source)
        self.assertIn("radio_guard_uwb_admission_resume();", report_source)


if __name__ == "__main__":
    unittest.main()
