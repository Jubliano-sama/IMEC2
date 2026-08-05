#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
ANCHOR = read_composed_source(ROOT / "app" / "src" / "app_anchor.c")
RELAY = read_composed_source(ROOT / "src" / "mesh_relay.c")
GATEWAY_BLE = read_composed_source(ROOT / "app" / "src" / "app_gateway_ble.c")
PERSISTENCE_H = (ROOT / "app" / "src" / "app_mesh_persistence.h").read_text()
PERSISTENCE_C = (ROOT / "app" / "src" / "app_mesh_persistence.c").read_text()
STREAM_C = (ROOT / "app" / "src" / "app_gateway_ble_stream.c").read_text()
ARBITRATION = (
    ROOT / "app" / "src" / "app_mesh_arbitration_zephyr.c"
).read_text()


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


def last_function_body(source: str, name: str) -> str:
    matches = list(re.finditer(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL))
    if not matches:
        raise AssertionError(f"function not found: {name}")
    start = matches[-1].end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


class ClickBleCustodySourceInvariantTests(unittest.TestCase):
    def test_gateway_host_lifecycle_is_serialized_across_workqueues(self):
        wrappers = (
            "gateway_host_command_lifecycle_admit_locked",
            "gateway_host_command_lifecycle_cancel_locked",
            "gateway_host_command_lifecycle_begin_locked",
            "gateway_host_command_lifecycle_requeue_locked",
            "gateway_host_command_lifecycle_finish_locked",
            "gateway_host_command_lifecycle_discard_locked",
        )

        for wrapper in wrappers:
            with self.subTest(wrapper=wrapper):
                body = function_body(ANCHOR, wrapper)
                self.assertIn(
                    "k_spin_lock(&gateway_host_command_lifecycle_lock)",
                    body,
                )
                self.assertIn(
                    "k_spin_unlock(&gateway_host_command_lifecycle_lock",
                    body,
                )
        for operation in (
            "admit",
            "cancel",
            "begin_dispatch",
            "requeue_retry",
            "finish",
            "discard",
        ):
            self.assertEqual(
                ANCHOR.count(f"app_gateway_command_lifecycle_{operation}("),
                1,
                f"direct lifecycle {operation} call bypasses the spinlock wrapper",
            )

    def test_preemptive_abort_acceptance_precedes_queue_custody(self):
        submit = function_body(
            ANCHOR, "gateway_host_command_submit_preemptive"
        )
        preflight = submit.index(
            "k_msgq_num_free_get(&gateway_host_abort_msgq)"
        )
        acceptance = submit.index("gateway_observe_host_acceptance", preflight)
        custody = submit.index(
            "k_msgq_put(&gateway_host_abort_msgq", acceptance
        )
        scheduling = submit.index("k_work_reschedule", custody)

        self.assertLess(preflight, acceptance)
        self.assertLess(acceptance, custody)
        self.assertLess(custody, scheduling)
        self.assertNotIn(
            "acceptance telemetry failed after custody",
            submit,
        )

    def test_preemptive_abort_route_worker_does_not_duplicate_terminal(self):
        route = function_body(
            ANCHOR, "gateway_host_abort_route_work_handler"
        )

        self.assertEqual(route.count("gateway_handle_local_survey_abort("), 1)
        self.assertNotIn("gateway_emit_host_command_result_reserved(", route)
        self.assertNotIn("gateway_observe_host_terminal(", route)

    def test_stale_notify_failure_cannot_charge_a_new_link_generation(self):
        worker = function_body(GATEWAY_BLE, "gateway_ble_stream_work_handler")
        failed_submit = worker.index("if (ret < 0)")
        generation_guard = worker.index(
            "generation == gateway_ble_tx_generation",
            failed_submit,
        )
        increment = worker.index(
            "gateway_ble_notify_failure_count++",
            generation_guard,
        )
        stale_return = worker.index("if (!attempt_current)", increment)
        stream_inspection = worker.index(
            "k_spin_lock(&gateway_ble_stream_lock)",
            stale_return,
        )

        self.assertLess(generation_guard, increment)
        self.assertLess(increment, stale_return)
        self.assertLess(stale_return, stream_inspection)
        self.assertIn("bt_conn_unref(conn);", worker[stale_return:stream_inspection])
        self.assertIn("return;", worker[stale_return:stream_inspection])

    def test_gateway_priority_boundary_handoff_is_serialized(self):
        submit = function_body(
            ARBITRATION,
            "app_mesh_arbitration_zephyr_gateway_command_submit",
        )
        boundary = function_body(
            ARBITRATION,
            "app_mesh_arbitration_zephyr_gateway_receive_abort_observed",
        )

        submit_lock = submit.index("k_spin_lock(&gateway_priority_lock)")
        candidate_ops = submit.index("candidate_ops = *ops", submit_lock)
        publish_wait = submit.index(
            "app_mesh_gateway_command_priority_request", candidate_ops
        )
        publish_ops = submit.index(
            "gateway_ops = candidate_ops", publish_wait
        )
        submit_unlock = submit.index(
            "k_spin_unlock(&gateway_priority_lock", publish_ops
        )
        self.assertLess(submit_lock, candidate_ops)
        self.assertLess(candidate_ops, publish_wait)
        self.assertLess(publish_wait, publish_ops)
        self.assertLess(publish_ops, submit_unlock)

        boundary_lock = boundary.index(
            "k_spin_lock(&gateway_priority_lock)"
        )
        observe_wait = boundary.index(
            "app_mesh_gateway_command_priority_waiting_for_safe_boundary",
            boundary_lock,
        )
        consume_wait = boundary.index(
            "app_mesh_gateway_command_priority_acknowledge_safe_boundary",
            observe_wait,
        )
        boundary_unlock = boundary.index(
            "k_spin_unlock(&gateway_priority_lock", consume_wait
        )
        failure_callback = boundary.index(
            "gateway_priority_notify_failure(failure_handler",
            boundary_unlock,
        )
        notify = function_body(ARBITRATION, "gateway_priority_notify_failure")
        self.assertLess(boundary_lock, observe_wait)
        self.assertLess(observe_wait, consume_wait)
        self.assertLess(consume_wait, boundary_unlock)
        self.assertLess(boundary_unlock, failure_callback)
        self.assertIn("handler(ctx,", notify)
        self.assertIn("failure->generation", notify)
        self.assertIn("failure->admission_cutoff", notify)

    def test_click_reports_use_the_gateway_ble_custody_gate(self):
        app_gate = function_body(
            APP, "mesh_gateway_delivery_requires_semantic_acceptance"
        )
        relay_gate = function_body(RELAY, "gateway_delivery_requires_commit")

        for gate in (app_gate, relay_gate):
            self.assertIn("MSG_CLICK_REPORT", gate)
            self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", gate)
            self.assertIn("packet->dst_id", gate)

    def test_reserve_commit_and_ack_order_is_single_path(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        reserve = drain.index("gateway_ble_reserve_stream_packet")
        semantic = drain.index("mesh_gateway_accept_semantic_delivery", reserve)
        new_accept = drain.index("APP_GATEWAY_SEMANTIC_ACCEPT_NEW", semantic)
        stream_commit = drain.index("gateway_ble_commit_stream_reservation", new_accept)
        relay_commit = drain.index("mesh_relay_commit_gateway_delivery", stream_commit)
        ack_actions = drain.index("mesh_handle_result_actions", relay_commit)

        self.assertLess(reserve, semantic)
        self.assertLess(semantic, new_accept)
        self.assertLess(new_accept, stream_commit)
        self.assertLess(stream_commit, relay_commit)
        self.assertLess(relay_commit, ack_actions)
        self.assertEqual(drain.count("gateway_ble_commit_stream_reservation"), 1)
        self.assertIn("gateway_semantic_delivery_processed = true", drain)

    def test_host_journal_commit_precedes_ble_commit_and_gateway_ack(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        journal_commit = drain.index(
            "app_mesh_persistence_commit_gateway_host_journal"
        )
        stream_commit = drain.index("gateway_ble_commit_stream_reservation")
        relay_commit = drain.index("mesh_relay_commit_gateway_delivery")

        self.assertLess(journal_commit, stream_commit)
        self.assertLess(stream_commit, relay_commit)

    def test_all_host_visible_types_reuse_pending_journal_identity(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        gate = function_body(
            APP, "mesh_gateway_delivery_requires_semantic_acceptance"
        )
        supports = function_body(
            PERSISTENCE_C,
            "app_mesh_persistence_gateway_host_journal_supports",
        )
        journal_match = drain.index(
            "app_mesh_persistence_gateway_host_journal_matches"
        )
        duplicate = drain.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", journal_match
        )

        for message_type in (
            "MSG_CLICK_REPORT",
            "MSG_COMMAND_RESULT",
            "MSG_RESULT_BUNDLE",
            "MSG_SURVEY_DISCOVERY_REPORT",
            "MSG_SURVEY_PAIR_RESULT",
        ):
            with self.subTest(message_type=message_type):
                self.assertIn(message_type, gate)
                self.assertIn(message_type, supports)
        self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", supports)
        self.assertLess(journal_match, duplicate)

    def test_host_journal_reuses_role_disjoint_nvs_keys_and_capacity_model(self):
        self.assertIn(
            "#define APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID "
            "APP_MESH_NVS_COLLECTION_RESULT_ID",
            PERSISTENCE_C,
        )
        self.assertIn(
            "#define APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID "
            "APP_MESH_NVS_LOCAL_DELIVERY_ID",
            PERSISTENCE_C,
        )
        self.assertIn(
            "APP_MESH_NVS_GATEWAY_JOURNAL_LIVE_BYTES +\n"
            "             APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_LIVE_BYTES +\n"
            "             APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_LIVE_BYTES +\n"
            "             APP_MESH_NVS_OTHER_LIVE_BYTES <=\n"
            "             APP_MESH_NVS_MINIMUM_USABLE_BYTES",
            PERSISTENCE_C,
        )
        self.assertIn(
            "sizeof(struct app_mesh_gateway_click_journal_metadata) <\n"
            "             (APP_MESH_NVS_SECTOR_SIZE / 2u)",
            PERSISTENCE_C,
        )
        self.assertIn(
            "sizeof(struct app_mesh_collection_result_record)) >=\n"
            "    APP_MESH_NVS_ENTRY_BYTES(\n"
            "        sizeof(struct app_mesh_gateway_click_journal_metadata))",
            PERSISTENCE_C,
        )
        self.assertIn(
            "sizeof(struct app_mesh_local_delivery_snapshot)) >=\n"
            "    APP_MESH_NVS_ENTRY_BYTES(\n"
            "        APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN)",
            PERSISTENCE_C,
        )

    def test_host_journal_adds_no_full_payload_static_buffer(self):
        restore = function_body(
            GATEWAY_BLE, "gateway_restore_host_journal_runtime"
        )

        self.assertIn("gateway_ble_stream_state.record_pool[staging_offset]", restore)
        self.assertIn("struct proto_packet staging_packet = {0};", restore)
        self.assertIn("struct proto_packet *packet = &staging_packet;", restore)
        self.assertNotIn("packet = &gateway_ble_stream_state.items", restore)
        self.assertNotRegex(
            restore,
            r"(?:static\s+)?uint8_t\s+\w+\s*\[\s*"
            r"(?:PACKET_EXT_MAX_PAYLOAD_LEN|GATEWAY_BLE_STREAM_RECORD_POOL_BYTES)",
        )

    def test_host_journal_schema_remains_click_journal_compatible(self):
        self.assertIn(
            "#define APP_MESH_GATEWAY_CLICK_JOURNAL_VERSION \\\n"
            "    APP_MESH_GATEWAY_HOST_JOURNAL_VERSION",
            PERSISTENCE_H,
        )
        self.assertIn(
            "#define APP_MESH_GATEWAY_CLICK_JOURNAL_MAGIC \\\n"
            "    APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC",
            PERSISTENCE_H,
        )
        self.assertIn(
            "struct app_mesh_gateway_click_journal_metadata",
            PERSISTENCE_H,
        )

    def test_gateway_click_journal_identity_excludes_mutable_transport_fields(self):
        identity = function_body(PERSISTENCE_C, "gateway_click_packet_matches")

        self.assertIn("left->flags == right->flags", identity)
        self.assertIn("left->src_id == right->src_id", identity)
        self.assertIn("left->dst_id == right->dst_id", identity)
        self.assertIn("left->session_id == right->session_id", identity)
        self.assertIn("left->seq == right->seq", identity)
        self.assertIn("left->payload_len == right->payload_len", identity)
        self.assertNotIn("left->ttl", identity)
        self.assertNotIn("left->message_age_ms", identity)

    def test_gateway_ble_defers_host_journal_flash_after_notify_completion(self):
        tx_complete = function_body(GATEWAY_BLE, "gateway_ble_tx_complete")
        retire = function_body(
            GATEWAY_BLE, "gateway_retire_notified_host_journal_runtime"
        )
        identity = tx_complete.index("gateway_ble_stream_head_journal_identity")
        notify = tx_complete.index(
            "GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED"
        )
        schedule = tx_complete.index(
            "gateway_ble_require_host_journal_restore", notify
        )
        durable_mark = retire.index(
            "app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest"
        )
        exact_retire = retire.index(
            "gateway_ble_stream_mark_sent", durable_mark
        )

        self.assertLess(identity, notify)
        self.assertLess(notify, schedule)
        self.assertLess(durable_mark, exact_retire)
        self.assertIn(
            "gateway_ble_stream_state.head_send_phase !=",
            retire[durable_mark:exact_retire],
        )
        self.assertNotIn(
            "app_mesh_persistence_mark_gateway_host_journal_notified",
            tx_complete,
        )
        self.assertNotIn("gateway_ble_schedule_stream_drain", retire)
        self.assertIn(
            "app_mesh_persistence_gateway_host_journal_supports",
            tx_complete,
        )

    def test_uncertain_notification_marker_reconciles_durable_phase(self):
        tx_complete = function_body(GATEWAY_BLE, "gateway_ble_tx_complete")
        retry = function_body(
            GATEWAY_BLE, "gateway_persistence_retry_work_handler"
        )
        retire = function_body(
            GATEWAY_BLE, "gateway_retire_notified_host_journal_runtime"
        )

        self.assertIn(
            "gateway journal identity missing",
            tx_complete,
        )
        self.assertIn(
            "marker_ret == -EAGAIN || marker_ret == -EBUSY",
            retire,
        )
        self.assertIn(
            "atomic_set(&gateway_host_journal_restore_pending, 1)",
            retire,
        )
        self.assertLess(
            retry.index("gateway_retire_notified_host_journal_runtime"),
            retry.index("gateway_restore_host_journal_runtime"),
        )
        restore = retry.index("gateway_restore_host_journal_runtime")
        self.assertLess(
            restore,
            retry.index("gateway_ble_schedule_stream_drain", restore),
        )
        self.assertNotIn(
            "app_mesh_persistence_clear_gateway_host_journal()",
            retry,
        )

    def test_gateway_ble_startup_restores_host_journal_before_transport_start(self):
        init = function_body(GATEWAY_BLE, "gateway_ble_init")
        stream_init = init.index("gateway_ble_stream_init")
        restore = init.index("gateway_restore_host_journal_runtime")
        bt_enable = init.index("bt_enable")

        self.assertLess(stream_init, restore)
        self.assertLess(restore, bt_enable)

    def test_host_journal_restore_can_evict_best_effort_stream_entries(self):
        restore = function_body(GATEWAY_BLE, "gateway_restore_host_journal_runtime")

        self.assertNotIn(
            "gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH",
            restore,
        )
        self.assertIn("gateway_ble_stream_state.restore_staging_active", restore)
        self.assertIn("gateway_ble_stream_enqueue_staged_packet", restore)
        self.assertIn("gateway_ble_stream_enqueue_staged_bundle_projection", restore)
        self.assertGreaterEqual(restore.count("k_uptime_get_32()"), 2)
        self.assertGreaterEqual(restore.count("true"), 2)

    def test_host_reservation_blocks_occupied_journal_before_stream_reservation(self):
        reserve = last_function_body(
            GATEWAY_BLE, "gateway_ble_reserve_stream_packet"
        )
        occupancy = reserve.index(
            "app_mesh_persistence_gateway_host_journal_matches"
        )
        stream_reserve = reserve.index("gateway_ble_stream_reserve_packet")

        self.assertIn("gateway_host_journal_restore_pending", reserve)
        self.assertIn(
            "app_mesh_persistence_gateway_host_journal_supports", reserve
        )
        self.assertLess(occupancy, stream_reserve)

    def test_gateway_host_journal_accepts_extended_payloads(self):
        self.assertRegex(
            PERSISTENCE_H,
            r"#define APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN\s+\\\s*"
            r"PACKET_EXT_MAX_PAYLOAD_LEN",
        )
        restore = function_body(
            GATEWAY_BLE, "gateway_restore_host_journal_runtime"
        )
        self.assertIn("PACKET_EXT_MAX_PAYLOAD_LEN", restore)
        self.assertIn("gateway_ble_stream_enqueue_staged_packet", restore)
        self.assertNotIn("PACKET_MAX_PAYLOAD_LEN", restore)

    def test_extended_restore_moves_overlapping_staging_before_header_build(self):
        staged = function_body(
            STREAM_C, "enqueue_staged_packet_with_journal_digest"
        )
        self.assertIn("memmove(destination_payload", staged)
        self.assertIn("memmove(&record[offset]", STREAM_C)
        self.assertIn("journal_payload_digest", staged)
        self.assertIn("gateway_click_clear_locked", PERSISTENCE_C)

    def test_corrupt_host_journal_is_retained_and_blocks_admission(self):
        read_payload = function_body(PERSISTENCE_C, "gateway_click_read_payload")
        persistence_restore = function_body(
            PERSISTENCE_C,
            "app_mesh_persistence_restore_gateway_host_journal_projection",
        )
        matches = function_body(
            PERSISTENCE_C,
            "app_mesh_persistence_gateway_host_journal_matches_with_projection",
        )
        prepare = function_body(
            PERSISTENCE_C,
            "app_mesh_persistence_prepare_gateway_host_journal_projection",
        )
        runtime_restore = function_body(
            GATEWAY_BLE, "gateway_restore_host_journal_runtime"
        )
        reserve = last_function_body(
            GATEWAY_BLE, "gateway_ble_reserve_stream_packet"
        )

        self.assertIn("read_len == -ENOENT", read_payload)
        self.assertIn("return -EBADMSG", read_payload)
        self.assertNotIn("gateway_click_clear_locked", persistence_restore)
        self.assertNotIn("gateway_click_clear_locked", matches)
        self.assertNotIn("gateway_click_clear_locked", prepare)
        self.assertNotIn("ret == 0 || ret == -EBADMSG", runtime_restore)
        self.assertIn(
            "atomic_set(&gateway_host_journal_restore_pending, 1)",
            runtime_restore,
        )
        self.assertIn("journal_ret == -EBADMSG", reserve)
        self.assertIn("gateway_ble_require_host_journal_restore", reserve)

    def test_click_clear_retires_payload_before_marker(self):
        clear = function_body(PERSISTENCE_C, "gateway_click_clear_locked")
        payload = clear.index("APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID")
        metadata = clear.index("APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID")
        self.assertLess(payload, metadata)

    def test_malformed_click_is_rejected_before_semantic_acceptance(self):
        accept = function_body(APP, "mesh_gateway_accept_semantic_delivery")
        click_case = accept.index("case MSG_CLICK_REPORT:")
        validate = accept.index("report_validate_click_payload", click_case)
        accepted = accept.index("APP_GATEWAY_SEMANTIC_ACCEPT_NEW", validate)

        self.assertLess(click_case, validate)
        self.assertLess(validate, accepted)
        self.assertIn("return -EINVAL", accept[validate:accepted])

    def test_duplicate_and_rejected_clicks_cancel_reserved_host_custody(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        rejected = drain.index("if (semantic_ret < 0)")
        first_cancel = drain.index("gateway_ble_cancel_stream_reservation", rejected)
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", drain)
        duplicate = drain.index("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", rejected)
        duplicate_cancel = drain.index(
            "gateway_ble_cancel_stream_reservation", duplicate
        )

        self.assertLess(rejected, first_cancel)
        self.assertLess(duplicate, duplicate_cancel)
        self.assertGreaterEqual(drain.count("gateway_ble_cancel_stream_reservation"), 2)

    def test_custody_gated_clicks_do_not_fall_through_best_effort_streaming(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        stream = drain.index("gateway_ble_stream_packet")
        processed = drain.index("gateway_semantic_delivery_processed = true")
        best_effort_guard = drain.rfind("!gateway_semantic_delivery_processed", 0, stream)

        self.assertLess(processed, stream)
        self.assertGreaterEqual(best_effort_guard, processed)
        self.assertIn("gateway_ble_stream_packet", drain[best_effort_guard:])

    def test_click_preempted_outbox_resumes_in_the_same_boot(self):
        timeout = function_body(APP, "mesh_tx_timeout_handler")

        restore = timeout.index("app_mesh_persistence_restore_outbox")
        tick = timeout.index("mesh_relay_tick_with_random")
        self.assertLess(restore, tick)
        self.assertIn("!mesh_relay_tx_active(&mesh_runtime)", timeout[:restore])

    def test_paused_click_window_rearms_deferred_outbox_work(self):
        timeout = function_body(APP, "mesh_tx_timeout_handler")
        paused = timeout.index("if (mesh_transport_paused())")
        rearm = timeout.index("mesh_reschedule_owned_work", paused)
        early_return = timeout.index("return;", paused)

        self.assertLess(paused, rearm)
        self.assertLess(rearm, early_return)
        self.assertIn("REPORT_TX_RETRY_DELAY_MS", timeout[rearm:early_return])


if __name__ == "__main__":
    unittest.main()
