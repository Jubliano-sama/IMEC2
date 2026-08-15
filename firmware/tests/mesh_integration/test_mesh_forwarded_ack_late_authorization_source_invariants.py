#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
CH9_ACK = (ROOT / "app" / "src" / "app_mesh_ch9_ack.c").read_text()


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
    def test_route_repair_binds_logical_origin_but_event_repair_binds_child_edge(self):
        state = function_body(CH9_ACK, "c5_repair_pending_state_matches")

        self.assertIn(
            "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR", state
        )
        self.assertIn("pending->packet.src_id == peer_id", state)
        self.assertIn(
            "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR", state
        )
        self.assertIn(
            "pending->gateway_ack_forward_next_hop_id == peer_id", state
        )
        self.assertIn(
            "batch->template_ack.packet.dst_id == pending->packet.src_id",
            state,
        )
        self.assertIn("batch->template_ack.next_hop_id == peer_id", state)

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

    def test_ack_queue_captures_then_enters_shared_authorized_repair(self):
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
        shared_repair = actions.index(
            "mesh_propose_event_after_channel5_contact_authorized(", capture
        )

        self.assertLess(queued, queue_success)
        self.assertLess(queue_success, transit_owner)
        self.assertLess(transit_owner, capture)
        self.assertLess(capture, shared_repair)
        self.assertIn(
            "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR",
            actions[capture:shared_repair],
        )
        self.assertIn(
            "forward_ack->next_hop_id",
            actions[capture:shared_repair + 240],
        )
        self.assertNotIn(
            "mesh_event_accept_promote_forwarded_ack_repair(",
            actions[capture:shared_repair],
        )

    def test_stale_ack_retry_uses_the_same_authorized_repair_seam(self):
        select = function_body(REPORT, "mesh_select_channel9_ack_tx_event")
        stale = select.index("if (ret == PROTO_ERR_STALE)")
        capture = select.index(
            "app_mesh_ch9_c5_repair_authorization_capture(", stale
        )
        shared_repair = select.index(
            "mesh_propose_event_after_channel5_contact_authorized(", capture
        )

        self.assertLess(stale, capture)
        self.assertLess(capture, shared_repair)
        self.assertIn(
            "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR",
            select[stale:capture],
        )
        self.assertNotIn(
            "mesh_event_accept_promote_forwarded_ack_repair(",
            select[capture:shared_repair],
        )

    def test_preexisting_accept_is_upgraded_in_place_and_retried_now(self):
        authorized = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        promote = function_body(
            REPORT, "mesh_event_accept_promote_forwarded_ack_repair"
        )

        supplied = authorized.index(
            "authorization != NULL && authorization->valid"
        )
        kind = authorized.index(
            "mesh_authorization_is_ack_event_repair(authorization)", supplied
        )
        peer = authorized.index("authorization->peer_id != peer_id", kind)
        owner_match = authorized.index(
            "app_mesh_ch9_c5_repair_owner_matches(", peer
        )
        stale_reject = authorized.index("return -ESTALE", owner_match)
        proposal_conflict = authorized.index(
            "mesh_event_propose_retry.active", stale_reject
        )
        retain_fresh = authorized.index(
            "mesh_forwarded_ack_event_repair_authorization = *authorization",
            proposal_conflict,
        )
        active_accept_branch = authorized.index(
            "mesh_event_accept_retry.retry.active", retain_fresh
        )
        promote_call = authorized.index(
            "mesh_event_accept_promote_forwarded_ack_repair(",
            active_accept_branch,
        )

        self.assertLess(supplied, kind)
        self.assertLess(kind, peer)
        self.assertLess(peer, owner_match)
        self.assertLess(owner_match, stale_reject)
        self.assertLess(stale_reject, proposal_conflict)
        self.assertLess(proposal_conflict, retain_fresh)
        self.assertLess(retain_fresh, active_accept_branch)
        self.assertLess(active_accept_branch, promote_call)
        self.assertIn("return -EBUSY", authorized[kind:owner_match])
        self.assertNotIn(
            "mesh_event_accept_retry.c5_repair_authorization =",
            authorized[supplied:promote_call],
        )

        active_accept = promote.index("mesh_event_accept_retry.retry.active")
        peer_match = promote.index(
            "mesh_event_accept_retry.retry.peer_id", active_accept
        )
        now = promote.index("now_ms = k_uptime_get_32()", peer_match)
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
        self.assertLess(peer_match, now)
        self.assertLess(now, expired)
        self.assertLess(expired, expired_clear)
        self.assertLess(expired_clear, expired_fallback)
        self.assertLess(expired_fallback, attach)
        self.assertLess(attach, arm)
        self.assertLess(arm, schedule)
        self.assertIn("retry_due_ms", promote[attach:schedule])
        self.assertNotIn("app_mesh_ch9_c5_repair_owner_matches", promote)
        self.assertNotIn("app_mesh_c5_tx_authorization_token_equal", promote)

if __name__ == "__main__":
    unittest.main()
