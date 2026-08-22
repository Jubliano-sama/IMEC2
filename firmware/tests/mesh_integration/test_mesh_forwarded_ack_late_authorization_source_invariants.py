#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
CH9_ACK = (ROOT / "app" / "src" / "app_mesh_ch9_ack.c").read_text()
MESH = (ROOT / "src" / "mesh.c").read_text()
MESH_RELAY_ROUTES = (ROOT / "src" / "mesh_relay_routes.inc").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^[A-Za-z_][^;{{]*?\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
    )
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

    def test_forwarded_ack_repair_waits_for_the_child_proposal(self):
        authorized = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )

        retain = authorized.index(
            "mesh_forwarded_ack_event_repair_authorization = *authorization"
        )
        forwarded_only = authorized.index(
            "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR",
            retain,
        )
        yield_to_child = authorized.index("return -EAGAIN", forwarded_only)
        prepare_local_proposal = authorized.index(
            "mesh_prepare_event_control_record(", yield_to_child
        )
        send_local_proposal = authorized.index(
            "mesh_send_event_control_record(", prepare_local_proposal
        )

        self.assertLess(retain, forwarded_only)
        self.assertLess(forwarded_only, yield_to_child)
        self.assertLess(yield_to_child, prepare_local_proposal)
        self.assertLess(prepare_local_proposal, send_local_proposal)
        self.assertNotIn(
            "APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR",
            authorized[forwarded_only:yield_to_child],
            "late-terminal recovery must retain its local-proposal fallback",
        )

    def test_usable_timing_skips_proposal_without_an_event_owner(self):
        authorized = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        timing = authorized.index("mesh_find_active_channel9_timing(")
        timing_guard = authorized.rfind("if (", 0, timing)
        reuse = authorized.index("return 0;", timing)
        prepare_local_proposal = authorized.index(
            "mesh_prepare_event_control_record(", reuse
        )

        self.assertGreaterEqual(timing_guard, 0)
        self.assertNotIn(
            "current_owner", authorized[timing_guard:timing],
            "usable timing must not depend on a separately retained owner",
        )
        self.assertLess(timing, reuse)
        self.assertLess(reuse, prepare_local_proposal)

    def test_late_terminal_auth_retains_the_local_proposal_fallback(self):
        allowed = function_body(CH9_ACK, "app_mesh_ch9_c5_repair_allowed")
        self.assertIn(
            "APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR", allowed
        )
        self.assertIn("MSG_MESH_EVENT_PROPOSE", allowed)

    def test_forwarded_ack_cannot_be_discarded_before_physical_commit(self):
        physical = function_body(
            REPORT, "mesh_ch9_ack_batch_requires_physical_commit"
        )
        discard = function_body(
            REPORT, "mesh_ch9_ack_batch_discard_if_safe"
        )

        self.assertIn("batch->preserve_payload", physical)
        self.assertIn(
            "batch->owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE", physical
        )
        self.assertIn(
            "batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK", physical
        )
        custody = discard.index(
            "mesh_ch9_ack_batch_preserves_terminal_forward(peer_id)"
        )
        retain = discard.index("return false;", custody)
        clear = discard.index("mesh_ch9_ack_batch_clear_for_peer", retain)
        self.assertLess(custody, retain)
        self.assertLess(retain, clear)

    def test_parent_forwarded_ack_batch_never_releases_child_timing(self):
        send = function_body(REPORT, "mesh_send_pending_ch9_ack_batch")
        physical_send = send.index("mesh_send_outbound(")
        physical_commit = send.index(
            "mesh_commit_forwarded_gateway_ack_sent(&ack)", physical_send
        )
        clear_exact_ack = send.index(
            "app_mesh_ch9_ack_table_clear_peer", physical_commit
        )

        self.assertLess(physical_send, physical_commit)
        self.assertLess(physical_commit, clear_exact_ack)
        self.assertNotIn("survey_release_ch9_if_current", send)
        self.assertNotIn("mesh_close_channel9_connection", send)
        self.assertNotIn("mesh_relay_clear_channel9_timing", send)

    def test_child_closes_survey_event_only_after_all_topology_work_drains(self):
        actions = function_body(REPORT, "mesh_handle_result_actions")
        drain = function_body(REPORT, "mesh_drain_rx_queue_locked")
        arm_close = function_body(REPORT, "mesh_close_channel9_connection")

        gateway_start = actions.index(
            "if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED)"
        )
        gateway_end = actions.index("gateway_confirmation_done:", gateway_start)
        hop_start = actions.index(
            "if (result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS)",
            gateway_end,
        )
        hop_end = actions.index(
            "if (result->actions & MESH_RELAY_ACTION_TX_RELAY_BUSY)", hop_start
        )

        gateway_branch = actions[gateway_start:gateway_end]
        local_confirm = gateway_branch.index(
            "confirmed_packet->msg_type == MSG_GATEWAY_ACK_CONFIRM"
        )
        source_empty = gateway_branch.index(
            "app_node_comm_pending_delivery_count() == 0u", local_confirm
        )
        queue_empty = gateway_branch.index(
            "report_tx_queue_used() == 0u", source_empty
        )
        route_empty = gateway_branch.index(
            "!mesh_route_waiting_tx_valid", queue_empty
        )
        ack_empty = gateway_branch.index(
            "!app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)",
            route_empty,
        )
        event_end = gateway_branch.index(
            "mesh_close_channel9_connection(", ack_empty
        )
        self.assertLess(local_confirm, source_empty)
        self.assertLess(source_empty, queue_empty)
        self.assertLess(queue_empty, route_empty)
        self.assertLess(route_empty, ack_empty)
        self.assertLess(ack_empty, event_end)
        self.assertNotIn(
            "mesh_close_channel9_connection(", actions[hop_start:hop_end]
        )
        self.assertIn("mesh_handle_result_actions(", drain)

        publish = arm_close.index(
            "*free_intent = (struct mesh_ch9_close_intent)"
        )
        valid = arm_close.index(".valid = true", publish)
        schedule = arm_close.index(
            "mesh_event_negotiation_schedule_next()", valid
        )
        self.assertLess(publish, valid)
        self.assertLess(valid, schedule)
        self.assertIn(".peer_id = peer_id", arm_close[publish:valid])
        self.assertIn(".retry_due_ms = now_ms", arm_close[publish:valid])
        self.assertIn(
            ".owner_session_id = owner->session_id", arm_close[publish:valid]
        )
        self.assertIn(
            ".owner_generation = owner->generation", arm_close[publish:valid]
        )
        self.assertIn(".upstream =", arm_close[publish:valid])
        self.assertIn(".requires_live_timing =", arm_close[publish:valid])
        self.assertNotIn("mesh_send_event_control", arm_close)
        self.assertNotIn("mesh_relay_clear_channel9_timing", arm_close)
        self.assertNotIn("mesh_event_owner_abandon", arm_close)

        ack_complete = function_body(REPORT, "mesh_ch9_tx_pending_handle_ack")
        live_source = ack_complete.index(
            "app_node_comm_pending_delivery_count() != 0u"
        )
        live_queue = ack_complete.index(
            ".report_tx_queue_used = report_tx_queue_used()"
        )
        live_route = ack_complete.index(
            ".route_waiting_tx_valid = mesh_route_waiting_tx_valid", live_queue
        )
        live_acks = ack_complete.index(
            "app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)", live_route
        )
        close_policy = ack_complete.index(
            "app_mesh_ch9_ack_complete_should_close_timing", live_acks
        )
        close_after_ack = ack_complete.index(
            "mesh_close_channel9_connection(", close_policy
        )
        self.assertLess(live_source, live_queue)
        self.assertLess(live_queue, live_route)
        self.assertLess(live_route, live_acks)
        self.assertLess(live_acks, close_policy)
        self.assertLess(close_policy, close_after_ack)

        policy = function_body(
            CH9_ACK, "app_mesh_ch9_ack_complete_should_close_timing"
        )
        self.assertRegex(
            policy,
            r"return !state->source_delivery_pending\s*&&\s*"
            r"state->report_tx_queue_used == 0u\s*&&\s*"
            r"!state->route_waiting_tx_valid\s*&&\s*"
            r"!state->ack_batch_valid;",
        )

    def test_deferred_close_worker_retries_without_one_sided_teardown(self):
        worker = function_body(
            REPORT, "mesh_event_negotiation_retry_work_handler"
        )
        service = function_body(
            REPORT, "mesh_channel9_close_intent_service_due"
        )
        attempt = function_body(REPORT, "mesh_try_close_channel9_connection")

        service_call = worker.index(
            "mesh_channel9_close_intent_service_due(now_ms)"
        )
        accept_retry = worker.index("mesh_event_accept_retry", service_call)
        final_schedule = worker.rindex("mesh_event_negotiation_schedule_next()")
        self.assertLess(service_call, accept_retry)
        self.assertLess(accept_retry, final_schedule)

        valid_gate = service.index("if (!intent->valid")
        due_gate = service.index("intent->retry_due_ms", valid_gate)
        live_timing = service.index("intent->requires_live_timing", due_gate)
        supervision_retire = service.index(
            "memset(intent, 0, sizeof(*intent))", live_timing
        )
        drain_gates = (
            "app_node_comm_pending_delivery_count() != 0u",
            "mesh_report_tx_backlog_active()",
            "report_tx_queue_used() != 0u",
            "mesh_route_waiting_tx_valid",
            "mesh_relay_tx_active(&mesh_runtime)",
            "mesh_ch9_tx_pending_is_active()",
            "mesh_relay_result_bundle_pending(&mesh_runtime)",
            "app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)",
            "k_msgq_num_used_get(&mesh_rx_msgq) != 0u",
            "atomic_get(&mesh_rx_handler_active_state) != 0",
            '!mesh_coordinator_c5_tx_allowed("ch9-event-close")',
        )
        gate_positions = []
        gate_cursor = supervision_retire
        for gate in drain_gates:
            gate_cursor = service.index(gate, gate_cursor)
            gate_positions.append(gate_cursor)
        defer_due = service.index(
            "intent->retry_due_ms = now_ms +", gate_positions[-1]
        )
        defer_continue = service.index("continue;", defer_due)
        send = service.index(
            "mesh_try_close_channel9_connection(", defer_continue
        )
        success = service.index("if (ret == 0)", send)
        success_clear = service.index(
            "memset(intent, 0, sizeof(*intent))", success
        )
        retry_due = service.index("intent->retry_due_ms = now_ms +", success_clear)
        self.assertLess(valid_gate, due_gate)
        self.assertLess(due_gate, live_timing)
        self.assertLess(live_timing, supervision_retire)
        self.assertEqual(gate_positions, sorted(gate_positions))
        self.assertLess(supervision_retire, gate_positions[0])
        self.assertLess(gate_positions[-1], defer_due)
        self.assertLess(defer_due, defer_continue)
        self.assertLess(defer_continue, send)
        self.assertLess(send, success)
        self.assertLess(success, success_clear)
        self.assertLess(success_clear, retry_due)
        stale_without_timing = service.index(
            "if (!intent->requires_live_timing && ret == -ESTALE)",
            success_clear,
        )
        stale_clear = service.index(
            "memset(intent, 0, sizeof(*intent))", stale_without_timing
        )
        self.assertLess(stale_without_timing, stale_clear)
        self.assertLess(stale_clear, retry_due)
        self.assertIn(
            "MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS",
            service[retry_due:],
        )
        self.assertNotIn("supervision_timeout_ms", service)

        send_end = attempt.index("mesh_send_event_control(")
        end_type = attempt.index("MSG_MESH_EVENT_END", send_end)
        failure = attempt.index("if (ret == 0)", end_type)
        retain_return = attempt.index("return ret;", failure)
        clear_timing = attempt.index(
            "mesh_relay_clear_channel9_timing", retain_return
        )
        success_return = attempt.index("return 0;", clear_timing)
        self.assertLess(send_end, end_type)
        self.assertLess(end_type, failure)
        self.assertLess(failure, retain_return)
        self.assertLess(retain_return, clear_timing)
        self.assertLess(clear_timing, success_return)
        self.assertNotIn(
            "mesh_relay_clear_channel9_timing", attempt[failure:retain_return]
        )
        self.assertNotIn("mesh_event_owner_abandon", attempt)

    def test_pending_close_blocks_parent_switch_and_owns_scheduler_due_time(self):
        block = function_body(
            REPORT, "mesh_channel9_close_intent_blocks_upstream"
        )
        propose = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        schedule = function_body(REPORT, "mesh_event_negotiation_schedule_next")

        selected = block.index("route_selected(&mesh_runtime.upstream)")
        requested_parent = block.index(
            "selected->next_hop_id != peer_id", selected
        )
        valid = block.index("if (!intent->valid)", requested_parent)
        upstream = block.index("if (intent->upstream", valid)
        different_parent = block.index(
            "intent->peer_id != peer_id", upstream
        )
        blocked = block.index("return true;", different_parent)
        self.assertLess(selected, requested_parent)
        self.assertLess(requested_parent, valid)
        self.assertLess(valid, upstream)
        self.assertLess(upstream, different_parent)
        self.assertLess(different_parent, blocked)

        close_gate = propose.index("mesh_channel9_close_intent_blocks_upstream")
        reject = propose.index("return -EAGAIN", close_gate)
        timing_reuse = propose.index("mesh_find_active_channel9_timing", reject)
        self.assertLess(close_gate, reject)
        self.assertLess(reject, timing_reuse)

        close_due = schedule.index("mesh_channel9_close_intent_next_delay(")
        earlier = schedule.index("close_delay_ms < delay_ms", close_due)
        select_due = schedule.index("delay_ms = close_delay_ms", earlier)
        cancel = schedule.index("mesh_cancel_delayable", select_due)
        reschedule = schedule.index("mesh_reschedule_owned_work", cancel)
        self.assertLess(close_due, earlier)
        self.assertLess(earlier, select_due)
        self.assertLess(select_due, cancel)
        self.assertLess(cancel, reschedule)

    def test_close_intent_is_bound_to_exact_event_owner_identity(self):
        struct_start = REPORT.index("struct mesh_ch9_close_intent {")
        intent_struct = REPORT[struct_start:REPORT.index("};", struct_start)]
        arm = function_body(REPORT, "mesh_close_channel9_connection")
        helpers = (
            function_body(REPORT, "mesh_channel9_close_intent_next_delay"),
            function_body(REPORT, "mesh_channel9_close_intent_blocks_upstream"),
            function_body(REPORT, "mesh_channel9_close_intent_service_due"),
        )

        for field in (
            "uint64_t peer_id",
            "uint32_t owner_session_id",
            "uint32_t owner_generation",
            "bool upstream",
            "bool requires_live_timing",
            "bool valid",
        ):
            self.assertIn(field, intent_struct)
        self.assertRegex(
            REPORT, r"#define\s+MESH_CH9_CLOSE_INTENT_MAX\s+2u\b"
        )

        owner = arm.index("owner = mesh_event_owner_for_peer(peer_id)")
        active = arm.index("owner == NULL || !owner->active", owner)
        nonzero_session = arm.index("owner->session_id == 0u", active)
        nonzero_generation = arm.index("owner->generation == 0u", nonzero_session)
        snapshot_session = arm.index(
            ".owner_session_id = owner->session_id", nonzero_generation
        )
        snapshot_generation = arm.index(
            ".owner_generation = owner->generation", snapshot_session
        )
        self.assertLess(owner, active)
        self.assertLess(active, nonzero_session)
        self.assertLess(nonzero_session, nonzero_generation)
        self.assertLess(nonzero_generation, snapshot_session)
        self.assertLess(snapshot_session, snapshot_generation)

        for helper in helpers:
            owner_lookup = helper.index(
                "owner = mesh_event_owner_for_peer(intent->peer_id)"
            )
            active_guard = helper.index(
                "owner == NULL || !owner->active", owner_lookup
            )
            session_guard = helper.index(
                "owner->session_id != intent->owner_session_id", active_guard
            )
            generation_guard = helper.index(
                "owner->generation != intent->owner_generation", session_guard
            )
            stale_clear = helper.index(
                "memset(intent, 0, sizeof(*intent))", generation_guard
            )
            self.assertLess(owner_lookup, active_guard)
            self.assertLess(active_guard, session_guard)
            self.assertLess(session_guard, generation_guard)
            self.assertLess(generation_guard, stale_clear)

    def test_channel9_supervision_remains_the_teardown_fallback(self):
        usable = function_body(MESH, "mesh_event_timing_usable")
        relay_expire = function_body(
            MESH_RELAY_ROUTES, "mesh_relay_expire_channel9_timings"
        )
        app_expire = function_body(REPORT, "mesh_expire_channel9_timings")

        self.assertIn("timing->supervision_timeout_ms == 0u", usable)
        self.assertIn(
            "timing->last_successful_ch9_event_ms +\n"
            "                         timing->supervision_timeout_ms",
            usable,
        )
        self.assertIn("!mesh_event_timing_usable(&entry->timing, now_ms)", relay_expire)
        owner_retire = app_expire.index("mesh_event_owner_abandon_peer")
        timing_expire = app_expire.index(
            "mesh_relay_expire_channel9_timings", owner_retire
        )
        self.assertLess(owner_retire, timing_expire)

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
