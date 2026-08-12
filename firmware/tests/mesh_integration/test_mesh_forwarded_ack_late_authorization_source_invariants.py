#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


class ForwardedAckLateAuthorizationSourceInvariantTests(unittest.TestCase):
    def test_exact_forwarded_ack_repair_can_escape_only_its_idle_mesh_rx_owner(self):
        capture = function_body(
            REPORT, "mesh_coordinator_decide_for_c5_intent"
        )
        decide = capture.index("app_mesh_command_orchestrator_decide(")
        publish = capture.index("*capture_out = capture", decide)
        self.assertIn("capture_out != NULL", capture[decide:publish])

        allowed = function_body(
            REPORT, "mesh_coordinator_c5_tx_allowed_authorized_intent"
        )
        captured_decision = allowed.index(
            "mesh_coordinator_decide_for_c5_intent("
        )
        exact = allowed.index("exact_ack_rx_repair_state =", captured_decision)
        state_gate = allowed.index(
            "if (decision.state != FW_RADIO_ACTIVITY_MESH_TX", exact
        )
        exact_expression = allowed[exact:state_gate]

        self.assertIn("&decision, &capture", allowed[captured_decision:exact])
        for required in (
            "decision.state == FW_RADIO_ACTIVITY_MESH_RX",
            "capture.relay_tx_active",
            "capture.ch9_ack_wait_active",
            "capture.ch9_ack_receive_eligible",
            "capture.rx_queue_used == 0u",
            "!capture.click_active",
            "!capture.survey_pending",
            "!capture.gateway_continuous_ch9",
        ):
            self.assertIn(required, exact_expression)
        self.assertNotIn("||", exact_expression)
        self.assertEqual(exact_expression.count("&&"), 7)

        authorization = allowed.index(
            "authorization != NULL && authorization->valid", state_gate
        )
        exact_token = allowed.index(
            "app_mesh_ch9_c5_repair_allowed(", authorization
        )
        repair_allowed = allowed.index("return true;", exact_token)
        rejected = allowed.rindex("return false;")

        self.assertIn(
            "!exact_ack_rx_repair_state", allowed[state_gate:authorization]
        )
        self.assertLess(state_gate, authorization)
        self.assertLess(authorization, exact_token)
        self.assertLess(exact_token, repair_allowed)
        self.assertLess(repair_allowed, rejected)

    def test_ack_queue_captures_then_promotes_before_fallback_propose(self):
        actions = function_body(REPORT, "mesh_handle_result_actions")

        queued = actions.index(
            "mesh_ch9_ack_batch_queue_forwarded_gateway_ack("
        )
        queue_success = actions.index("forward_queued_gateway_ack = ret == 0", queued)
        transit_owner = actions.index(
            "MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING",
            queue_success,
        )
        capture = actions.index(
            "app_mesh_ch9_c5_repair_authorization_capture(", transit_owner
        )
        promote = actions.index(
            "mesh_event_accept_promote_forwarded_ack_repair(", capture
        )
        no_accept = actions.index("promote_ret == -ENOENT", promote)
        fallback = actions.index(
            "mesh_propose_event_after_channel5_contact_authorized(", no_accept
        )

        self.assertLess(queued, queue_success)
        self.assertLess(queue_success, transit_owner)
        self.assertLess(transit_owner, capture)
        self.assertLess(capture, promote)
        self.assertLess(promote, no_accept)
        self.assertLess(no_accept, fallback)
        self.assertIn(
            "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR",
            actions[capture:promote],
        )
        self.assertIn("forward_ack->next_hop_id", actions[capture:fallback + 200])

    def test_preexisting_accept_is_upgraded_in_place_and_retried_now(self):
        promote = function_body(
            REPORT, "mesh_event_accept_promote_forwarded_ack_repair"
        )

        active_accept = promote.index("mesh_event_accept_retry.retry.active")
        peer_match = promote.index(
            "mesh_event_accept_retry.retry.peer_id", active_accept
        )
        owner_match = promote.index(
            "app_mesh_ch9_c5_repair_owner_matches(", peer_match
        )
        exact_existing = promote.index(
            "app_mesh_c5_tx_authorization_token_equal(", owner_match
        )
        now = promote.index("now_ms = k_uptime_get_32()", exact_existing)
        expired = promote.index("app_mesh_event_retry_expired(", now)
        expired_clear = promote.index(
            "memset(&mesh_event_accept_retry, 0", expired
        )
        expired_fallback = promote.index("return -ENOENT", expired_clear)
        attach = promote.index(
            "mesh_event_accept_retry.c5_repair_authorization =",
            expired_fallback,
        )
        arm = promote.index("retry_due_armed = true", attach)
        schedule = promote.index("mesh_event_negotiation_schedule_next()", arm)

        self.assertLess(active_accept, peer_match)
        self.assertLess(peer_match, owner_match)
        self.assertLess(owner_match, exact_existing)
        self.assertLess(exact_existing, now)
        self.assertLess(now, expired)
        self.assertLess(expired, expired_clear)
        self.assertLess(expired_clear, expired_fallback)
        self.assertLess(expired_fallback, attach)
        self.assertLess(exact_existing, attach)
        self.assertLess(attach, arm)
        self.assertLess(arm, schedule)
        self.assertIn("retry_due_ms", promote[attach:schedule])

if __name__ == "__main__":
    unittest.main()
