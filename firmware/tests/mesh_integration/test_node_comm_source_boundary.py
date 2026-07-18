#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


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
            "mesh_schedule_route_request(target_id, reason)",
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
        report = read_composed_source(APP_SRC / "app_mesh_report.c")
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

    def test_anchor_command_tracking_reuses_the_communication_context(self):
        anchor = read_composed_source(APP_SRC / "app_anchor.c")

        self.assertNotIn("anchor_command_orchestrator", anchor)
        self.assertIn(
            "app_mesh_command_orchestrator_anchor_receive(\n"
            "        mesh_gateway_command_orchestrator_context(),",
            anchor,
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

    def test_large_control_payload_has_one_gateway_only_exact_owner(self):
        header = (APP_SRC / "app_node_comm.h").read_text(encoding="utf-8")
        source = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        native_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        app_cmake = (ROOT / "app" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "#define APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN 192u", header
        )
        self.assertIn(
            "#define APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN "
            "PACKET_EXT_MAX_PAYLOAD_LEN",
            header,
        )
        self.assertEqual(
            1,
            source.count(
                "static uint8_t node_comm_large_control_payload["
            ),
        )
        large_owner = source.index(
            "static uint8_t node_comm_large_control_payload["
        )
        self.assertIn(
            "#if APP_NODE_COMM_GATEWAY_ROLE",
            source[max(0, large_owner - 80) : large_owner],
        )
        self.assertIn(
            "#ifndef APP_NODE_COMM_GATEWAY_ROLE\n"
            "#define APP_NODE_COMM_GATEWAY_ROLE 0",
            source,
        )
        self.assertIn(
            "DEVICE_ROLE=ROLE_GATEWAY\n"
            "    APP_NODE_COMM_GATEWAY_ROLE=1",
            native_cmake,
        )
        self.assertIn(
            "DEVICE_ROLE=3\n"
            "        APP_NODE_COMM_GATEWAY_ROLE=1",
            app_cmake,
        )

        size_start = source.index(
            "static bool app_node_comm_payload_size_supported("
        )
        size_end = source.index(
            "static bool app_node_comm_frozen_delivery_matches(", size_start
        )
        size_policy = source[size_start:size_end]
        self.assertIn(
            "payload_len <= APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN",
            size_policy,
        )
        self.assertIn(
            "profile == NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD",
            size_policy,
        )
        self.assertIn("#if APP_NODE_COMM_GATEWAY_ROLE", size_policy)
        self.assertNotIn("NODE_COMM_PROFILE_RELIABLE_UPLINK", size_policy)

        freeze_start = source.index("static int app_node_comm_freeze_delivery(")
        freeze_end = source.index(
            "static bool app_node_comm_backend_error_retryable", freeze_start
        )
        freeze = source[freeze_start:freeze_end]
        self.assertIn(
            "node_comm_large_control_payload_handle != 0u", freeze
        )
        self.assertIn("return -ENOSPC", freeze)
        self.assertIn(
            "memcpy(node_comm_large_control_payload,", freeze
        )
        self.assertIn(
            "node_comm_large_control_payload_handle = handle", freeze
        )
        self.assertIn("record->uses_large_control_payload = true", freeze)

        attempt_start = source.index("int app_node_comm_service_deliveries(void)")
        attempt_end = source.index(
            "int app_node_comm_cancel_delivery", attempt_start
        )
        attempt = source[attempt_start:attempt_end]
        self.assertIn(
            "attempt_payload = app_node_comm_frozen_payload(&attempt_record)",
            attempt,
        )
        self.assertIn(".payload = attempt_payload", attempt)

        clear_start = source.index(
            "static void app_node_comm_clear_delivery_record("
        )
        clear_end = source.index(
            "static void app_node_comm_reconcile_terminal_backends_locked",
            clear_start,
        )
        clear = source[clear_start:clear_end]
        self.assertIn(
            "node_comm_large_control_payload_handle == handle", clear
        )
        self.assertIn(
            "node_comm_large_control_payload_handle = 0u", clear
        )

        init_start = source.index("int app_node_comm_init(")
        init_end = source.index("void app_node_comm_stop_role_scan", init_start)
        init = source[init_start:init_end]
        self.assertIn("node_comm_large_control_payload_handle = 0u", init)

    def test_gateway_cancel_defers_scan_restart_to_fresh_work(self):
        source = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        cancel_start = source.index("int app_node_comm_cancel_delivery(")
        cancel_end = source.index(
            "int app_node_comm_abandon_delivery(", cancel_start
        )
        cancel = source[cancel_start:cancel_end]

        self.assertNotIn("mesh_restart_role_scan()", cancel)
        self.assertIn("&node_comm_gateway_scan_restart_work", cancel)
        self.assertIn("K_NO_WAIT", cancel)

        handler_start = source.index(
            "static void app_node_comm_gateway_scan_restart_work_handler("
        )
        handler_end = source.index(
            "static void app_node_comm_schedule_delivery_locked", handler_start
        )
        handler = source[handler_start:handler_end]
        self.assertIn("node_comm_state(&node_comm_policy) == NODE_COMM_RUNNING", handler)
        self.assertIn("mesh_restart_role_scan()", handler)

    def test_synthetic_transmitter_uses_terminal_communication_custody(self):
        source = (APP_SRC / "app_mesh_test.c").read_text(encoding="utf-8")
        header = (APP_SRC / "app_node_comm.h").read_text(encoding="utf-8")
        report = read_composed_source(APP_SRC / "app_mesh_report.c")
        cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn('#include "app_node_comm.h"', source)
        self.assertIn("app_node_comm_submit_reliable_uplink(", source)
        self.assertIn("app_node_comm_take_delivery_event_for(", source)
        self.assertIn("NODE_COMM_TERMINAL_DELIVERED", source)
        self.assertIn("mesh_test_pending_admission_valid", source)
        self.assertNotIn("queue_anchor_report(&outbound)", source)
        self.assertNotIn("report_tx_queue_used()", source)
        self.assertNotIn("mesh_report_ch9_ack_wait_active()", source)

        self.assertIn(
            "APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN PACKET_EXT_MAX_PAYLOAD_LEN",
            header,
        )
        self.assertIn("#define APP_NODE_COMM_MAX_DELIVERIES 5u", header)
        self.assertIn("NODE_COMM_MAX_REQUESTS=5", cmake)

        queue_definition = report[
            report.index("K_THREAD_STACK_DEFINE(mesh_route_work_q_stack") - 60 :
            report.index("K_THREAD_STACK_DEFINE(mesh_route_work_q_stack") + 120
        ]
        self.assertIn(
            "#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)",
            queue_definition,
        )
        self.assertNotIn(
            "CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER", queue_definition
        )

    def test_facade_terminal_releases_backend_without_owning_retry_policy(self):
        facade = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        report = read_composed_source(APP_SRC / "app_mesh_report.c")

        reconcile_start = facade.index(
            "static void app_node_comm_reconcile_terminal_backends_locked(void)"
        )
        reconcile_end = facade.index(
            "static size_t app_node_comm_service_policy_locked", reconcile_start
        )
        reconcile = facade[reconcile_start:reconcile_end]
        peek = reconcile.index("node_comm_peek_terminal_event_for")
        cancel = reconcile.index("mesh_cancel_reliable_uplink")
        self.assertLess(peek, cancel)
        self.assertIn("record->backend_attempt_outstanding", reconcile[:peek])
        self.assertIn("record->backend_released = true", reconcile[cancel:])

        account_start = facade.index("int app_node_comm_complete_backend_attempt")
        account_end = facade.index(
            "int app_node_comm_note_backend_rf_started", account_start
        )
        account = facade[account_start:account_end]
        self.assertIn("record->backend_attempt_outstanding = false", account)
        self.assertIn("rf_started ?", account)
        self.assertIn("node_comm_note_backend_rf_started", account)
        self.assertIn("node_comm_peek_terminal_event_for", account)
        self.assertIn("-ETIMEDOUT : -ECANCELED", account)
        self.assertIn("app_node_comm_service_policy_locked", account)
        self.assertIn("app_node_comm_reap_auto_terminal_events_locked", account)

        retransmit = report.index("app_node_comm_backend_retry_preflight")
        send = report.index("mesh_send_outbound_with_release", retransmit)
        complete = report.index("app_node_comm_complete_backend_attempt", send)
        self.assertLess(retransmit, send)
        self.assertLess(send, complete)
        self.assertIn("backend_rf_started", report[send:complete + 160])
        self.assertNotIn("if (backend_rf_started)", report[send:complete])
        self.assertNotIn("max_attempt", report[retransmit:complete])

    def test_gateway_due_kick_keeps_rf_worker_on_mesh_route_queue(self):
        facade = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        report = read_composed_source(APP_SRC / "app_mesh_report.c")

        schedule_start = facade.index(
            "static void app_node_comm_schedule_delivery_locked"
        )
        schedule_end = facade.index(
            "static void app_node_comm_begin_recovery", schedule_start
        )
        schedule = facade[schedule_start:schedule_end]
        if "mesh_route_work_reschedule(&node_comm_delivery_work" not in schedule:
            self.fail(
                "node communication delivery work is not assigned to the "
                "dedicated mesh route queue"
            )

        self.assertIn("node_comm_delivery_due_kick_work", schedule)
        kick_start = facade.index(
            "static void app_node_comm_delivery_due_kick_handler"
        )
        kick_end = facade.index(
            "int app_node_comm_gateway_delivery_safe_boundary", kick_start
        )
        kick = facade[kick_start:kick_end]
        self.assertIn("mesh_node_comm_gateway_delivery_due_begin", kick)
        self.assertIn("mesh_route_work_reschedule(&node_comm_delivery_work", kick)
        for forbidden in (
            "mesh_try_send_",
            "dwm3000_driver_configure",
            "dwm3000_driver_receive",
            "dwm3000_driver_transmit",
        ):
            self.assertNotIn(forbidden, kick)

        default_queue_call = re.search(
            r"\bk_work_(?:reschedule|schedule|submit)\s*\(\s*"
            r"&node_comm_delivery_work\b",
            facade,
        )
        if default_queue_call is not None:
            self.fail(
                "node communication delivery work can fall back to the "
                "default system workqueue"
            )

        helper_start = report.index("static int mesh_reschedule_delayable")
        helper_end = report.index(
            "int mesh_route_work_reschedule", helper_start
        )
        helper = report[helper_start:helper_end]
        if (
            "k_work_reschedule_for_queue(&mesh_route_work_q, work" not in helper
        ):
            self.fail(
                "mesh route rescheduler no longer targets its dedicated "
                "workqueue"
            )

    def test_mesh_clicker_uses_dedicated_communication_queue(self):
        report = read_composed_source(APP_SRC / "app_mesh_report.c")
        cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
        clicker_conf = (ROOT / "app" / "conf" / "mesh-clicker.conf").read_text(
            encoding="utf-8"
        )

        clicker_start = cmake.index(
            'elseif(IMEC_BUILD_PRESET STREQUAL "mesh_clicker")'
        )
        clicker_end = cmake.index(
            'elseif(IMEC_BUILD_PRESET MATCHES "^ml_anchor_', clicker_start
        )
        clicker_preset = cmake[clicker_start:clicker_end]
        self.assertNotIn("IMEC_MESH_ROUTE_TEST_BUILD ON", clicker_preset)
        self.assertIn("CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE=y", clicker_conf)

        helper_start = report.index("static int mesh_reschedule_delayable")
        helper_end = report.index(
            "int mesh_route_work_reschedule", helper_start
        )
        helper = report[helper_start:helper_end]
        self.assertIn("CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE", helper)
        self.assertIn(
            "k_work_reschedule_for_queue(&mesh_route_work_q, work", helper
        )
        self.assertIn("return k_work_reschedule(work", helper)

    def test_facade_has_no_synchronous_delivery_or_route_start_api(self):
        header = (APP_SRC / "app_node_comm.h").read_text(encoding="utf-8")
        source = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")

        for forbidden in (
            "app_node_comm_start_delivery(",
            "app_node_comm_start_owned_delivery(",
            "mesh_request_route(target_id, reason)",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_gateway_single_ack_sends_current_channel9_before_bounded_fallback(self):
        report = read_composed_source(APP_SRC / "app_mesh_report.c")
        facade = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")

        ack_path_start = report.index(
            "if (DEVICE_ROLE == ROLE_GATEWAY &&\n"
            "            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD)"
        )
        ack_path_end = report.index(
            "\n        app_mesh_gateway_ack_decide(&ack_state, &ack_decision);",
            ack_path_start,
        )
        ack_path = report[ack_path_start:ack_path_end]

        immediate_action = ack_path.index(
            "APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9"
        )
        guard = ack_path.index(
            "k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS)", immediate_action
        )
        immediate_send = ack_path.index(
            "mesh_send_outbound_keep_channel9_awake(", guard
        )
        send_success = ack_path.index("if (ret == 0)", immediate_send)
        success_exit = ack_path.index("goto after_gateway_ack", send_success)
        fallback_admission = ack_path.index(
            "app_node_comm_submit_control_response(", success_exit
        )

        self.assertLess(immediate_action, guard)
        self.assertLess(guard, immediate_send)
        self.assertLess(immediate_send, send_success)
        self.assertLess(send_success, success_exit)
        self.assertLess(success_exit, fallback_admission)
        self.assertNotIn("mesh_store_route_waiting_tx(", ack_path)
        self.assertNotIn("mesh_propose_event_after_channel5_contact(", ack_path)
        self.assertIn("NODE_COMM_PROFILE_CONTROL_RESPONSE", facade)
        self.assertIn("mesh_try_send_control_response_view(", facade)
        self.assertIn("app_node_comm_reap_auto_terminal_events_locked()", facade)

    def test_global_radio_admission_gate_closes_legacy_role_bypasses(self):
        state_header = (APP_SRC / "app_state.h").read_text(encoding="utf-8")
        state_source = (APP_SRC / "app_state.c").read_text(encoding="utf-8")
        report_source = read_composed_source(APP_SRC / "app_mesh_report.c")

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
