#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP = read_composed_source(ROOT / "app/src/app_mesh_report.c")
RX = (ROOT / "app/src/app_mesh_report_rx.inc").read_text(encoding="utf-8")
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
GATEWAY = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
RELAY = read_composed_source(ROOT / "src/mesh_relay.c")
PERSISTENCE = (ROOT / "app/src/app_mesh_persistence.c").read_text(
    encoding="utf-8"
)
JOURNAL = (ROOT / "src/gateway_collection_journal.c").read_text(encoding="utf-8")
RECEIPTS = (
    ROOT / "app/src/app_gateway_collection_receipts.c"
).read_text(encoding="utf-8")
INGRESS_H = (ROOT / "app/src/app_gateway_command_ingress.h").read_text(
    encoding="utf-8"
)
INGRESS = (ROOT / "app/src/app_gateway_command_ingress.c").read_text(
    encoding="utf-8"
)
SURVEY_H = (ROOT / "include/survey.h").read_text(encoding="utf-8")
SURVEY_ROUND = (
    ROOT / "app/src/app_anchor_gateway_survey_round.inc"
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


def ble_write_admission_model(
    free_slots: int,
    data: bytes,
    write_command: bool,
) -> tuple[bool, str]:
    complete_frames = data.count(b"\x00")
    if complete_frames > free_slots:
        return False, "disconnect" if write_command else "att-error"
    return True, "accepted"


class GatewayDurableAcceptSourceInvariantTests(unittest.TestCase):
    def test_ble_write_backpressure_is_observable_and_propagates_to_att(self):
        capacity = function_body(GATEWAY, "gateway_ble_rx_write_capacity")
        queue = function_body(GATEWAY, "gateway_ble_queue_frame")
        receive = function_body(GATEWAY, "gateway_ble_rx_bytes")
        write = function_body(GATEWAY, "gateway_ble_packet_rx_write")

        self.assertIn("completed_frames", capacity)
        self.assertIn("SERIAL_FRAME_DELIMITER", capacity)
        self.assertIn("k_msgq_num_free_get(&gateway_ble_rx_msgq)", capacity)
        self.assertIn("return completed_frames <=", capacity)
        self.assertIn("ret = k_msgq_put(&gateway_ble_rx_msgq", queue)
        self.assertIn("return ret < 0 ? -ENOSPC : 0;", queue)
        self.assertIn("int first_error = 0;", receive)
        self.assertIn("int ret = gateway_ble_queue_frame();", receive)
        self.assertIn("first_error = ret;", receive)
        self.assertIn("return first_error;", receive)
        preflight = write.index("gateway_ble_rx_write_capacity(buf, len)")
        consume = write.index("gateway_ble_rx_bytes(buf, len)", preflight)
        failure = write.index("if (ret < 0)")
        command_only = write.index("if (write_command && conn != NULL)", failure)
        disconnect = write.index("bt_conn_disconnect(", command_only)
        att_error = write.index("BT_ATT_ERR_INSUFFICIENT_RESOURCES", disconnect)
        self.assertIn("BT_GATT_WRITE_FLAG_CMD", write[:failure])
        self.assertLess(preflight, consume)
        self.assertLess(consume, failure)
        self.assertLess(failure, command_only)
        self.assertLess(command_only, disconnect)
        self.assertLess(disconnect, att_error)
        self.assertIn("return len;", write[failure:])

    def test_ble_full_queue_request_errors_and_command_disconnects_atomically(self):
        complete_frame = b"\x02\x11\x00"

        self.assertEqual(
            (False, "att-error"),
            ble_write_admission_model(0, complete_frame, False),
        )
        self.assertEqual(
            (False, "disconnect"),
            ble_write_admission_model(0, complete_frame, True),
        )
        self.assertEqual(
            (False, "att-error"),
            ble_write_admission_model(
                1, complete_frame + complete_frame, False
            ),
        )
        self.assertEqual(
            (True, "accepted"),
            ble_write_admission_model(1, complete_frame, False),
        )

    def test_assignment_claims_and_acks_do_not_depend_on_ble_stream_custody(self):
        classifier = function_body(
            APP, "mesh_gateway_delivery_is_internal_control"
        )
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        self.assertIn("MSG_COMMAND_RESULT", classifier)
        self.assertIn("tlv_find_unique", classifier)
        self.assertIn("TLV_COMMAND_ID", classifier)
        self.assertIn("CMD_ASSIGN_DISCOVERY_SLOTS", classifier)
        self.assertNotIn("discovery_assignment_parse_result_tlvs", classifier)
        self.assertLess(
            classifier.index("TLV_COMMAND_ID"),
            classifier.index("CMD_ASSIGN_DISCOVERY_SLOTS"),
        )
        internal = drain.index("if (internal_control)")
        internal_end = drain.index("} else {", internal)
        self.assertIn(
            "host_custody_ready = true;",
            drain[internal:internal_end],
        )
        self.assertNotIn(
            "gateway_ble_reserve_stream_packet",
            drain[internal:internal_end],
        )
        self.assertIn("!internal_control", drain)
        self.assertLess(
            drain.index("mesh_gateway_delivery_is_internal_control(pending)"),
            drain.index("gateway_ble_reserve_stream_packet"),
        )
        self.assertIn(
            "mesh_gateway_accept_semantic_delivery", drain
        )

    def test_durable_host_output_precedes_duplicate_commit_and_ack_actions(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        journal_match = drain.index(
            "app_mesh_persistence_gateway_host_journal_matches"
        )
        semantic_preflight = drain.index(
            "mesh_gateway_preflight_semantic_delivery", journal_match
        )
        reserve = drain.index(
            "gateway_ble_reserve_stream_packet", semantic_preflight
        )
        journal_prepare = drain.index(
            "app_mesh_persistence_prepare_gateway_host_journal",
            reserve,
        )
        semantic = drain.index(
            "mesh_gateway_accept_semantic_delivery", journal_prepare
        )
        rejected = drain.index("if (semantic_ret < 0)", semantic)
        rollback = drain.index(
            "app_mesh_persistence_clear_gateway_host_journal_if_matches",
            rejected,
        )
        journal_commit = drain.index(
            "app_mesh_persistence_commit_gateway_host_journal", semantic
        )
        stream = drain.index(
            "gateway_ble_commit_stream_reservation", journal_commit
        )
        finalize = drain.index("gateway_finalize_semantic_delivery", stream)
        commit = drain.index("mesh_relay_commit_gateway_delivery", finalize)
        actions = drain.index("mesh_handle_result_actions", commit)

        self.assertLess(journal_match, semantic_preflight)
        self.assertLess(semantic_preflight, reserve)
        self.assertLess(semantic_preflight, journal_prepare)
        self.assertLess(journal_prepare, semantic)
        self.assertLess(semantic, rejected)
        self.assertLess(rejected, rollback)
        self.assertLess(semantic, journal_commit)
        self.assertLess(journal_commit, stream)
        self.assertLess(rollback, stream)
        self.assertLess(journal_prepare, stream)
        self.assertLess(rejected, stream)
        self.assertLess(stream, finalize)
        self.assertLess(finalize, commit)
        self.assertLess(stream, actions)
        self.assertLess(commit, actions)
        self.assertIn("gateway_ble_cancel_stream_reservation", drain)
        self.assertIn("deferred for BLE custody", drain[reserve:semantic])
        self.assertIn("rejected without ACK", drain[semantic:actions])

    def test_same_boot_commit_shortcut_has_exact_semantic_owner(self):
        owner = re.search(
            r"struct gateway_semantic_commit_owner \{(?P<body>.*?)\};",
            APP,
            re.DOTALL,
        )
        self.assertIsNotNone(owner)
        owner_body = owner.group("body")
        for field in (
            "msg_type",
            "flags",
            "src_id",
            "dst_id",
            "session_id",
            "seq",
            "payload_len",
            "payload_digest",
            "active",
        ):
            with self.subTest(field=field):
                self.assertIn(field, owner_body)
        self.assertNotIn("ttl", owner_body)
        self.assertNotIn("message_age_ms", owner_body)
        self.assertNotIn("payload_crc", owner_body)
        self.assertNotRegex(
            APP,
            r"static\s+bool\s+gateway_semantic_commit_pending",
        )

        matches = function_body(
            APP, "gateway_semantic_commit_owner_identity_matches"
        )
        self.assertIn("packet->payload_len == payload_len", matches)
        self.assertIn("owner->payload_len == payload_len", matches)
        self.assertIn("semantic_digest_equal", matches)
        self.assertIn("SEMANTIC_DIGEST_SHA256_LEN", matches)
        self.assertNotIn("payload_crc", matches)
        self.assertIn("owner->flags == packet->flags", matches)
        self.assertNotIn("packet->ttl", matches)
        self.assertNotIn("packet->message_age_ms", matches)

        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        prepared = drain.index(
            "APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED"
        )
        exact_owner = drain.index(
            "gateway_semantic_commit_owner_matches", prepared
        )
        shortcut = drain.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", exact_owner
        )
        mutation = drain.index(
            "mesh_gateway_accept_semantic_delivery", exact_owner
        )
        owner_set = drain.index(
            "gateway_semantic_commit_owner_set", mutation
        )
        self.assertLess(exact_owner, shortcut)
        self.assertLess(mutation, owner_set)

    def test_negative_pure_preflight_rejects_before_prepare_or_mutation(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        preflight = drain.index("mesh_gateway_preflight_semantic_delivery")
        completed_negative = drain.index(
            "semantic_preflight_complete && semantic_ret < 0",
            preflight,
        )
        reject_jump = drain.index(
            "goto gateway_semantic_result_ready", completed_negative
        )
        prepare = drain.index(
            "app_mesh_persistence_prepare_gateway_host_journal",
            reject_jump,
        )
        mutation = drain.index(
            "mesh_gateway_accept_semantic_delivery", prepare
        )
        rollback = drain.index(
            "app_mesh_persistence_clear_gateway_host_journal_if_matches",
            mutation,
        )

        self.assertLess(completed_negative, reject_jump)
        self.assertLess(reject_jump, prepare)
        self.assertLess(prepare, mutation)
        self.assertIn(
            "if (journal_owned_for_packet &&",
            drain[mutation:rollback],
        )

    def test_ambiguous_prepare_failure_forces_restore(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        prepare = drain.index(
            "app_mesh_persistence_prepare_gateway_host_journal"
        )
        failure = drain.index("if (ret < 0)", prepare)
        restore = drain.index(
            'gateway_ble_require_host_journal_restore(\n'
            '                                "host-output-prepare-uncertain")',
            failure,
        )
        cleanup = drain.index("goto mesh_rx_item_cleanup", restore)
        mutation = drain.index(
            "mesh_gateway_accept_semantic_delivery", prepare
        )

        self.assertLess(failure, restore)
        self.assertLess(restore, cleanup)
        self.assertLess(cleanup, mutation)

    def test_terminal_journal_match_never_owns_stream_reservation(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        reserve = function_body(
            real_gateway, "gateway_ble_reserve_stream_packet"
        )
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        self.assertIn(
            "GATEWAY_BLE_STREAM_RESERVATION_JOURNAL_TERMINAL",
            reserve,
        )
        self.assertIn(
            "GATEWAY_BLE_STREAM_RESERVATION_ACQUIRED",
            drain,
        )
        self.assertIn(
            "GATEWAY_BLE_STREAM_RESERVATION_JOURNAL_TERMINAL",
            drain,
        )
        self.assertNotIn("stream_reserved", drain)
        cancel_count = drain.count(
            "gateway_ble_cancel_stream_reservation();"
        )
        guarded_cancel_count = len(re.findall(
            r"if \(stream_reservation_acquired\) \{\s*"
            r"gateway_ble_cancel_stream_reservation\(\);",
            drain,
        ))
        self.assertEqual(cancel_count, guarded_cancel_count)

    def test_journal_restore_resolution_clears_same_boot_owner(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        restore = function_body(
            real_gateway, "gateway_restore_host_journal_runtime"
        )
        self.assertGreaterEqual(
            restore.count("mesh_gateway_semantic_commit_owner_clear()"),
            3,
        )

    def test_all_semantic_gateway_types_share_the_commit_gate(self):
        app_gate = function_body(
            APP, "mesh_gateway_delivery_requires_semantic_acceptance"
        )
        relay_gate = function_body(RELAY, "gateway_delivery_requires_commit")

        for message_type in (
            "MSG_CLICK_REPORT",
            "MSG_COMMAND_RESULT",
            "MSG_RESULT_BUNDLE",
            "MSG_SURVEY_DISCOVERY_REPORT",
            "MSG_SURVEY_PAIR_RESULT",
        ):
            with self.subTest(message_type=message_type):
                self.assertIn(message_type, app_gate)
                self.assertIn(message_type, relay_gate)

    def test_survey_burst_uses_explicit_end_to_end_host_flow_control(self):
        matches = function_body(
            PERSISTENCE,
            "app_mesh_persistence_gateway_host_journal_matches_with_projection",
        )
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        self.assertIn("metadata.valid : -EBUSY", matches)
        self.assertNotIn(
            "app_mesh_persistence_clear_gateway_host_journal", matches
        )
        self.assertRegex(
            drain,
            r"\} else if \(journal_ret < 0\) \{\s*"
            r"semantic_ret = journal_ret;\s*"
            r"host_custody_ready = true;\s*\}",
        )
        self.assertRegex(
            drain,
            r"if \(journal_ret < 0\) \{\s*"
            r"semantic_ret = journal_ret;\s*"
            r"\} else if \(",
        )
        result_gate = drain.index("gateway_semantic_result_ready:")
        rejection = drain.index("if (semantic_ret < 0)", result_gate)
        cleanup = drain.index("goto mesh_rx_item_cleanup", rejection)
        finalize = drain.index("gateway_finalize_semantic_delivery", cleanup)
        relay_commit = drain.index(
            "mesh_relay_commit_gateway_delivery", finalize
        )
        self.assertIn("rejected without ACK", drain[rejection:cleanup])
        self.assertLess(cleanup, finalize)
        self.assertLess(finalize, relay_commit)

        for token in (
            "#define SURVEY_PAIR_RESULT_MAX_BURST_RECORDS",
            "SURVEY_GATEWAY_MAX_REPORTS * "
            "SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT",
            "#define SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS",
            "SURVEY_PAIR_RESULT_MAX_BURST_RECORDS *",
            "SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS",
            "#define SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS",
            "SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS",
            "SURVEY_PAIR_RESULT_MAX_BURST_RECORDS == 200u",
            "SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS == 120000u",
            "SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS == 30000u",
        ):
            with self.subTest(token=token):
                self.assertIn(token, SURVEY_H)
        commitment = function_body(
            SURVEY_ROUND, "gateway_survey_round_commitment"
        )
        submit = function_body(
            SURVEY_ROUND, "gateway_survey_round_submit_go"
        )
        self.assertIn(
            "longest_run_ms + SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS",
            commitment,
        )
        self.assertIn(
            "SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS", submit
        )

    def test_prepared_boot_recovery_is_bounded_and_phase_explicit(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        recover = function_body(
            real_gateway, "gateway_recover_prepared_host_journal"
        )
        restore = function_body(
            real_gateway, "gateway_restore_host_journal_runtime"
        )

        self.assertIn(
            "APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED", recover
        )
        self.assertIn(
            "APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW", recover
        )
        self.assertIn(
            "gateway_preflight_result_semantic_delivery", recover
        )
        self.assertIn(
            "app_mesh_persistence_clear_gateway_host_journal_if_matches",
            recover,
        )
        prepared = restore.index(
            "APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED"
        )
        classify = restore.index(
            "gateway_recover_prepared_host_journal", prepared
        )
        enqueue = restore.index(
            "gateway_ble_stream_enqueue_staged_packet", classify
        )
        self.assertLess(prepared, classify)
        self.assertLess(classify, enqueue)

    def test_collection_epoch_is_categorical_and_unique_in_preflight(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        preflight = function_body(
            real_gateway, "gateway_preflight_result_semantic_delivery"
        )
        finalize = function_body(
            real_gateway, "gateway_finalize_semantic_delivery"
        )

        for body in (preflight, finalize):
            self.assertIn("tlv_find_unique", body)
            self.assertIn("TLV_COLLECTION_EPOCH_ID", body)
            self.assertIn("collection_epoch_len != sizeof(uint32_t)", body)
        self.assertIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW", preflight
        )
        self.assertIn("return -EBADMSG;", preflight)

    def test_collection_redrive_without_a_journal_bypasses_host_custody(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        preflight = drain.index(
            "mesh_gateway_preflight_semantic_delivery"
        )
        redrive = drain.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE", preflight
        )
        reserve = drain.index("gateway_ble_reserve_stream_packet", redrive)
        prepare = drain.index(
            "app_mesh_persistence_prepare_gateway_host_journal", reserve
        )
        semantic = drain.index(
            "mesh_gateway_accept_semantic_delivery", prepare
        )
        marker_gate = drain.index(
            "!collection_redrive_without_host", semantic
        )
        bypass = drain.index(
            "collection_redrive_without_host &&",
            marker_gate + len("!collection_redrive_without_host"),
        )
        finalize = drain.index("gateway_finalize_semantic_delivery", bypass)
        relay_commit = drain.index(
            "mesh_relay_commit_gateway_delivery", finalize
        )

        self.assertLess(preflight, redrive)
        self.assertLess(redrive, reserve)
        self.assertIn(
            "collection_redrive_without_host = true",
            drain[redrive:reserve],
        )
        self.assertIn(
            "!collection_redrive_without_host",
            drain[reserve:prepare],
        )
        self.assertLess(marker_gate, bypass)
        self.assertNotIn(
            "gateway_ble_commit_stream_reservation",
            drain[bypass:finalize],
        )
        self.assertLess(bypass, finalize)
        self.assertLess(finalize, relay_commit)

    def test_prepared_collection_redrive_keeps_original_host_custody(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        recover = function_body(
            real_gateway, "gateway_recover_prepared_host_journal"
        )

        redrive = recover.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE"
        )
        commit = recover.index(
            "app_mesh_persistence_commit_gateway_host_journal", redrive
        )
        self.assertLess(redrive, commit)

    def test_collection_handler_reports_durable_acceptance_to_the_gate(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        result = function_body(real_gateway, "gateway_note_command_result")
        bundle = function_body(real_gateway, "gateway_note_command_result_bundle")
        collection_result = function_body(
            real_gateway, "gateway_note_collection_result"
        )
        collection_bundle = function_body(
            real_gateway, "gateway_note_collection_bundle"
        )
        accept = function_body(GATEWAY, "gateway_accept_collection_record")
        finalize = function_body(
            real_gateway, "gateway_finalize_semantic_delivery"
        )

        self.assertIn("if (collection_ret >= 0)", result)
        self.assertIn("return collection_ret;", result)
        self.assertIn("return gateway_note_collection_bundle", bundle)
        self.assertIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE",
            collection_result,
        )
        self.assertIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE",
            collection_bundle,
        )
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

    def test_uncertain_terminal_marker_restores_host_custody_before_retry_ack(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        marker_failure = drain.index(
            '"gateway semantic commit marker failed without ACK'
        )
        recovery = drain.rfind(
            "gateway_ble_require_host_journal_restore", 0, marker_failure
        )
        cleanup = drain.index("goto mesh_rx_item_cleanup", marker_failure)

        self.assertGreater(recovery, drain.index(
            "app_mesh_persistence_commit_gateway_host_journal"
        ))
        self.assertLess(recovery, marker_failure)
        self.assertLess(marker_failure, cleanup)

        rollback_failure = drain.index(
            '"gateway rejected host journal rollback failed'
        )
        rollback_recovery = drain.rfind(
            "gateway_ble_require_host_journal_restore",
            0,
            rollback_failure,
        )
        self.assertLess(rollback_recovery, rollback_failure)

        restore = function_body(
            GATEWAY, "gateway_ble_require_host_journal_restore"
        )
        self.assertIn(
            "atomic_set(&gateway_host_journal_restore_pending, 1)", restore
        )
        self.assertIn(
            "atomic_clear(&gateway_host_journal_restored)", restore
        )
        self.assertIn("gateway_schedule_persistence_retry", restore)

    def test_rejected_command_payload_keeps_pending_transaction_untouched(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        result = function_body(real_gateway, "gateway_note_command_result")
        deliver = result.index("app_mesh_command_orchestrator_gateway_deliver")
        success = result.index("if (ret == PROTO_OK)", deliver)
        claim = result.index(
            "gateway_command_pending_claim_result", success
        )

        self.assertLess(deliver, success)
        self.assertLess(success, claim)
        self.assertIn("gateway_pending_snapshot_current_locked", result[success:claim])
        self.assertNotIn("gateway_command_result_side_effects", result)

    def test_timeout_claim_is_prompt_but_side_effects_keep_route_ownership(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        timeout = function_body(
            real_gateway, "gateway_command_result_timeout_handler"
        )
        side_effect = function_body(
            real_gateway, "gateway_command_timeout_side_effect_handler"
        )
        schedule = function_body(
            real_gateway, "gateway_schedule_command_timeout_side_effects"
        )
        retry = function_body(
            real_gateway,
            "gateway_command_timeout_side_effect_retry_handler",
        )
        wait = function_body(
            real_gateway, "gateway_begin_command_result_wait_until"
        )
        collection = function_body(
            real_gateway, "gateway_begin_command_collection"
        )

        claim = timeout.index("gateway_command_pending_expired")
        retain = timeout.index(
            "gateway_command_timeout_dispatch_state.pending = true", claim
        )
        emit = timeout.index("gateway_emit_host_command_result", retain)
        dispatch = timeout.index(
            "gateway_schedule_command_timeout_side_effects", emit
        )
        self.assertLess(claim, retain)
        self.assertLess(retain, emit)
        self.assertLess(emit, dispatch)
        self.assertNotIn("mesh_relay_note_delivery_failure_at", timeout)
        self.assertNotIn("gateway_command_timeout_side_effects(", timeout)
        self.assertIn("mesh_relay_note_delivery_failure_at", side_effect)
        self.assertIn("gateway_command_timeout_side_effects(", side_effect)
        self.assertIn("mesh_route_work_reschedule", schedule)
        self.assertIn(
            "gateway_command_timeout_side_effect_retry_work", schedule
        )
        self.assertIn(
            "gateway_schedule_command_timeout_side_effects", retry
        )
        self.assertIn(
            "gateway_command_timeout_dispatch_state.pending", wait
        )
        self.assertIn(
            "gateway_command_timeout_dispatch_state.pending", collection
        )

    def test_predeadline_rf_arrival_owns_validation_before_timeout(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        timeout = function_body(
            real_gateway, "gateway_command_result_timeout_handler"
        )
        result = function_body(real_gateway, "gateway_note_command_result")
        reserve = function_body(
            real_gateway, "gateway_command_result_validation_reserve"
        )
        rx = function_body(RX, "mesh_queue_from_frame_at_internal")
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        interval = timeout.index(
            "gateway_command_result_validation_check_interval("
        )
        blocked = timeout.index(
            "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED", interval
        )
        lease_expired = timeout.index(
            "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED", interval
        )
        expire = timeout.index("gateway_command_pending_expired(", interval)
        self.assertLess(interval, lease_expired)
        self.assertLess(interval, blocked)
        self.assertLess(blocked, expire)
        self.assertLess(lease_expired, expire)
        self.assertIn(
            "uint64_t result_received_at_ms = first_received_at_ms", result
        )
        self.assertIn(
            "packet, result_received_at_ms, result_validation_token", result
        )
        self.assertIn(
            "(uint32_t)result_received_at_ms", result
        )
        self.assertIn(
            "gateway_command_result_validation_acquire(", reserve
        )
        lease = rx.index("gateway_command_result_validation_reserve(")
        enqueue = rx.index("k_msgq_put(&mesh_rx_msgq", lease)
        rollback = rx.index(
            "gateway_command_result_validation_release_reserved(", enqueue
        )
        self.assertLess(lease, enqueue)
        self.assertLess(enqueue, rollback)
        self.assertIn("mesh_rx_item_cleanup:", drain)
        self.assertIn(
            "gateway_command_result_validation_release_reserved(",
            drain[drain.index("mesh_rx_item_cleanup:"):],
        )

    def test_survey_and_generic_wait_share_one_absolute_deadline(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        send = function_body(ANCHOR, "gateway_survey_auto_send_outbound")
        relative = function_body(
            real_gateway, "gateway_begin_command_result_wait_for"
        )
        absolute = function_body(
            real_gateway, "gateway_begin_command_result_wait_until"
        )

        self.assertIn(
            "gateway_begin_command_result_wait_until(", send
        )
        self.assertIn("(uint32_t)absolute_deadline_ms", send)
        self.assertNotIn(
            "gateway_begin_command_result_wait_for(", send
        )
        self.assertIn(
            "gateway_begin_command_result_wait_until(", relative
        )
        self.assertIn(
            "gateway_command_pending_start_until(", absolute
        )
        self.assertIn("absolute_deadline_ms", absolute)

    def test_send_failure_cannot_emit_after_timeout_wins_terminal_claim(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        clear = function_body(
            real_gateway, "gateway_clear_pending_command_result"
        )
        generic = function_body(ANCHOR, "gateway_route_mesh_host_packet")
        survey_pair = function_body(
            ANCHOR, "gateway_route_survey_pair_control"
        )

        self.assertIn("return cleared;", clear)
        self.assertIn(
            "if (!gateway_clear_pending_command_result", generic
        )
        self.assertIn(
            "if (!gateway_clear_pending_command_result", survey_pair
        )

    def test_transformed_commands_rebind_result_custody_before_wait_or_rf(self):
        generic = function_body(ANCHOR, "gateway_route_mesh_host_packet")
        survey_pair = function_body(
            ANCHOR, "gateway_route_survey_pair_control"
        )

        generic_rebind = generic.index(
            "gateway_command_result_rebind_command"
        )
        generic_wait = generic.index(
            "gateway_begin_command_result_wait", generic_rebind
        )
        generic_send = generic.index(
            "mesh_send_gateway_command_flood", generic_rebind
        )
        pair_rebind = survey_pair.index(
            "gateway_command_result_rebind_command"
        )
        pair_wait = survey_pair.index(
            "gateway_begin_command_result_wait_for", pair_rebind
        )
        pair_send = survey_pair.index(
            "gateway_survey_send_pair_control", pair_wait
        )

        self.assertLess(generic_rebind, generic_wait)
        self.assertLess(generic_rebind, generic_send)
        self.assertLess(pair_rebind, pair_wait)
        self.assertLess(pair_wait, pair_send)

    def test_result_custody_token_survives_queue_transform_and_terminal(self):
        real_gateway = GATEWAY[GATEWAY.index(
            "static struct gateway_command_pending gateway_command_pending_state"
        ):]
        admit = function_body(ANCHOR, "gateway_host_command_admit")
        dispatch = function_body(ANCHOR, "gateway_host_command_work_handler")
        wait = function_body(
            real_gateway, "gateway_begin_command_result_wait_until"
        )
        timeout = function_body(
            real_gateway, "gateway_command_result_timeout_handler"
        )
        result = function_body(real_gateway, "gateway_note_command_result")

        self.assertIn("uint32_t result_reservation_token;", INGRESS_H)
        self.assertIn("item->result_reservation_token =", admit)
        set_token = dispatch.index(
            "gateway_command_result_set_dispatch_token("
        )
        route = dispatch.index("gateway_route_host_packet(", set_token)
        clear_token = dispatch.index(
            "gateway_command_result_set_dispatch_token(0u)", route
        )
        self.assertLess(set_token, route)
        self.assertLess(route, clear_token)
        self.assertIn(
            "gateway_pending_command_result_token = "
            "gateway_command_result_dispatch_token",
            " ".join(wait.split()),
        )
        self.assertIn(
            "gateway_command_timeout_dispatch_state.result_reservation_token",
            timeout,
        )
        self.assertIn(
            "gateway_command_pending_claim_result(", result
        )
        self.assertIn(
            "gateway_command_result_release_terminal_reserved(", result
        )
        preflight = result.index("gateway_survey_auto_preflight_result(")
        claim = result.index("gateway_command_pending_claim_result(", preflight)
        commit = result.index(
            "gateway_survey_auto_commit_preflight_result()", claim
        )
        boundary = result.index("gateway_command_delivery_boundary(", commit)
        self.assertLess(preflight, claim)
        self.assertLess(claim, commit)
        self.assertLess(commit, boundary)
        expired = result.index(
            "GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED", claim
        )
        discard = result.index(
            "gateway_survey_auto_discard_preflight_result()", expired
        )
        timeout_emit = result.index(
            "gateway_emit_host_command_result_reserved(", discard
        )
        self.assertLess(expired, discard)
        self.assertLess(discard, timeout_emit)

    def test_admission_failures_retain_reserved_terminal_result_until_rejection(self):
        admit = function_body(ANCHOR, "gateway_host_command_admit")
        preemptive = function_body(
            ANCHOR, "gateway_host_command_submit_preemptive"
        )
        ingress = function_body(
            INGRESS, "app_gateway_command_ingress_handle_frame"
        )
        emit = function_body(ANCHOR, "gateway_host_command_emit_result")

        allocate = admit.index("gateway_next_broadcast_command_seq()")
        allocation_failure = admit.index("if (ret != PROTO_OK)", allocate)
        allocation_return = admit.index(
            "return mesh_errno_from_proto(ret)", allocation_failure
        )
        self.assertNotIn(
            "gateway_command_result_release_ingress",
            admit[allocation_failure:allocation_return],
        )

        bind = admit.index("gateway_host_command_bind_result_reservation")
        bind_failure = admit.index("if (ret < 0)", bind)
        bind_return = admit.index("return ret;", bind_failure)
        self.assertNotIn(
            "gateway_command_result_release_ingress",
            admit[bind_failure:bind_return],
        )

        preemptive_bind = preemptive.index(
            "gateway_host_command_bind_result_reservation"
        )
        preemptive_failure = preemptive.index(
            "if (ret < 0)", preemptive_bind
        )
        preemptive_return = preemptive.index(
            "return ret;", preemptive_failure
        )
        self.assertNotIn(
            "gateway_command_result_release_ingress",
            preemptive[preemptive_failure:preemptive_return],
        )

        admit_call = ingress.index("ret = ops->admit")
        common_failure = ingress.index("if (ret < 0)", admit_call)
        common_emit = ingress.index("ops->emit_result", common_failure)
        common_return = ingress.index("return ret;", common_emit)
        self.assertLess(common_failure, common_emit)
        self.assertLess(common_emit, common_return)

        bind_rejection = emit.index("gateway_command_result_bind_ingress")
        already_bound = emit.index("bind_ret == -EALREADY", bind_rejection)
        reserved_emit = emit.index(
            "gateway_emit_host_command_result_reserved", already_bound
        )
        self.assertLess(bind_rejection, already_bound)
        self.assertLess(already_bound, reserved_emit)

    def test_receipt_lookup_only_exposes_initialized_explicitly_found_state(self):
        scan = function_body(RECEIPTS, "receipt_store_scan")
        lookup = function_body(RECEIPTS, "receipt_lookup_for_gateway")
        classify = function_body(RECEIPTS, "receipt_classify_result")

        self.assertIn(
            "struct app_gateway_collection_receipt candidate = {0}", scan
        )
        self.assertIn("if (ret != 1)", scan)
        clear = lookup.index("memset(receipt, 0, sizeof(*receipt))")
        isr = lookup.index("if (k_is_in_isr())")
        lock = lookup.index("k_mutex_lock(", isr)
        lock_contract = lookup.index("if (ret != 0)", lock)
        scan_call = lookup.index("receipt_store_scan(", lock_contract)
        scan_contract = lookup.index("if (ret != 0)", scan_call)
        publish = lookup.index("*receipt = scan.found", scan_contract)
        self.assertLess(clear, isr)
        self.assertLess(isr, lock)
        self.assertLess(lock, lock_contract)
        self.assertLess(lock_contract, scan_call)
        self.assertLess(scan_call, scan_contract)
        self.assertLess(scan_contract, publish)
        self.assertIn("ret = ret < 0 ? ret : -EIO", lookup)
        self.assertIn(
            "struct app_gateway_collection_receipt stored = {0}", classify
        )

    def test_async_gateway_terminals_copy_token_out_of_dispatch_scope(self):
        assignment = function_body(
            ANCHOR, "gateway_start_discovery_assignment"
        )
        assignment_fail = function_body(
            ANCHOR, "gateway_discovery_assignment_fail_locked"
        )
        route = function_body(ANCHOR, "gateway_route_host_packet")
        refresh = function_body(ANCHOR, "gateway_route_refresh_observe")

        self.assertIn(
            "gateway_discovery_assignment_state.result_reservation_token = "
            "gateway_command_result_get_dispatch_token()",
            " ".join(assignment.split()),
        )
        self.assertIn(
            "gateway_emit_host_command_result_reserved(", assignment_fail
        )
        self.assertIn(
            "gateway_route_refresh_result_token = "
            "gateway_command_result_get_dispatch_token()",
            " ".join(route.split()),
        )
        self.assertIn(
            "gateway_emit_host_command_result_reserved(", refresh
        )

    def test_preemptive_abort_never_uses_the_route_owner_ambient_token(self):
        abort_work = function_body(
            ANCHOR, "gateway_host_abort_work_handler"
        )
        abort_route = function_body(
            ANCHOR, "gateway_host_abort_route_work_handler"
        )
        abort = function_body(
            ANCHOR, "gateway_handle_local_survey_abort"
        )

        self.assertNotIn("gateway_command_result_set_dispatch_token(", abort_work)
        self.assertNotIn("gateway_handle_local_survey_abort(", abort_work)
        self.assertNotIn(
            "gateway_host_command_cancel_pending_surveys(", abort_work
        )
        self.assertIn("mesh_gateway_command_priority_submit(", abort_work)
        self.assertIn("item.result_reservation_token", abort_route)
        self.assertIn("gateway_handle_local_survey_abort(", abort_route)
        cancel = abort_route.index(
            "gateway_host_command_cancel_pending_surveys("
        )
        finish = abort_route.index("gateway_handle_local_survey_abort(", cancel)
        self.assertLess(cancel, finish)
        self.assertIn(
            "gateway_emit_host_command_result_reserved(", abort
        )
        self.assertIn("result_reservation_token", abort)

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
        clear = begin.index("gateway_clear_eack_custody_or_defer", reset)
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
        wait = function_body(
            real_gateway, "gateway_begin_command_result_wait_until"
        )

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
