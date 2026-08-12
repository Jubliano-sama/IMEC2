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
# Local delivery owns transport custody identities and validates the exact
# semantic packet before handing it to a protocol consumer. Route freshness is
# RAM-owned by the relay; no storage facade is part of this boundary.
TRANSPORT_STATE_IMPLEMENTATION = {
    "app_gateway_collection_recovery.h",
    "app_mesh_local_delivery.c",
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
    def test_facade_async_handoff_has_exact_pre_rf_cancel_and_rf_observation(self):
        report = read_composed_source(APP_SRC / "app_mesh_report.c")

        tx_start = report.index(
            "static int mesh_start_tracked_tx_with_retry("
        )
        tx_end = report.index("\nint mesh_start_tracked_tx(", tx_start)
        tracked_tx = report[tx_start:tx_end]
        self.assertIn("mesh_cancel_observation_owned_pre_rf_tx(", tracked_tx)
        self.assertGreaterEqual(
            tracked_tx.count("mesh_cancel_observation_owned_pre_rf_tx("), 2
        )
        slot_full = tracked_tx.index('"channel9-slot-full"')
        send_failure = tracked_tx.index('"send-failure"', slot_full)
        self.assertIn("observation != NULL", tracked_tx[:slot_full])
        self.assertIn("!observation->rf_started", tracked_tx[:send_failure])

        result_offer = tracked_tx.index("if (send_prepared_c5_control)")
        result_offer_end = tracked_tx.index(
            "} else if (direct_gateway_tx_pending)", result_offer
        )
        result_offer_send = tracked_tx[result_offer:result_offer_end]
        self.assertIn("mesh_send_c5_control_attempt(", result_offer_send)
        self.assertIn("absolute_deadline_ms", result_offer_send)
        self.assertIn("observation", result_offer_send)

        c5_start = report.index("static int mesh_send_c5_control_attempt(")
        c5_end = report.index("\nint mesh_send_c5_control(", c5_start)
        c5_attempt = report[c5_start:c5_end]
        self.assertIn("wake_rf_started_at_ms", c5_attempt)
        self.assertIn("&wake_rf_started", c5_attempt)
        self.assertIn("&wake_rf_started_at_ms", c5_attempt)
        self.assertNotIn("payload_observation", c5_attempt)
        payload_send = c5_attempt.index(
            "mesh_send_outbound_with_release_on_channel_until("
        )
        wake_merge = c5_attempt.index(
            "observation->rf_started = true", payload_send
        )
        self.assertIn("observation", c5_attempt[payload_send:wake_merge])
        self.assertLess(payload_send, wake_merge)
        self.assertIn("observation->rf_started = true", c5_attempt)
        self.assertIn(
            "observation->rf_started_at_ms = wake_rf_started_at_ms",
            c5_attempt,
        )

    def test_exact_relay_terminal_commit_precedes_facade_backend_release(self):
        report = read_composed_source(APP_SRC / "app_mesh_report.c")

        complete_start = report.index(
            "static int mesh_complete_gateway_ack_confirm("
        )
        complete_end = report.index(
            "static int mesh_complete_terminal_release(", complete_start
        )
        complete = report[complete_start:complete_end]
        producer_cleanup = complete.index(
            "anchor_survey_delivery_gateway_confirmed"
        )
        facade_proof = complete.index(
            "app_node_comm_note_gateway_confirmed_digest_at("
        )
        relay_commit = complete.index(
            "mesh_relay_commit_gateway_ack_confirm_terminal("
        )
        self.assertLess(producer_cleanup, relay_commit)
        self.assertLess(relay_commit, facade_proof)
        self.assertNotIn("mesh_save_outbox_durable", complete)
        self.assertNotIn("mesh_deferred_outbox_pending", complete)

    def test_rx_snapshots_transient_confirm_before_core_handles_its_ack(self):
        report = read_composed_source(APP_SRC / "app_mesh_report.c")

        drain_start = report.index(
            "static uint32_t mesh_drain_rx_queue_locked("
        )
        drain_end = report.index(
            "static void mesh_rx_work_handler(", drain_start
        )
        drain = report[drain_start:drain_end]
        transient_state = drain.index(
            "mesh_runtime.pending.gateway_ack_confirm_pending"
        )
        transient_snapshot = drain.index(
            "mesh_relay_pending_gateway_ack_confirm_wire(", transient_state
        )
        immutable_fallback = drain.index(
            "confirmed_packet = mesh_runtime.pending.packet;",
            transient_snapshot,
        )
        handle_rx = drain.index(
            "mesh_relay_handle_rx_with_random(", immutable_fallback
        )
        action_handoff = drain.index(
            "mesh_handle_result_actions(", handle_rx
        )

        self.assertLess(transient_state, transient_snapshot)
        self.assertLess(transient_snapshot, immutable_fallback)
        self.assertLess(immutable_fallback, handle_rx)
        self.assertLess(handle_rx, action_handoff)
        self.assertIn(
            "confirmed_payload_len > 0u ?\n"
            "                                           confirmed_payload : NULL",
            drain[action_handoff:],
        )

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
        allowed = (
            LEGACY_TRANSPORT_CLIENTS
            | TRANSPORT_STATE_IMPLEMENTATION
            | service_internals
        )
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

        # Gateway ACK is the one facade-level semantic class: its producer
        # allocates a fresh outer reply sequence for every replay, while the
        # ACK payload commits the stable acknowledged packet.  Coalescing is
        # deliberately closed to this profile/type and exact reverse edge.
        self.assertEqual(
            ["MSG_GATEWAY_ACK", "MSG_GATEWAY_ACK"],
            re.findall(r"MSG_[A-Z0-9_]+", source),
        )
        coalescer = source[
            source.index("static bool app_node_comm_gateway_ack_response_matches(") :
            source.index(
                "static struct app_node_comm_delivery_record *\n"
                "app_node_comm_gateway_ack_response_record("
            )
        ]
        self.assertEqual(2, coalescer.count("MSG_GATEWAY_ACK"))
        self.assertEqual(2, coalescer.count("NODE_COMM_PROFILE_CONTROL_RESPONSE"))
        self.assertIn("record->packet.session_id == envelope->packet.session_id", coalescer)
        self.assertIn("memcmp(payload, envelope->payload", coalescer)
        self.assertIn("record->next_hop_id == envelope->next_hop_id", coalescer)
        self.assertNotIn("record->packet.seq == envelope->packet.seq", coalescer)
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
        self.assertEqual(2, source.count("node_comm_submit("))
        self.assertIn("app_node_comm_submit_delivery_internal(", source)
        self.assertIn("app_node_comm_commit_protocol_response(", source)
        self.assertIn("app_node_comm_submit_delivery(", source)
        self.assertIn("app_node_comm_take_delivery_event_for(", source)
        self.assertGreaterEqual(source.count("app_node_comm_require_running()"), 7)

        for name in ("app_anchor.c", "app_anchor_survey_discovery.c", "app_clicker.c"):
            with self.subTest(name=name):
                protocol_source = (APP_SRC / name).read_text(encoding="utf-8")
                self.assertNotIn("node_comm_submit(", protocol_source)

    def test_production_delivery_trace_is_installed_and_fits_formatter(self):
        source = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        board = (APP_SRC / "app_board.c").read_text(encoding="utf-8")

        # The compact record has fixed-width hex fields and a compile-time
        # rendered-width calculation, while the board formatter owns 128 bytes.
        self.assertIn("char line[128];", board)
        self.assertIn(
            "k_mutex_lock(&status_debug_rtt_mutex, K_NO_WAIT)", board
        )
        self.assertIn("DBG_NC o=%016llx g=%08x", source)
        self.assertIn("w=%02x e=%08x", source)
        self.assertIn("q=%04x>%04x", source)
        self.assertIn("t=%08x p=%08x a=%02x", source)
        self.assertIn("DBG_NC_RSVX i=%01x k=%02x", source)
        self.assertIn(
            "#define APP_NODE_COMM_DELIVERY_TRACE_MAX_CHARS", source
        )
        self.assertIn(
            "#define APP_NODE_COMM_RESERVATION_EXPIRED_TRACE_MAX_CHARS",
            source,
        )
        self.assertIn(
            "APP_NODE_COMM_DELIVERY_TRACE_MAX_CHARS == 125u", source
        )
        self.assertIn(
            "APP_NODE_COMM_RESERVATION_EXPIRED_TRACE_MAX_CHARS == 89u",
            source,
        )
        self.assertIn(
            "APP_NODE_COMM_DELIVERY_TRACE_MAX_CHARS < 128u", source
        )

        init_start = source.index("int app_node_comm_init(")
        init_end = source.index("void app_node_comm_stop_role_scan", init_start)
        init = source[init_start:init_end]
        trace_install = init.index("node_comm_set_delivery_transition_trace(")
        self.assertLess(init.index("node_comm_init(&node_comm_policy)"),
                        trace_install)
        self.assertIn(
            "app_node_comm_production_delivery_transition_trace", init
        )

        trace_start = source.index(
            "static void app_node_comm_production_delivery_transition_trace("
        )
        trace_end = source.index(
            "static bool app_node_comm_transport_quiesced", trace_start
        )
        trace = source[trace_start:trace_end]
        self.assertIn("APP_NODE_COMM_DELIVERY_TRACE_FORMAT", trace)
        self.assertIn("status_debug_printf(", trace)
        self.assertNotIn("k_work_", trace)
        self.assertNotIn("app_node_comm_sync_", trace)
        self.assertNotIn("app_node_comm_", trace[trace.index("{") + 1 :])

    def test_resource_wait_owner_is_core_generation_bound(self):
        header = (ROOT / "include" / "node_comm.h").read_text(
            encoding="utf-8"
        )
        core = (ROOT / "src" / "node_comm.c").read_text(encoding="utf-8")
        adapter = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")

        self.assertIn("struct node_comm_resource_wait_owner", header)
        self.assertIn("uint32_t delivery_generation;", header)
        self.assertIn("node_comm_resource_wait_owner_next(", header)
        self.assertIn("NODE_COMM_DELIVERY_TRACE_RESOURCE_BLOCKED", header)
        self.assertIn("NODE_COMM_DELIVERY_TRACE_RESOURCE_RESUMED", header)
        self.assertIn("node_comm_resource_wait_owner_next(", core)
        self.assertIn("owner->delivery_generation", core)
        self.assertNotIn("waiting_for_reliable_owner", adapter)
        self.assertIn("node_comm_resource_wait_owner_next(", adapter)
        self.assertIn("node_comm_release_resource_wait(", adapter)

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
        self.assertIn("app_node_comm_schedule_gateway_scan_restart", cancel)

        restart_start = source.index(
            "static void app_node_comm_schedule_gateway_scan_restart("
        )
        restart_end = source.index(
            "static void app_node_comm_delivery_due_kick_handler",
            restart_start,
        )
        restart = source[restart_start:restart_end]
        self.assertIn("&node_comm_gateway_scan_restart_work", restart)
        self.assertIn("K_NO_WAIT", restart)
        self.assertIn("app_watchdog_stop_feeding();", restart)

        handler_start = source.index(
            "static void app_node_comm_gateway_scan_restart_work_handler("
        )
        handler_end = source.index(
            "static int app_node_comm_schedule_delivery_locked", handler_start
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
        self.assertIn("#define APP_NODE_COMM_MAX_DELIVERIES 6u", header)
        self.assertIn("NODE_COMM_MAX_REQUESTS=6", cmake)

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

        poll_start = facade.index(
            "static int app_node_comm_poll_backend_release_locked("
        )
        poll_end = facade.index(
            "static void app_node_comm_guard_external_backend_attempts_locked",
            poll_start,
        )
        poll = facade[poll_start:poll_end]
        self.assertIn("mesh_take_reliable_uplink_cancel_result(", poll)
        self.assertIn("record->backend_release_request_token", poll)

        reconcile_start = facade.index(
            "static void app_node_comm_reconcile_terminal_backends_locked(void)"
        )
        reconcile_end = facade.index(
            "static size_t app_node_comm_service_policy_locked", reconcile_start
        )
        reconcile = facade[reconcile_start:reconcile_end]
        peek = reconcile.index("node_comm_peek_terminal_event_for")
        request = reconcile.index("mesh_request_reliable_uplink_cancel")
        immediate_poll = reconcile.index(
            "app_node_comm_poll_backend_release_locked(", request
        )
        self.assertLess(peek, request)
        self.assertLess(request, immediate_poll)
        self.assertIn("record->backend_attempt_outstanding", reconcile[:peek])
        self.assertIn("record->backend_release_request_outstanding", reconcile)
        self.assertIn("mesh_packet_semantic_digest(", reconcile)
        self.assertIn("app_node_comm_poll_backend_release_locked(", reconcile)
        self.assertNotIn("mesh_cancel_reliable_uplink(", facade)

        route_cancel_start = report.index(
            "static int mesh_cancel_reliable_uplink_route_owned("
        )
        complete_start = report.index(
            "static bool mesh_node_comm_cancel_complete(",
            route_cancel_start,
        )
        route_cancel_end = complete_start
        complete_end = report.index(
            "static void mesh_node_comm_cancel_work_handler",
            complete_start,
        )
        route_cancel = report[route_cancel_start:route_cancel_end]
        matcher_start = report.index(
            "static bool mesh_node_comm_packet_matches_cancel("
        )
        matcher_end = report.index(
            "int mesh_try_send_reliable_uplink_view(", matcher_start
        )
        matcher = report[matcher_start:matcher_end]
        self.assertIn("mesh_packet_semantic_digest(", matcher)
        self.assertIn("mesh_node_comm_packet_matches_cancel(", route_cancel)
        self.assertIn("mesh_relay_cancel_tx(&mesh_runtime)", route_cancel)
        self.assertIn("mesh_route_waiting_tx_valid = false", route_cancel)
        self.assertNotIn("mesh_save_outbox_durable", route_cancel)
        self.assertNotIn("mesh_deferred_outbox_pending", route_cancel)

        complete_path = report[complete_start:complete_end]
        complete_lock = complete_path.index(
            "k_spin_lock(&mesh_node_comm_cancel_lock)"
        )
        complete_match = complete_path.index(
            "mesh_node_comm_cancel_request_matches(", complete_lock
        )
        result_publish = complete_path.index(
            "mesh_node_comm_cancel_request.result = result", complete_match
        )
        completion_publish = complete_path.index(
            "mesh_node_comm_cancel_request.complete = true", result_publish
        )
        complete_unlock = complete_path.index(
            "k_spin_unlock(&mesh_node_comm_cancel_lock", completion_publish
        )
        self.assertLess(complete_lock, complete_match)
        self.assertLess(complete_match, result_publish)
        self.assertLess(result_publish, completion_publish)
        self.assertLess(completion_publish, complete_unlock)
        self.assertIn("mesh_node_comm_cancel_request.pending", complete_path)
        self.assertIn("!mesh_node_comm_cancel_request.complete", complete_path)
        self.assertIn("request->delivery_handle", complete_path)
        self.assertIn("request->request_token", complete_path)

        worker_start = complete_end
        worker_end = report.index(
            "int mesh_request_reliable_uplink_cancel(", worker_start
        )
        worker = report[worker_start:worker_end]
        worker_cancel = worker.index(
            "mesh_cancel_reliable_uplink_route_owned("
        )
        worker_complete = worker.index(
            "mesh_node_comm_cancel_complete(&request, ret)", worker_cancel
        )
        publish_gate = worker.index("if (publish)", worker_complete)
        release_callback = worker.index(
            "app_node_comm_backend_release_ready(", publish_gate
        )
        self.assertLess(
            worker_cancel,
            worker_complete,
        )
        self.assertLess(worker_complete, publish_gate)
        self.assertLess(publish_gate, release_callback)

        request_start = worker_end
        request_end = report.index(
            "int mesh_take_reliable_uplink_cancel_result(", request_start
        )
        request_path = report[request_start:request_end]
        publish_request = request_path.index(
            "mesh_node_comm_cancel_request ="
        )
        snapshot_request = request_path.index(
            "request = mesh_node_comm_cancel_request", publish_request
        )
        publish_unlock = request_path.index(
            "k_spin_unlock(&mesh_node_comm_cancel_lock", snapshot_request
        )
        dedicated_gate = request_path.index(
            "#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)",
            publish_unlock,
        )
        same_owner = request_path.index(
            "k_current_get() == k_work_queue_thread_get(&mesh_route_work_q)",
            dedicated_gate,
        )
        inline_cancel = request_path.index(
            "mesh_cancel_reliable_uplink_route_owned(&request)", same_owner
        )
        inline_complete = request_path.index(
            "mesh_node_comm_cancel_complete(&request, ret)", inline_cancel
        )
        async_schedule = request_path.index(
            "mesh_route_owner_work_reschedule(&mesh_node_comm_cancel_work",
            inline_complete,
        )
        self.assertLess(publish_request, snapshot_request)
        self.assertLess(snapshot_request, publish_unlock)
        self.assertLess(publish_unlock, dedicated_gate)
        self.assertLess(dedicated_gate, same_owner)
        self.assertLess(same_owner, inline_cancel)
        self.assertLess(inline_cancel, inline_complete)
        self.assertLess(inline_complete, async_schedule)
        self.assertIn("? 0 : -ESTALE", request_path[inline_complete:async_schedule])
        self.assertNotIn(
            "app_node_comm_backend_release_ready(",
            request_path[same_owner:async_schedule],
            "the inline route-owner path runs under the facade lock, so it "
            "must publish only the polled result and never recurse by callback",
        )

        schedule_failure = request_path.index("if (ret >= 0)", async_schedule)
        cleanup_match = request_path.index(
            "mesh_node_comm_cancel_request_matches(", schedule_failure
        )
        cleanup_clear = request_path.index(
            "memset(&mesh_node_comm_cancel_request", cleanup_match
        )
        self.assertLess(async_schedule, schedule_failure)
        self.assertLess(schedule_failure, cleanup_match)
        self.assertLess(cleanup_match, cleanup_clear)

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

        retransmit_action = report.rindex(
            "bool direct_gateway_retransmit", 0, retransmit
        )
        direct_select = report.index(
            "direct_gateway_retransmit =", retransmit_action
        )
        scheduled_select = report.index(
            "mesh_relay_require_channel9_tx_event", direct_select
        )
        direct_send = report.index(
            "mesh_send_direct_gateway_payload_and_wait_ack", retransmit
        )
        self.assertLess(direct_select, scheduled_select)
        self.assertLess(direct_select, retransmit)
        self.assertLess(retransmit, direct_send)
        self.assertLess(direct_send, send)
        self.assertIn(
            "debug_next_hop == GATEWAY_ID",
            report[direct_select:scheduled_select],
        )
        self.assertIn(
            '"retransmit-direct-gateway"',
            report[direct_send:send],
        )

    def test_gateway_due_kick_keeps_rf_worker_on_mesh_route_queue(self):
        facade = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        report = read_composed_source(APP_SRC / "app_mesh_report.c")

        schedule_start = facade.index(
            "static int app_node_comm_schedule_delivery_locked"
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
        self.assertIn("return k_work_reschedule(", schedule)
        self.assertIn("return mesh_route_work_reschedule(", schedule)
        self.assertIn("app_watchdog_stop_feeding();", schedule)
        kick_start = facade.index(
            "static void app_node_comm_delivery_due_kick_handler"
        )
        kick_end = facade.index(
            "int app_node_comm_gateway_delivery_safe_boundary", kick_start
        )
        kick = facade[kick_start:kick_end]
        self.assertIn("mesh_node_comm_gateway_delivery_due_begin", kick)
        self.assertIn("mesh_route_work_reschedule(&node_comm_delivery_work", kick)
        self.assertIn("app_node_comm_retain_gateway_due_retry_locked", kick)
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

    def test_caller_terminal_watchdog_owns_an_idle_deadline(self):
        facade = (APP_SRC / "app_node_comm.c").read_text(encoding="utf-8")
        start = facade.index(
            "static bool app_node_comm_next_required_service_due_locked("
        )
        end = facade.index(
            "static struct app_node_comm_delivery_record *", start
        )
        due = facade[start:end]

        for required in (
            "record->auto_reap_terminal",
            "record->backend_attempt_outstanding",
            "node_comm_delivery_backend_active_handle == record->handle",
            "app_node_comm_terminal_backend_released(record)",
            "node_comm_peek_terminal_event_for(",
            "APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS",
            "terminal.terminal_at_ms",
            "have_record_due = true",
        ):
            self.assertIn(required, due)

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
        self.assertIn("IMEC_MESH_ROUTE_TEST_BUILD ON", clicker_preset)
        self.assertNotIn(
            "IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD ON", clicker_preset
        )
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
            "mesh_send_causal_channel9_response(", guard
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
        probe_retry_owned = ack_path.index(
            "rx->packet.msg_type == MSG_GATEWAY_ROUTE_REQ", success_exit
        )
        probe_exit = ack_path.index(
            "goto after_gateway_ack;", probe_retry_owned
        )
        self.assertLess(success_exit, probe_retry_owned)
        self.assertLess(probe_retry_owned, probe_exit)
        self.assertLess(probe_exit, fallback_admission)
        self.assertNotIn("mesh_store_route_waiting_tx(", ack_path)
        self.assertNotIn("mesh_propose_event_after_channel5_contact(", ack_path)
        self.assertIn("NODE_COMM_PROFILE_CONTROL_RESPONSE", facade)
        self.assertIn("mesh_try_send_control_response_view(", facade)
        self.assertIn("app_node_comm_reap_auto_terminal_events_locked()", facade)

    def test_global_radio_admission_gate_closes_legacy_role_bypasses(self):
        guard_header = (APP_SRC / "app_radio_guard.h").read_text(
            encoding="utf-8"
        )
        guard_source = (APP_SRC / "app_radio_guard.c").read_text(
            encoding="utf-8"
        )
        report_source = read_composed_source(APP_SRC / "app_mesh_report.c")

        for declaration in (
            "void radio_guard_uwb_admission_pause(void);",
            "void radio_guard_uwb_admission_resume(void);",
            "bool radio_guard_uwb_admission_paused(void);",
        ):
            self.assertIn(declaration, guard_header)

        claim = re.search(
            r"int radio_guard_uwb_claim\([^)]*\)\s*\{(?P<body>.*?)\n\}",
            guard_source,
            re.DOTALL,
        )
        self.assertIsNotNone(claim)
        body = claim.group("body")
        pause_check = body.index("if (uwb_rf_admission_paused)")
        poison_check = body.index("phase == RADIO_GUARD_UWB_POISONED")
        owner_claim = body.index("uwb_rf_owner =")
        self.assertLess(pause_check, owner_claim)
        self.assertLess(poison_check, owner_claim)
        self.assertIn("ret = -ESHUTDOWN", body[pause_check:owner_claim])
        self.assertIn(
            "ret = uwb_rf_poison_error < 0 ? uwb_rf_poison_error : -EIO",
            body[poison_check:owner_claim],
        )

        legacy_start = re.search(
            r"int radio_guard_uwb_start\([^)]*\)\s*\{(?P<body>.*?)\n\}",
            guard_source,
            re.DOTALL,
        )
        self.assertIsNotNone(legacy_start)
        self.assertIn(
            "radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_LEGACY",
            legacy_start.group("body"),
        )

        pause = re.search(
            r"void radio_guard_uwb_admission_pause\(void\)\s*\{(?P<body>.*?)\n\}",
            guard_source,
            re.DOTALL,
        )
        resume = re.search(
            r"void radio_guard_uwb_admission_resume\(void\)\s*\{(?P<body>.*?)\n\}",
            guard_source,
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
