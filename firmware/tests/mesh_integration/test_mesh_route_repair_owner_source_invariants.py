#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
NODE_COMM = read_composed_source(ROOT / "app" / "src" / "app_node_comm.c")
ASYNC_ROUTE_HEADER = (
    ROOT / "app" / "src" / "app_mesh_async_route_request.h"
).read_text(encoding="utf-8")
REPORT_HEADER = (ROOT / "app" / "src" / "app_mesh_report.h").read_text(
    encoding="utf-8"
)
ANCHOR = read_composed_source(ROOT / "app" / "src" / "app_anchor.c")
GATEWAY_BLE = read_composed_source(
    ROOT / "app" / "src" / "app_gateway_ble.c"
)
RELAY = read_composed_source(ROOT / "src" / "mesh_relay.c")
ROUTE_WAIT = (
    ROOT / "app" / "src" / "app_mesh_route_wait_tx.h"
).read_text(encoding="utf-8")
ROUTE_WAIT_IMPL = (
    ROOT / "app" / "src" / "app_mesh_route_wait_tx.c"
).read_text(encoding="utf-8")


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


class MeshRouteRepairOwnerSourceInvariantTests(unittest.TestCase):
    def test_route_ready_retires_exact_async_target_and_resumes_both_owners(self):
        complete = function_body(
            REPORT, "mesh_complete_async_route_request_for_ready_target"
        )
        actions = function_body(REPORT, "mesh_handle_result_actions")

        self.assertIn("k_mutex_lock(&mesh_route_discovery_lock", complete)
        self.assertIn("app_mesh_async_route_request_snapshot(", complete)
        self.assertIn("attempt.target_id == target_id", complete)
        self.assertIn("app_mesh_async_route_request_complete(", complete)
        self.assertIn("k_mutex_unlock(&mesh_route_discovery_lock)", complete)

        ready = actions.index("MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY")
        ready_end = actions.index(
            "MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING", ready
        )
        ready_path = actions[ready:ready_end]
        cancel = ready_path.index(
            "mesh_complete_async_route_request_for_ready_target("
        )
        target = ready_path.index("result->route_discovery_target_id")
        route_retry = ready_path.index("mesh_schedule_route_waiting_retry")
        core_retry = ready_path.index("mesh_schedule_tx_timeout")
        self.assertLess(target, cancel)
        self.assertLess(cancel, route_retry)
        self.assertLess(cancel, core_retry)

        # Every core transition that publishes READY must carry the exact
        # target; selected upstream state is not a safe proxy for downlinks.
        self.assertRegex(
            RELAY,
            r"result->route_discovery_target_id\s*=\s*fields\.gateway_id\s*;"
            r"\s*result->actions\s*\|=\s*"
            r"MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY",
        )
        target_ready = re.findall(
            r"result->route_discovery_target_id\s*=\s*fields\.target_id\s*;"
            r"\s*result->actions\s*\|=\s*"
            r"MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY",
            RELAY,
        )
        self.assertGreaterEqual(len(target_ready), 2)

    def test_core_timeout_route_repair_is_owner_bound_before_rf_and_defer(self):
        schedule = function_body(
            REPORT, "mesh_schedule_route_request_authorized"
        )
        owner_match = function_body(
            REPORT, "mesh_async_route_transfer_owner_matches"
        )
        worker = function_body(REPORT, "mesh_route_discovery_work_handler")

        route_wait = schedule.index(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT"
        )
        core_pending = schedule.index(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING"
        )
        self.assertLess(route_wait, core_pending)
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", schedule)
        self.assertIn("mesh_runtime.pending.packet.session_id", schedule)
        self.assertIn("mesh_runtime.pending.packet.seq", schedule)
        self.assertIn("mesh_runtime.pending.packet.msg_type", schedule)

        self.assertIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING", owner_match
        )
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", owner_match)
        self.assertIn("app_mesh_async_route_request_transfer_matches(", owner_match)

        pre_rf = worker.index(
            "if (!mesh_async_route_transfer_owner_matches(&attempt))"
        )
        rf = worker.index("mesh_request_route(", pre_rf)
        post_rf = worker.index(
            "!mesh_async_route_transfer_owner_matches(&attempt)", rf
        )
        defer = worker.index("app_mesh_async_route_request_defer(", post_rf)
        complete = worker.index("ret == -ESTALE", post_rf)
        self.assertLess(pre_rf, rf)
        self.assertLess(rf, post_rf)
        self.assertLess(post_rf, complete)
        self.assertLess(complete, defer)

    def test_node_comm_route_repair_is_generation_owned_until_terminal_cancel(self):
        service = function_body(NODE_COMM, "app_node_comm_service_deliveries")
        reconcile = function_body(
            NODE_COMM, "app_node_comm_reconcile_terminal_backends_locked"
        )
        owner_match = function_body(
            REPORT, "mesh_async_route_transfer_owner_matches"
        )
        send = function_body(REPORT, "mesh_try_send_reliable_uplink_view")
        cancel_route = function_body(
            REPORT, "mesh_cancel_reliable_uplink_route_owned"
        )
        cancel_request = function_body(
            REPORT, "mesh_request_reliable_uplink_cancel"
        )
        cancel_worker = function_body(
            REPORT, "mesh_node_comm_cancel_work_handler"
        )
        route_worker = function_body(
            REPORT, "mesh_route_discovery_work_handler"
        )

        self.assertIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM", ASYNC_ROUTE_HEADER
        )

        # The facade lease generation crosses the backend seam instead of
        # being reconstructed later from a destination shared by successors.
        generation = service.index(
            ".delivery_generation = lease.delivery_generation"
        )
        backend = service.index("mesh_try_send_reliable_uplink_view(")
        self.assertLess(generation, backend)

        self.assertIn("view->delivery_generation == 0u", send)
        self.assertIn(
            ".owner_generation = view->delivery_generation", send
        )
        self.assertIn(".packet_seq = out->packet.seq", send)
        self.assertIn(".msg_type = out->packet.msg_type", send)
        self.assertIn(
            ".owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM", send
        )
        self.assertIn("&route_transfer", send)

        self.assertIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM", owner_match
        )
        self.assertIn("app_node_comm_delivery_owner_matches(", owner_match)
        self.assertIn("attempt->transfer.owner_generation", owner_match)
        self.assertIn("attempt->transfer.target_id", owner_match)
        self.assertIn("attempt->transfer.packet_seq", owner_match)
        self.assertIn("attempt->transfer.msg_type", owner_match)

        # Every async worker probe checks the exact live owner first and
        # checks again after RF before it can retain a deferred retry.
        pre_rf = route_worker.index(
            "if (!mesh_async_route_transfer_owner_matches(&attempt))"
        )
        rf = route_worker.index("mesh_request_route(", pre_rf)
        post_rf = route_worker.index(
            "!mesh_async_route_transfer_owner_matches(&attempt)", rf
        )
        defer = route_worker.index(
            "app_mesh_async_route_request_defer(", post_rf
        )
        self.assertLess(pre_rf, rf)
        self.assertLess(rf, post_rf)
        self.assertLess(post_rf, defer)

        # Terminalization freezes the same generation into cancellation;
        # matching the target alone cannot cancel a same-target successor.
        self.assertIn("terminal.delivery_generation", reconcile)
        self.assertRegex(
            REPORT_HEADER,
            r"mesh_request_reliable_uplink_cancel\s*\([^;]*"
            r"uint32_t delivery_generation\s*,",
        )
        self.assertIn(
            ".delivery_generation = delivery_generation", cancel_request
        )
        self.assertIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM", cancel_route
        )
        self.assertIn("request->delivery_generation", cancel_route)
        match = cancel_route.index(
            "app_mesh_async_route_request_transfer_matches("
        )
        complete = cancel_route.index(
            "app_mesh_async_route_request_complete(", match
        )
        reschedule = cancel_route.index("mesh_schedule_tx_timeout()", complete)
        self.assertLess(match, complete)
        self.assertLess(complete, reschedule)

        # The deferred route request is invalidated before terminal release is
        # published, so its worker cannot survive to issue another RF probe.
        cancel = cancel_worker.index(
            "mesh_cancel_reliable_uplink_route_owned(&request)"
        )
        publish = cancel_worker.index(
            "mesh_node_comm_cancel_complete(&request, ret)", cancel
        )
        release = cancel_worker.index(
            "app_node_comm_backend_release_ready(", publish
        )
        self.assertLess(cancel, publish)
        self.assertLess(publish, release)

    def test_forwarded_gateway_ack_route_wait_cannot_be_replaced(self):
        self.assertTrue(
            "APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK" in ROUTE_WAIT,
            "route-wait ownership needs an explicit transit gateway-ACK class",
        )
        store = function_body(REPORT, "mesh_store_route_waiting_tx_owned")
        exact = function_body(
            REPORT, "mesh_route_waiting_tx_exact_owner_equal"
        )

        self.assertIn("mesh_route_waiting_tx_owner", store)
        self.assertIn("mesh_route_waiting_tx_exact_owner_equal", store)
        self.assertIn("next_hop_id", exact)
        self.assertIn("mesh_packet_semantic_digest", exact)
        self.assertIn("memcmp", exact)
        critical = store.index(
            "APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK"
        )
        reject = store.index("return false", critical)
        publish = store.index("mesh_route_waiting_tx = waiting")
        self.assertLess(reject, publish)

        actions = function_body(REPORT, "mesh_handle_result_actions")
        self.assertIn("mesh_store_route_waiting_tx_owned(", actions)
        self.assertIn(
            "APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK", actions
        )
        self.assertNotIn("mesh_store_route_waiting_tx(", REPORT)

    def test_gateway_command_timeout_cannot_clear_transit_ack_route_wait(self):
        clear = function_body(REPORT, "mesh_clear_route_waiting_tx")
        timeout = function_body(ANCHOR, "gateway_command_timeout_side_effects")
        wait_commit = function_body(
            GATEWAY_BLE, "gateway_command_result_wait_commit"
        )
        dispatch = function_body(
            GATEWAY_BLE, "gateway_command_timeout_side_effect_handler"
        )
        exact = function_body(
            ROUTE_WAIT_IMPL, "app_mesh_route_wait_tx_clear_matches"
        )

        self.assertIn("app_mesh_route_wait_tx_clear_matches(", clear)
        self.assertIn("mesh_route_waiting_tx_owner", clear)
        self.assertIn("expected_owner", clear)
        self.assertIn("mesh_route_waiting_tx.payload", clear)
        self.assertIn("semantic_digest", clear)
        self.assertIn("active_owner == expected_owner", exact)
        self.assertIn("mesh_packet_semantic_digest", clear)
        self.assertIn("semantic_digest_equal", exact)
        self.assertNotIn('#include "mesh.h"', ROUTE_WAIT_IMPL)
        self.assertLess(
            clear.index("mesh_packet_semantic_digest("),
            clear.index("app_mesh_route_wait_tx_clear_matches("),
        )
        self.assertLess(
            clear.index("app_mesh_route_wait_tx_clear_matches("),
            clear.index("mesh_route_waiting_tx_valid = false"),
        )
        self.assertIn("semantic_digest", timeout)
        self.assertIn("APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC", timeout)
        digest = wait_commit.index("mesh_packet_semantic_digest(")
        activate = wait_commit.index(
            "gateway_pending_command_semantic_digest_valid = true"
        )
        self.assertLess(digest, activate)
        self.assertIn(
            "gateway_pending_command_semantic_digest", dispatch
        )
        side_effect = dispatch.index("gateway_command_timeout_side_effects(")
        clear_digest = dispatch.index(
            "gateway_pending_command_semantic_digest_valid = false"
        )
        self.assertLess(side_effect, clear_digest)

    def test_route_ready_deferred_peer_has_generation_owned_identity(self):
        self.assertTrue(
            "struct app_mesh_route_ready_event_owner" in REPORT,
            "route-ready deferred work needs a generation-bound owner type",
        )
        self.assertTrue(
            "mesh_route_ready_event_owner" in REPORT,
            "route-ready deferred work needs an owned instance",
        )
        self.assertNotIn("mesh_route_ready_event_peer_id", REPORT)

        owner_set = function_body(REPORT, "mesh_route_ready_event_owner_set")
        owner_match = function_body(
            REPORT, "mesh_route_ready_event_owner_matches"
        )
        ready = function_body(REPORT, "mesh_handle_result_actions")
        waiting = function_body(REPORT, "mesh_try_route_waiting_tx")

        self.assertIn("peer_id", owner_set)
        self.assertIn("generation", owner_set)
        self.assertIn("owner->valid = true", owner_set)
        self.assertIn("owner->peer_id == peer_id", owner_match)
        self.assertIn("owner->generation == generation", owner_match)
        self.assertRegex(
            ready,
            r"route_ready_generation\s*=\s*\(uint32_t\)atomic_inc\("
            r"&mesh_route_ready_generation\)\s*\+\s*1u",
        )
        self.assertIn("mesh_route_ready_event_owner_set(", ready)
        self.assertIn("route_ready_generation", ready)
        self.assertIn("mesh_route_ready_event_owner_matches(", waiting)
        self.assertIn("atomic_get(&mesh_route_ready_generation)", waiting)

    def test_busy_other_peer_preserves_exact_forwarded_ack_repair_edge(self):
        self.assertTrue(
            "mesh_deferred_forwarded_ack_event_repair_authorization" in REPORT,
            "busy event negotiation must retain the exact forwarded-ACK repair capability",
        )
        propose = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        defer = function_body(
            REPORT, "mesh_defer_forwarded_ack_event_repair"
        )
        retry = function_body(
            REPORT, "mesh_retry_deferred_forwarded_ack_event_repair"
        )
        worker = function_body(
            REPORT, "mesh_event_negotiation_retry_work_handler"
        )

        other_peer = propose.index(
            "mesh_event_propose_retry.peer_id != peer_id"
        )
        busy_return = propose.index("return -EBUSY", other_peer)
        self.assertIn(
            "mesh_defer_forwarded_ack_event_repair(",
            propose[other_peer:busy_return],
        )

        self.assertIn(
            "mesh_deferred_forwarded_ack_event_repair_authorization =",
            defer,
        )
        self.assertIn(
            "app_mesh_c5_tx_authorization_token_equal(", defer
        )
        occupied = defer.index(
            "mesh_deferred_forwarded_ack_event_repair_authorization.valid"
        )
        reject = defer.index("return false", occupied)
        publish = defer.index(
            "mesh_deferred_forwarded_ack_event_repair_authorization ="
        )
        self.assertLess(reject, publish)
        self.assertIn("mesh_event_negotiation_schedule_next()", defer)
        self.assertIn("app_mesh_ch9_c5_repair_owner_matches(", retry)
        self.assertIn(
            "mesh_propose_event_after_channel5_contact_authorized(", retry
        )
        self.assertIn("ret == -EBUSY", retry)
        self.assertNotIn(
            "memset(&mesh_deferred_forwarded_ack_event_repair_authorization",
            retry[: retry.index("ret == -EBUSY")],
        )
        self.assertIn("mesh_retry_deferred_forwarded_ack_event_repair(", worker)
        self.assertIn("mesh_event_negotiation_schedule_next()", worker)

    def test_late_gateway_ack_owner_survives_and_never_commits_absent_core(self):
        actions = function_body(REPORT, "mesh_handle_result_actions")
        queue = function_body(
            REPORT, "mesh_ch9_ack_batch_queue_forwarded_gateway_ack"
        )
        send = function_body(REPORT, "mesh_send_pending_ch9_ack_batch")
        discard = function_body(REPORT, "mesh_ch9_ack_batch_discard_if_safe")
        select = function_body(REPORT, "mesh_select_channel9_ack_tx_event")

        # Relay-core provenance is captured at the receive handoff, instead of
        # being inferred later from the preserved gateway-ACK wire type.
        queued = actions.index(
            "mesh_ch9_ack_batch_queue_forwarded_gateway_ack("
        )
        action = actions.index(
            "MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING", queued
        )
        self.assertLess(queued, action)
        self.assertIn("transit_core_owned", queue)
        self.assertIn("app_mesh_ch9_ack_table_queue_forwarded(", queue)
        self.assertIn("app_mesh_ch9_ack_table_queue_late_forwarded(", queue)

        # Only the transit class calls the relay-core commit. A late terminal
        # forward is cleared after the same proven physical send succeeds.
        self.assertIn("batch->owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE", send)
        commit_guard = send.index("if (transit_core_commit)")
        commit = send.index("mesh_commit_forwarded_gateway_ack_sent", commit_guard)
        clear = send.index("app_mesh_ch9_ack_table_clear_peer", commit)
        self.assertLess(commit_guard, commit)
        self.assertLess(commit, clear)

        # Connection teardown and timing expiry may discard generated ACKs,
        # but both exact forwarded-owner classes must survive for repair.
        self.assertIn("mesh_ch9_ack_batch_preserves_terminal_forward", discard)
        self.assertIn("mesh_ch9_ack_batch_requires_physical_commit", REPORT)
        self.assertIn("mesh_ch9_ack_batch_is_late_terminal_forward", REPORT)

        # Stale timing repair selects the owner-specific capability; the late
        # form is intentionally valid without a live mesh_runtime.pending.
        self.assertIn("late_terminal_forward", select)
        self.assertIn("APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR", select)
        self.assertIn("app_mesh_ch9_c5_repair_authorization_capture", select)


if __name__ == "__main__":
    unittest.main()
