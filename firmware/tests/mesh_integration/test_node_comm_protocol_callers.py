#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"


def function_body(source: str, name: str) -> str:
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
                    return source[brace : index + 1]
    raise AssertionError(f"function definition not found: {name}")


class NodeCommProtocolCallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.anchor = (APP_SRC / "app_anchor.c").read_text(encoding="utf-8")
        cls.survey = (APP_SRC / "app_anchor_survey_discovery.c").read_text(
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
        for name in ("anchor_send_command_result", "anchor_send_discovery_response"):
            with self.subTest(name=name):
                body = function_body(self.anchor, name)
                self.assertIn("app_node_comm_start_delivery(", body)
                self.assertNotIn("mesh_start_tracked_tx(", body)

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
        self.assertIn("app_node_comm_send_control_flood(", sender)
        self.assertNotIn("mesh_send_c5_flood(", sender)
        self.assertNotIn("mesh_start_tracked_tx(", sender)

        self.assertEqual(automatic.count("gateway_survey_prepare_pair_control("), 1)
        self.assertIn("app_node_comm_submit_delivery(", automatic)
        self.assertIn("NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD", automatic)
        self.assertEqual(manual.count("gateway_survey_send_pair_control("), 1)
        self.assertNotIn("mesh_send_c5_flood(", automatic)
        self.assertNotIn("mesh_start_tracked_tx(", manual)

        for command in ("CMD_SURVEY_PREPARE_PAIR", "CMD_SURVEY_START_PAIR"):
            with self.subTest(command=command):
                self.assertIn(command, manual)
                self.assertIn(command, router)
                self.assertIn(command, classifier)

    def test_targeted_survey_abort_is_pair_scoped(self):
        executor = function_body(self.anchor, "anchor_execute_command_side_effects")
        targeted = function_body(self.anchor,
                                 "anchor_abort_survey_pair_matching")

        self.assertIn("survey_extract_pair_tlvs(", executor)
        self.assertIn("anchor_abort_survey_pair_matching(", executor)
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

    def test_survey_discovery_scan_handoff_uses_facade(self):
        preempt = function_body(self.anchor, "anchor_preempt_for_survey_discovery")
        worker = function_body(self.anchor, "anchor_survey_work_handler")

        self.assertIn("app_node_comm_stop_role_scan()", preempt)
        self.assertNotIn("mesh_stop_role_scan()", preempt)
        self.assertIn("app_node_comm_stop_role_scan()", worker)
        self.assertIn("app_node_comm_restart_role_scan()", worker)


if __name__ == "__main__":
    unittest.main()
