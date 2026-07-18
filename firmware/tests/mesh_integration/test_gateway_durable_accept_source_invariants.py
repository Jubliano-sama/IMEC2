#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP = read_composed_source(ROOT / "app/src/app_mesh_report.c")
GATEWAY = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")
RELAY = read_composed_source(ROOT / "src/mesh_relay.c")
PERSISTENCE = (ROOT / "app/src/app_mesh_persistence.c").read_text(
    encoding="utf-8"
)
JOURNAL = (ROOT / "src/gateway_collection_journal.c").read_text(encoding="utf-8")


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


class GatewayDurableAcceptSourceInvariantTests(unittest.TestCase):
    def test_assignment_claims_and_acks_do_not_depend_on_ble_stream_custody(self):
        classifier = function_body(
            APP, "mesh_gateway_delivery_is_internal_control"
        )
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        self.assertIn("MSG_COMMAND_RESULT", classifier)
        self.assertIn("CMD_ASSIGN_DISCOVERY_SLOTS", classifier)
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_CLAIM", classifier)
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_ACK", classifier)
        self.assertIn("reservation_ret = internal_control ? 1", drain)
        self.assertIn("!internal_control", drain)

    def test_semantic_acceptance_precedes_duplicate_commit_and_ack_actions(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        reserve = drain.index("gateway_ble_reserve_stream_packet")
        semantic = drain.index("mesh_gateway_accept_semantic_delivery")
        rejected = drain.index("if (semantic_ret < 0)", semantic)
        stream = drain.index("gateway_ble_commit_stream_reservation", rejected)
        finalize = drain.index("gateway_finalize_semantic_delivery", stream)
        commit = drain.index("mesh_relay_commit_gateway_delivery", finalize)
        actions = drain.index("mesh_handle_result_actions", commit)

        self.assertLess(reserve, semantic)
        self.assertLess(semantic, rejected)
        self.assertLess(rejected, stream)
        self.assertLess(stream, finalize)
        self.assertLess(finalize, commit)
        self.assertLess(stream, actions)
        self.assertLess(commit, actions)
        self.assertIn("gateway_ble_cancel_stream_reservation", drain)
        self.assertIn("deferred for BLE custody", drain[reserve:semantic])
        self.assertIn("rejected without ACK", drain[semantic:actions])

    def test_all_semantic_gateway_types_share_the_commit_gate(self):
        app_gate = function_body(
            APP, "mesh_gateway_delivery_requires_semantic_acceptance"
        )
        relay_gate = function_body(RELAY, "gateway_delivery_requires_commit")

        for message_type in (
            "MSG_COMMAND_RESULT",
            "MSG_RESULT_BUNDLE",
            "MSG_SURVEY_DISCOVERY_REPORT",
            "MSG_SURVEY_PAIR_RESULT",
        ):
            with self.subTest(message_type=message_type):
                self.assertIn(message_type, app_gate)
                self.assertIn(message_type, relay_gate)

    def test_collection_handler_reports_durable_acceptance_to_the_gate(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        result = function_body(real_gateway, "gateway_note_command_result")
        bundle = function_body(real_gateway, "gateway_note_command_result_bundle")
        accept = function_body(GATEWAY, "gateway_accept_collection_record")
        finalize = function_body(
            real_gateway, "gateway_finalize_semantic_delivery"
        )

        self.assertIn("if (collection_ret >= 0)", result)
        self.assertIn("return collection_ret;", result)
        self.assertIn("return gateway_note_collection_bundle", bundle)
        persist = accept.index("gateway_persist_collection_state")
        rollback = accept.index(
            "app_mesh_persistence_rollback_gateway_collection", persist
        )
        accepted = accept.rindex("return 0;")
        self.assertLess(persist, accepted)
        self.assertLess(persist, rollback)
        self.assertLess(rollback, accepted)
        self.assertIn("gateway_drive_collection_eack_after_record", finalize)

    def test_commit_builds_ack_before_storing_duplicate_identity(self):
        commit = function_body(RELAY, "mesh_relay_commit_gateway_delivery")
        build = commit.index("build_gateway_ack")
        store = commit.index("duplicate_store", build)
        expose = commit.index("MESH_RELAY_ACTION_SEND_GATEWAY_ACK", store)

        self.assertLess(build, store)
        self.assertLess(store, expose)

    def test_rejected_command_payload_keeps_pending_transaction_untouched(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        result = function_body(real_gateway, "gateway_note_command_result")
        deliver = result.index("app_mesh_command_orchestrator_gateway_deliver")
        success = result.index("if (ret == PROTO_OK)", deliver)
        clear = result.index("gateway_command_pending_clear", success)

        self.assertLess(deliver, success)
        self.assertLess(success, clear)
        self.assertNotIn("gateway_command_result_side_effects", result)

    def test_collection_state_owns_the_exact_resolved_roster(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        begin = function_body(real_gateway, "gateway_begin_command_collection")
        send_eack = function_body(real_gateway, "gateway_send_collection_eack")

        self.assertNotIn("gateway_collection_expected_node_ids", real_gateway)
        resolve = begin.index("gateway_command_resolve_collection_roster")
        start = begin.index("gateway_collection_start", resolve)
        bind = begin.index("gateway_collection_set_expected_roster", start)
        persist = begin.index("gateway_persist_collection_state", bind)
        self.assertLess(resolve, start)
        self.assertLess(start, bind)
        self.assertLess(bind, persist)
        self.assertIn("gateway_collection_state.expected_node_ids", send_eack)
        self.assertIn("gateway_collection_state.expected_node_id_count", send_eack)

    def test_collection_start_is_durable_before_custody_or_rf_scheduling(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        begin = function_body(real_gateway, "gateway_begin_command_collection")

        persist = begin.index("gateway_persist_collection_state")
        rollback = begin.index("gateway_collection_rollback_failed_start", persist)
        reset = begin.index("app_gateway_eack_retry_reset", rollback)
        clear = begin.index("app_mesh_persistence_clear_gateway_eack_custody", reset)
        schedule = begin.index("gateway_schedule_collection_eack_round", clear)
        self.assertLess(persist, rollback)
        self.assertLess(rollback, reset)
        self.assertLess(reset, clear)
        self.assertLess(clear, schedule)

    def test_closed_collection_custody_blocks_all_new_command_tracking(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        pending = function_body(real_gateway, "gateway_collection_work_pending")
        begin = function_body(real_gateway, "gateway_begin_command_collection")
        wait = function_body(real_gateway, "gateway_begin_command_result_wait_for")

        for state in (
            "gateway_membership_restore_pending",
            "gateway_collection_restore_pending",
            "gateway_membership_persistence_dirty",
            "collection_open",
            "eack_pending",
            "gateway_collection_eack_retry_state.active",
            "gateway_collection_eack_round_dirty",
            "gateway_collection_persistence_dirty",
            "gateway_collection_clear_pending",
        ):
            with self.subTest(state=state):
                self.assertIn(state, pending)
        self.assertIn("gateway_collection_work_pending()", begin)
        self.assertIn("gateway_collection_work_pending()", wait)

    def test_transient_journal_restore_blocks_and_retries_command_work(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        retry = function_body(
            real_gateway, "gateway_persistence_retry_work_handler"
        )
        persist = function_body(real_gateway, "gateway_persist_collection_state")
        restore = function_body(
            real_gateway, "gateway_restore_collection_runtime"
        )
        init = function_body(
            real_gateway, "gateway_command_result_tracking_init"
        )

        restore_gate = retry.index("if (gateway_collection_restore_pending)")
        reload = retry.index("gateway_restore_collection_runtime", restore_gate)
        save = retry.index("app_mesh_persistence_save_gateway_collection", reload)
        self.assertLess(restore_gate, reload)
        self.assertLess(reload, save)
        self.assertIn("gateway_collection_restore_pending", persist)
        self.assertIn("gateway_collection_restore_pending = true", restore)
        self.assertIn("gateway_collection_restore_pending = false", restore)
        init_reload = init.index("gateway_restore_collection_runtime")
        self.assertLess(
            init_reload,
            init.index("gateway_schedule_persistence_retry", init_reload),
        )

    def test_transient_membership_restore_precedes_collection_reload(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        retry = function_body(
            real_gateway, "gateway_persistence_retry_work_handler"
        )
        init = function_body(
            real_gateway, "gateway_command_result_tracking_init"
        )
        set_roster = function_body(
            real_gateway, "gateway_set_registered_membership_roster"
        )

        membership = retry.index("if (gateway_membership_restore_pending)")
        membership_restore = retry.index(
            "gateway_restore_membership_runtime", membership
        )
        collection = retry.index("if (gateway_collection_restore_pending)")
        collection_restore = retry.index(
            "gateway_restore_collection_runtime", collection
        )
        self.assertLess(membership, membership_restore)
        self.assertLess(membership_restore, collection)
        self.assertLess(collection, collection_restore)
        self.assertLess(
            init.index("gateway_restore_membership_runtime"),
            init.index("gateway_restore_collection_runtime"),
        )
        self.assertIn("gateway_membership_restore_pending", set_roster)
        self.assertIn("gateway_collection_restore_pending", set_roster)
        self.assertIn("gateway_membership_persistence_dirty", set_roster)

    def test_failed_start_restores_the_previous_durable_snapshot(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        rollback = function_body(
            real_gateway, "gateway_collection_rollback_failed_start"
        )

        clear_dirty = rollback.index("gateway_collection_persistence_dirty = false")
        restore = rollback.index(
            "app_mesh_persistence_restore_gateway_collection", clear_dirty
        )
        fallback_clear = rollback.index("gateway_collection_clear", restore)
        block = rollback.index(
            "gateway_collection_restore_pending = true", fallback_clear
        )
        retry = rollback.index("gateway_schedule_persistence_retry", block)
        self.assertLess(clear_dirty, restore)
        self.assertLess(restore, fallback_clear)
        self.assertLess(fallback_clear, block)
        self.assertLess(block, retry)

    def test_gateway_collection_persistence_uses_compact_journal_records(self):
        save = function_body(
            PERSISTENCE, "app_mesh_persistence_save_gateway_collection"
        )
        restore = function_body(
            PERSISTENCE, "app_mesh_persistence_restore_gateway_collection"
        )

        self.assertNotIn("gateway_collection_state_snapshot", save)
        self.assertNotIn("gateway_collection_state_snapshot", restore)
        self.assertIn("gateway_collection_state_validate(collection)", save)
        self.assertIn("gateway_collection_journal_save", save)
        self.assertIn("gateway_collection_journal_restore", restore)
        self.assertNotIn("sizeof(*collection)", save)
        self.assertIn("encode_result", JOURNAL)
        self.assertIn("encode_control", JOURNAL)
        self.assertIn("committed_slots", JOURNAL)
        self.assertIn("decode_result", JOURNAL)

    def test_collection_clear_waits_for_a_durable_tombstone(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        clear = function_body(real_gateway, "gateway_clear_command_collection")
        retry = function_body(
            real_gateway, "gateway_persistence_retry_work_handler"
        )
        finalize = function_body(
            real_gateway, "gateway_finalize_collection_clear"
        )

        tombstone = clear.index("app_mesh_persistence_clear_gateway_collection")
        failed = clear.index("if (ret < 0)", tombstone)
        pending = clear.index("gateway_collection_clear_pending = true", failed)
        commit = clear.index("gateway_finalize_collection_clear", pending)
        self.assertLess(tombstone, failed)
        self.assertLess(failed, pending)
        self.assertLess(pending, commit)
        self.assertIn("if (gateway_collection_clear_pending)", retry)
        self.assertLess(
            retry.index("app_mesh_persistence_clear_gateway_collection"),
            retry.index("gateway_finalize_collection_clear"),
        )
        self.assertIn("gateway_collection_clear(&gateway_collection_state)", finalize)

    def test_membership_clear_waits_for_durable_delete(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        clear = function_body(
            real_gateway, "gateway_clear_registered_membership_roster"
        )
        retry = function_body(
            real_gateway, "gateway_persistence_retry_work_handler"
        )
        finalize = function_body(
            real_gateway, "gateway_finalize_membership_clear"
        )
        pending = function_body(real_gateway, "gateway_collection_work_pending")
        set_roster = function_body(
            real_gateway, "gateway_set_registered_membership_roster"
        )

        durable_clear = clear.index("app_mesh_persistence_clear_gateway_membership")
        failed = clear.index("if (ret < 0)", durable_clear)
        defer = clear.index("gateway_membership_clear_pending = true", failed)
        commit = clear.index("gateway_finalize_membership_clear", defer)
        self.assertLess(durable_clear, failed)
        self.assertLess(failed, defer)
        self.assertLess(defer, commit)
        self.assertNotIn("gateway_membership_clear(", clear)
        self.assertIn("if (gateway_membership_clear_pending)", retry)
        self.assertLess(
            retry.index("app_mesh_persistence_clear_gateway_membership"),
            retry.index("gateway_finalize_membership_clear"),
        )
        self.assertIn(
            "gateway_membership_clear(&gateway_membership_roster_state)", finalize
        )
        self.assertIn("gateway_membership_clear_pending", pending)
        self.assertIn("gateway_membership_clear_pending", set_roster)


if __name__ == "__main__":
    unittest.main()
