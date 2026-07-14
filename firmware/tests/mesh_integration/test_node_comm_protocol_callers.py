#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"


def function_bodies(source: str, name: str) -> list[str]:
    bodies = []
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    bodies.append(source[brace : index + 1])
                    break
    return bodies


def function_body(source: str, name: str) -> str:
    bodies = function_bodies(source, name)
    if bodies:
        return bodies[0]
    raise AssertionError(f"function definition not found: {name}")


class NodeCommProtocolCallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.anchor = (APP_SRC / "app_anchor.c").read_text(encoding="utf-8")
        cls.survey = (APP_SRC / "app_anchor_survey_discovery.c").read_text(
            encoding="utf-8"
        )
        cls.survey_runtime = (
            APP_SRC / "app_anchor_survey_runtime.c"
        ).read_text(encoding="utf-8")
        cls.command_completion = (
            APP_SRC / "app_anchor_command_completion.c"
        ).read_text(encoding="utf-8")
        cls.gateway_ble = (APP_SRC / "app_gateway_ble.c").read_text(
            encoding="utf-8"
        )

    def test_survey_report_uses_owned_delivery_facade(self):
        body = function_body(self.survey, "app_anchor_survey_discovery_retry_report")

        self.assertIn('#include "app_node_comm.h"', self.survey)
        self.assertIn("app_node_comm_start_owned_delivery(", body)
        self.assertEqual(body.count("app_node_comm_start_owned_delivery("), 1)
        self.assertNotIn("mesh_start_owned_tracked_tx(", body)
        # The durable journal remains the sole attempt owner around the facade call.
        self.assertIn("app_mesh_local_delivery_begin_attempt(", body)
        self.assertIn("app_mesh_local_delivery_note_attempt_sent(", body)
        self.assertIn("app_mesh_local_delivery_note_attempt_blocked(", body)
        self.assertLess(
            body.index("app_mesh_local_delivery_begin_attempt("),
            body.index("app_node_comm_start_owned_delivery("),
        )
        self.assertLess(
            body.index("app_node_comm_start_owned_delivery("),
            body.index("app_mesh_local_delivery_note_attempt_sent("),
        )

    def test_survey_report_ack_remains_packet_exact(self):
        body = function_body(
            self.survey, "app_anchor_survey_delivery_gateway_confirmed"
        )

        ack_commit = body.index("app_mesh_local_delivery_note_ack(delivery, packet)")
        sample = body.index("app_stack_workload_diag_anchor_survey_sample(packet")
        terminal = body.index("app_stack_workload_diag_anchor_survey_release(packet")

        self.assertLess(ack_commit, sample)
        self.assertLess(sample, terminal)

    def test_assignment_result_sends_use_delivery_facade(self):
        submit = function_body(self.anchor, "anchor_submit_command_result")
        wrapper = function_body(self.anchor, "anchor_send_command_result")
        discovery = function_body(self.anchor, "anchor_send_discovery_response")

        self.assertIn("app_node_comm_submit_protocol_response(", submit)
        self.assertNotIn("mesh_start_tracked_tx(", submit)
        self.assertIn("anchor_submit_command_result(", wrapper)
        self.assertIn("app_node_comm_submit_protocol_response(", discovery)
        self.assertNotIn("mesh_start_tracked_tx(", discovery)

        schedule = function_body(
            self.anchor, "anchor_schedule_discovery_response"
        )
        self.assertIn("app_node_comm_abandon_delivery(", schedule)
        self.assertIn(
            "if (anchor_discovery_claim_pending.delivery_handle != 0u)",
            schedule,
        )

    def test_gateway_protocol_floods_use_control_facade(self):
        for name in (
            "gateway_route_survey_reachability",
            "gateway_send_discovery_assignment_claim_request",
            "gateway_discovery_assignment_publish_table",
        ):
            with self.subTest(name=name):
                body = function_body(self.anchor, name)
                self.assertIn("app_node_comm_send_control_flood(", body)
                self.assertNotIn("mesh_send_c5_flood(", body)

    def test_manual_and_automatic_survey_pair_control_share_reliable_lane(self):
        preparer = function_body(self.anchor, "gateway_survey_prepare_pair_control")
        sender = function_body(self.anchor, "gateway_survey_send_pair_control")
        automatic = function_body(self.anchor, "gateway_survey_auto_send_outbound")
        manual = function_body(self.anchor, "gateway_route_survey_pair_control")
        router = function_body(self.anchor, "gateway_route_survey_command")
        classifier = function_body(self.anchor, "gateway_command_uses_survey_mesh")

        self.assertIn("survey_gateway_reverse_hint_for_target(", preparer)
        self.assertIn("mesh_relay_note_gateway_survey_reverse_route(", preparer)
        self.assertIn("outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT", preparer)
        self.assertIn("gateway_survey_prepare_pair_control(", sender)
        self.assertIn("app_node_comm_submit_delivery(", sender)
        self.assertIn("NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD", sender)
        self.assertIn("absolute_deadline_ms", sender)
        self.assertIn("client_token", sender)
        self.assertNotIn("mesh_send_c5_flood(", sender)
        self.assertNotIn("app_node_comm_send_control_flood(", sender)
        self.assertNotIn("mesh_start_tracked_tx(", sender)

        self.assertEqual(automatic.count("gateway_survey_send_pair_control("), 1)
        self.assertNotIn("app_node_comm_submit_delivery(", automatic)
        self.assertEqual(manual.count("gateway_survey_send_pair_control("), 1)
        self.assertIn("app_node_comm_auto_reap_delivery(", manual)
        self.assertNotIn("survey_gateway_transaction_begin(", manual)
        self.assertNotIn("gateway_survey_transaction", manual)
        self.assertNotIn("app_node_comm_submit_delivery(", manual)
        self.assertNotIn("mesh_send_c5_flood(", automatic)
        self.assertNotIn("mesh_start_tracked_tx(", manual)

        for command in ("CMD_SURVEY_PREPARE_PAIR", "CMD_SURVEY_START_PAIR"):
            with self.subTest(command=command):
                self.assertIn(command, manual)
                self.assertIn(command, router)
                self.assertIn(command, classifier)

    def test_manual_pair_results_do_not_require_automatic_transaction(self):
        ingress = next(
            body
            for body in function_bodies(
                self.gateway_ble, "gateway_note_command_result"
            )
            if "gateway_command_result_admit(" in body
        )
        owner = function_body(
            self.anchor, "gateway_survey_auto_owns_pending_command"
        )
        result = function_body(
            self.anchor, "gateway_survey_auto_note_command_result"
        )
        timeout = function_body(
            self.anchor, "gateway_survey_auto_note_command_timeout"
        )

        self.assertIn("gateway_command_pending_matches_result(", ingress)
        self.assertIn("gateway_command_result_admit(", ingress)
        self.assertIn("if (auto_transaction_owned || !pending_matches)", ingress)
        self.assertNotIn("app_mesh_gateway_command_flow_result_matches(", ingress)
        self.assertIn("gateway_survey_pending_command_valid", owner)
        self.assertIn("survey_gateway_auto_command_matches(", owner)
        self.assertIn("gateway_survey_auto_owns_pending_command(", result)
        self.assertIn("gateway_survey_auto_owns_pending_command(", timeout)

    def test_targeted_survey_abort_is_pair_scoped(self):
        executor = function_body(self.anchor, "anchor_execute_command_side_effects")
        targeted = function_body(
            self.survey_runtime,
            "app_anchor_survey_runtime_abort_pair_matching",
        )

        self.assertIn("survey_extract_pair_tlvs(", executor)
        self.assertIn("app_anchor_survey_runtime_abort_pair_matching(", executor)
        self.assertIn("payload_len == 4u", executor)
        self.assertIn("survey_pair_lease_abort_matching(", targeted)

    def test_survey_result_releases_exact_delivery_before_phase_advance(self):
        close = function_body(
            self.anchor, "gateway_survey_cancel_take_active_delivery"
        )
        accepted = function_body(
            self.anchor, "gateway_survey_complete_accepted_delivery"
        )
        result = function_body(
            self.anchor, "gateway_survey_auto_note_command_result"
        )

        self.assertIn("app_node_comm_cancel_delivery(", close)
        self.assertIn("app_node_comm_take_delivery_event_for(", close)
        self.assertIn("survey_gateway_transaction_note_delivery_terminal(", close)
        self.assertIn("gateway_survey_cancel_take_active_delivery(", accepted)
        self.assertLess(
            result.index("gateway_survey_complete_accepted_delivery("),
            result.index("survey_gateway_transaction_phase_complete("),
        )

    def test_survey_abort_consumes_handle_and_keeps_cleanup_worker_alive(self):
        abandon = function_body(self.anchor, "gateway_survey_abandon_current")
        finish = function_body(self.anchor, "gateway_survey_auto_finish_status")
        admission = function_body(self.anchor,
                                  "gateway_route_survey_reachability")

        self.assertIn("gateway_survey_cancel_take_active_delivery(", abandon)
        self.assertIn("gateway_survey_cancel_take_active_delivery(", finish)
        self.assertIn("active.state != NODE_TRANSACTION_EMPTY", abandon)
        self.assertIn("active.state != NODE_TRANSACTION_EMPTY", finish)
        self.assertIn("!gateway_survey_transaction.active.request_delivery_terminal",
                      abandon)
        self.assertIn("!gateway_survey_transaction.active.request_delivery_terminal",
                      finish)
        self.assertIn("if (gateway_survey_cleanup_pending())", finish)
        self.assertIn("k_work_reschedule(", finish)
        self.assertIn("else {", finish)
        self.assertIn("k_work_cancel_delayable(&gateway_survey_work)", finish)
        self.assertIn("gateway_survey_cleanup_pending()", admission)
        self.assertLess(admission.index("gateway_survey_cleanup_pending()"),
                        admission.index("survey_gateway_transaction_init("))

    def test_final_survey_pair_terminal_is_not_blocked_by_cleanup(self):
        worker = function_body(self.anchor, "gateway_survey_work_handler")
        finalize = function_body(
            self.anchor, "gateway_survey_finalize_pair_observation"
        )

        self.assertIn("gateway_survey_cleanup_pending()", worker)
        self.assertIn("gateway_survey_pair_observation_active", worker)
        self.assertIn("survey_gateway_auto_no_unstarted_pairs(", worker)
        self.assertLess(
            worker.index("survey_gateway_auto_no_unstarted_pairs("),
            worker.index("gateway_survey_flush_boundary_event()"),
        )
        self.assertLess(
            worker.index("gateway_survey_flush_boundary_event()"),
            worker.index("survey_gateway_auto_next_action("),
        )
        self.assertLess(
            finalize.index("gateway_survey_begin_cleanup()"),
            finalize.index("survey_gateway_auto_no_unstarted_pairs("),
        )
        self.assertLess(
            finalize.index("survey_gateway_auto_no_unstarted_pairs("),
            finalize.index("GATEWAY_SURVEY_PAIR_FINALIZE_TERMINAL"),
        )
        self.assertLess(
            worker.index("gateway_survey_finalize_pair_observation()"),
            worker.index("gateway_survey_auto_finish()"),
        )

    def test_survey_finish_graph_is_acyclic_and_terminal_is_exactly_once(self):
        names = (
            "gateway_survey_finalize_pair_observation",
            "gateway_survey_auto_finish_status",
            "gateway_survey_auto_finish",
            "gateway_survey_work_handler",
        )
        bodies = {
            name: function_body(self.anchor, name)
            for name in names
        }
        graph = {
            name: {
                target for target in names
                if re.search(rf"\b{re.escape(target)}\s*\(", body)
            }
            for name, body in bodies.items()
        }
        remaining = set(names)
        while remaining:
            sinks = {
                name for name in remaining
                if not (graph[name] & remaining)
            }
            self.assertTrue(sinks, f"recursive survey finish graph: {graph}")
            remaining -= sinks

        finalize = bodies["gateway_survey_finalize_pair_observation"]
        finish = bodies["gateway_survey_auto_finish_status"]
        automatic = bodies["gateway_survey_auto_finish"]
        self.assertNotIn("gateway_survey_auto_finish(", finalize)
        self.assertNotIn("gateway_survey_auto_finish_status(", finalize)
        self.assertEqual(1, automatic.count("gateway_survey_auto_finish_status("))
        self.assertEqual(1, finish.count("gateway_observe_survey_terminal("))
        self.assertLess(
            finish.index("if (!gateway_survey_active)"),
            finish.index("gateway_observe_survey_terminal("),
        )
        self.assertLess(
            finish.index("gateway_observe_survey_terminal("),
            finish.index("gateway_survey_active = false"),
        )
        self.assertIn(
            "GATEWAY_SURVEY_PAIR_FINALIZE_TERMINAL",
            bodies["gateway_survey_work_handler"],
        )
        self.assertNotIn("gateway_command_survey_terminal_outcome(", finish)
        self.assertIn("gateway_command_survey_terminal_outcome(", automatic)
        self.assertLess(
            finalize.index("gateway_survey_observe_with_custody("),
            finalize.index("gateway_survey_pair_observation_active = false"),
        )

    def test_explicit_survey_deadline_reason_survives_final_pair_flush(self):
        worker = function_body(self.anchor, "gateway_survey_work_handler")
        finish = function_body(
            self.anchor, "gateway_survey_auto_finish_status"
        )
        deadline = re.search(
            r"gateway_survey_budget_explicit\s*&&.*?"
            r"gateway_survey_auto_finish_status\s*\(\s*COMMAND_TIMEOUT\s*,\s*"
            r"GATEWAY_COMMAND_EVENT_REASON_TIMEOUT\s*\)",
            worker,
            re.DOTALL,
        )

        self.assertIsNotNone(deadline)
        self.assertLess(
            worker.index("gateway_survey_operation_deadline_ms"),
            worker.index("gateway_survey_finalize_pair_observation()"),
        )
        self.assertLess(
            finish.index("gateway_survey_finalize_pair_observation()"),
            finish.index("gateway_observe_survey_terminal(status, reason)"),
        )
        self.assertNotIn("gateway_command_survey_terminal_outcome(", finish)

    def test_survey_cleanup_completion_publishes_next_pair_wake(self):
        scheduler = function_body(self.anchor,
                                  "gateway_survey_schedule_drive")
        worker = function_body(self.anchor, "gateway_survey_work_handler")
        state = function_body(self.anchor, "gateway_survey_drive_state")

        self.assertIn("survey_gateway_drive_action(", scheduler)
        self.assertIn("gateway_survey_drive_state()", scheduler)
        self.assertIn("SURVEY_GATEWAY_DRIVE_POLL_CLEANUP", scheduler)
        self.assertIn("GATEWAY_SURVEY_TRANSACTION_POLL_MS", scheduler)
        self.assertIn("SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY", scheduler)
        self.assertIn("GATEWAY_BLE_TX_RETRY_MS", scheduler)
        self.assertIn("SURVEY_GATEWAY_DRIVE_RUN_NOW", scheduler)
        self.assertIn("K_NO_WAIT", scheduler)
        self.assertLess(
            scheduler.index("SURVEY_GATEWAY_DRIVE_POLL_CLEANUP"),
            scheduler.index("SURVEY_GATEWAY_DRIVE_RUN_NOW"),
        )
        self.assertIn("gateway_survey_schedule_drive();", worker)
        self.assertGreater(
            worker.index("gateway_survey_schedule_drive();"),
            worker.index("gateway_survey_auto_send_action("),
        )
        self.assertIn("gateway_survey_pair_observation_active", state)
        self.assertIn("gateway_survey_auto.running", state)
        self.assertIn("gateway_survey_auto.waiting", state)
        self.assertIn("gateway_survey_cleanup_pending()", state)
        self.assertIn("gateway_survey_observability.boundary_pending", state)

    def test_survey_discovery_scan_handoff_uses_facade(self):
        preempt = function_body(self.anchor, "anchor_preempt_for_survey_discovery")
        worker = function_body(self.survey_runtime, "survey_work_handler")

        self.assertIn("app_node_comm_stop_role_scan()", preempt)
        self.assertNotIn("mesh_stop_role_scan()", preempt)
        self.assertIn("app_node_comm_stop_role_scan()", worker)
        self.assertIn("app_node_comm_restart_role_scan()", worker)

    def test_survey_pair_radio_starts_only_after_gateway_confirmation(self):
        accept = function_body(
            self.survey_runtime,
            "app_anchor_survey_runtime_start_pair_from_command",
        )
        bind = function_body(
            self.survey_runtime,
            "app_anchor_survey_runtime_bind_pair_start_delivery",
        )
        gate = function_body(self.survey_runtime, "pair_start_delivery_ready")
        handler = function_body(self.anchor, "anchor_handle_local_command")
        worker = function_body(self.survey_runtime, "survey_work_handler")

        self.assertNotIn("schedule(K_NO_WAIT)", accept)
        self.assertIn("pair_start_delivery_handle = delivery_handle", bind)
        self.assertIn("schedule(K_NO_WAIT)", bind)
        self.assertIn("app_node_comm_take_delivery_event_for(", gate)
        self.assertIn("event.reason == NODE_COMM_TERMINAL_DELIVERED", gate)
        self.assertIn("survey_pair_lease_release_start(", gate)
        self.assertLess(
            handler.index("anchor_submit_command_result("),
            handler.index("app_anchor_survey_runtime_bind_pair_start_delivery("),
        )
        self.assertIn("pair_start_delivery_ready()", worker)

    def test_survey_pair_start_handle_is_abandoned_on_every_early_exit(self):
        abandon = function_body(
            self.survey_runtime, "abandon_pair_start_delivery"
        )
        cancel = function_body(
            self.survey_runtime,
            "app_anchor_survey_runtime_cancel_pair_start",
        )
        expire = function_body(self.survey_runtime, "pair_lease_work_handler")
        abort = function_body(
            self.survey_runtime, "app_anchor_survey_runtime_abort_pair"
        )
        targeted = function_body(
            self.survey_runtime,
            "app_anchor_survey_runtime_abort_pair_matching",
        )

        self.assertIn("app_node_comm_abandon_delivery(", abandon)
        for body in (cancel, expire, abort, targeted):
            self.assertIn("delivery_handle = pair_start_delivery_handle", body)
            self.assertIn("abandon_pair_start_delivery(", body)

    def test_post_result_side_effects_require_terminal_delivery(self):
        worker = function_body(
            self.command_completion, "completion_work_handler"
        )
        handler = function_body(self.anchor, "anchor_handle_local_command")

        take = worker.index("app_node_comm_take_delivery_event_for(")
        delivered = worker.index(
            "event.reason != NODE_COMM_TERMINAL_DELIVERED"
        )
        rediscovery = worker.index("completion_ops.force_rediscovery()")
        reboot = worker.index("completion_ops.schedule_reboot()")
        self.assertLess(take, delivered)
        self.assertLess(delivered, rediscovery)
        self.assertLess(delivered, reboot)
        self.assertIn("app_anchor_command_completion_watch(", handler)


if __name__ == "__main__":
    unittest.main()
