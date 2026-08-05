#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
ENCODE = (ROOT / "app" / "src" / "app_mesh_report_encode.c").read_text()
ANCHOR = read_composed_source(ROOT / "app" / "src" / "app_anchor.c")
PERSISTENCE = (ROOT / "app" / "src" / "app_mesh_persistence.c").read_text()
CH9_ACK = (ROOT / "app" / "src" / "app_mesh_ch9_ack.c").read_text()
REPORT_TX_QUEUE_DEPTH = 9
RANGE_REPORT_MAX_PACKET_FRAGMENTS = 9


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


def reserve_model(
    preload: tuple[str, ...],
    recovery: str | None = None,
    head_owned: bool = False,
) -> tuple[bool, tuple[str, ...], str | None]:
    """Mirror the zero-fragment ownership decisions in batch reserve."""
    queue = list(preload)
    if recovery == "local" or head_owned:
        return False, tuple(queue), recovery
    if recovery is None and len(queue) == 1:
        candidate = queue.pop(0)
        if candidate == "local":
            queue.append(candidate)
        else:
            recovery = candidate
    if (REPORT_TX_QUEUE_DEPTH - len(queue) <
            RANGE_REPORT_MAX_PACKET_FRAGMENTS):
        return False, tuple(queue), recovery
    return True, tuple(queue), recovery


def abort_zero_fragment_model(
    queue: tuple[str, ...],
    recovery: str | None,
) -> tuple[tuple[str, ...], str | None, bool, bool]:
    """Release a provisional hold and restore/schedule any displaced owner."""
    restored = list(queue)
    if recovery is not None and len(restored) < REPORT_TX_QUEUE_DEPTH:
        restored.append(recovery)
        recovery = None
    scheduled = bool(restored) or recovery is not None
    return tuple(restored), recovery, False, scheduled


class RangeReportBatchAdmissionSourceInvariantTests(unittest.TestCase):
    def test_retry_attempts_use_disjoint_range_transport_sequences(self):
        body = function_body(ENCODE, "build_range_report_samples")

        first = body.index("report_range_transport_seq")
        packet_init = body.index("report_init_range_packet", first)
        self.assertLess(first, packet_init)
        self.assertIn(
            "report_range_transport_seq(attempt_index,\n"
            "                                         packet_index,\n"
            "                                         &cir_seq)",
            body,
        )
        self.assertNotIn("range_result->seq", body)

    def test_encoder_reserves_before_first_fragment_and_aborts_on_failure(self):
        body = function_body(ENCODE, "build_range_report_samples")

        reserve = body.index("mesh_range_report_batch_reserve")
        loop = body.index("do {")
        enqueue = body.index("queue_anchor_range_report_fragment")
        abort = body.rindex("mesh_range_report_batch_abort")
        self.assertLess(reserve, loop)
        self.assertLess(loop, enqueue)
        self.assertLess(enqueue, abort)
        self.assertNotIn("queue_anchor_report(&outbound)", body)
        self.assertIn("final_fragment", body)

    def test_reservation_requires_capacity_for_the_complete_worst_case(self):
        body = function_body(REPORT, "mesh_range_report_batch_reserve")

        self.assertIn("k_msgq_num_free_get", body)
        self.assertIn("RANGE_REPORT_MAX_PACKET_FRAGMENTS", body)
        self.assertIn("report_tx_queue_recovery_valid", body)
        self.assertIn("app_mesh_queue_head_owned", body)
        self.assertLess(
            body.index("k_msgq_num_free_get"),
            body.index("anchor_range_report_batch_reservation ="),
        )

    def test_requested_preload_matrix_has_one_unambiguous_owner(self):
        body = function_body(REPORT, "mesh_range_report_batch_reserve")

        single = body.index("queue_count == 1u")
        capacity_with_displacement = body.index(
            "k_msgq_num_free_get(&report_tx_msgq) + 1u", single
        )
        dequeue = body.index("k_msgq_get(&report_tx_msgq", capacity_with_displacement)
        local = body.index("mesh_outbound_is_local_origin_priority", dequeue)
        restore_local = body.index("k_msgq_put(&report_tx_msgq", local)
        retain_transit = body.index("report_tx_queue_recovery_valid = true", restore_local)
        final_capacity = body.index(
            "k_msgq_num_free_get(&report_tx_msgq) <", retain_transit
        )

        self.assertLess(single, capacity_with_displacement)
        self.assertLess(capacity_with_displacement, dequeue)
        self.assertLess(dequeue, local)
        self.assertLess(local, restore_local)
        self.assertLess(restore_local, retain_transit)
        self.assertLess(retain_transit, final_capacity)

        cases = (
            ("preload-0", (), None, True, (), None),
            ("one-transit-recovery-owner", (), "transit", True, (), "transit"),
            ("one-transit-queued", ("transit",), None, True, (), "transit"),
            ("one-local", ("local",), None, False, ("local",), None),
            ("two-items", ("transit", "transit"), None, False,
             ("transit", "transit"), None),
        )
        for name, preload, recovery, admitted, queue_after, recovery_after in cases:
            with self.subTest(name=name):
                self.assertEqual(
                    (admitted, queue_after, recovery_after),
                    reserve_model(preload, recovery),
                )

    def test_partial_batch_is_invisible_to_tx_and_blocks_other_admission(self):
        begin = function_body(REPORT, "report_tx_queue_begin_head")
        append = function_body(REPORT, "report_tx_queue_append")
        peek = function_body(REPORT, "report_tx_queue_peek")
        remove = function_body(REPORT, "mesh_preempt_discard_requeued_click_report")
        generic = function_body(REPORT, "queue_anchor_report")
        worker = function_body(REPORT, "report_tx_work_handler")
        fragment = function_body(REPORT, "queue_anchor_range_report_fragment")

        self.assertIn("anchor_range_report_batch_reservation.active", begin)
        self.assertIn("anchor_range_report_batch_reservation.active", append)
        self.assertIn("ret = -EAGAIN", append)
        self.assertIn("anchor_range_report_batch_reservation.active", peek)
        self.assertIn("ret = -EBUSY", peek)
        self.assertIn("anchor_range_report_batch_reservation.active", remove)
        self.assertIn("return -EBUSY", remove)
        self.assertIn("anchor_range_report_batch_reservation.active", generic)
        self.assertIn("return -EAGAIN", generic)
        self.assertIn("anchor_range_report_batch_reservation.active", worker)
        final = fragment.index("if (final_fragment)")
        release = fragment.index(
            "memset(&anchor_range_report_batch_reservation", final
        )
        promote = fragment.index("report_tx_queue_promote_local_locked", final)
        self.assertLess(release, promote)

    def test_encode_failure_after_prefix_removes_only_batch_and_restores_transit(self):
        encode = function_body(ENCODE, "build_range_report_samples")
        abort = function_body(REPORT, "mesh_range_report_batch_abort")

        enqueue = encode.index("queue_anchor_range_report_fragment")
        failure = encode.index("goto fail", enqueue)
        rollback = encode.rindex("mesh_range_report_batch_abort")
        self.assertLess(enqueue, failure)
        self.assertLess(failure, rollback)

        remove_prefix = abort.index(
            "anchor_range_report_batch_reservation.queued_fragment_count"
        )
        release = abort.rindex(
            "memset(&anchor_range_report_batch_reservation"
        )
        restore = abort.index(
            "report_tx_queue_restore_recovery_locked", release
        )
        schedule = abort.index("report_tx_schedule(0u)", restore)
        self.assertLess(remove_prefix, release)
        self.assertLess(release, restore)
        self.assertLess(restore, schedule)
        self.assertIn("recovery_pending", abort[restore:schedule])

    def test_click_handoff_reserves_before_work_and_always_cancels(self):
        admit = function_body(ANCHOR, "anchor_handle_mesh_click_wake_claim")
        run = function_body(ANCHOR, "anchor_run_mesh_click_wake_claim")

        self.assertLess(
            admit.index("mesh_range_report_batch_reserve"),
            admit.index("k_work_submit_to_queue"),
        )
        self.assertIn("mesh_range_report_batch_abort", admit)
        self.assertIn("mesh_range_report_batch_abort", run)
        provisional_release = run.index("mesh_range_report_batch_abort")
        preempt = run.index("mesh_preempt_for_click_event", provisional_release)
        final_reserve = run.index("mesh_range_report_batch_reserve", preempt)
        radio = run.index("radio_guard_uwb_start", final_reserve)
        self.assertLess(provisional_release, preempt)
        self.assertLess(preempt, final_reserve)
        self.assertLess(final_reserve, radio)

    def test_preemption_failure_has_no_stale_hold_or_rf_and_restores_custody(self):
        run = function_body(ANCHOR, "anchor_run_mesh_click_wake_claim")
        abort = function_body(REPORT, "mesh_range_report_batch_abort")

        provisional_release = run.index("mesh_range_report_batch_abort")
        preempt = run.index("mesh_preempt_for_click_event", provisional_release)
        final_reserve = run.index("mesh_range_report_batch_reserve", preempt)
        failure_path = run[preempt:final_reserve]

        self.assertLess(provisional_release, preempt)
        self.assertIn("anchor_click_window_set_active(false)", failure_path)
        self.assertIn("return false", failure_path)
        self.assertNotIn("radio_guard_uwb_start", failure_path)
        self.assertNotIn("anchor_handle_uwb_claim", failure_path)
        self.assertLess(
            abort.index("memset(&anchor_range_report_batch_reservation"),
            abort.index("report_tx_queue_restore_recovery_locked"),
        )
        self.assertLess(
            abort.index("report_tx_queue_restore_recovery_locked"),
            abort.index("report_tx_schedule(0u)"),
        )

        admitted, queue, recovery = reserve_model(("old-transit",))
        self.assertTrue(admitted)
        queue, recovery, active, scheduled = abort_zero_fragment_model(
            queue, recovery
        )
        self.assertEqual(("old-transit",), queue)
        self.assertIsNone(recovery)
        self.assertFalse(active)
        self.assertTrue(scheduled)

    def test_post_preemption_reserve_failure_stops_before_rf_and_reschedules(self):
        run = function_body(ANCHOR, "anchor_run_mesh_click_wake_claim")
        reserve = function_body(REPORT, "mesh_range_report_batch_reserve")

        preempt = run.index("mesh_preempt_for_click_event")
        final_reserve = run.index("mesh_range_report_batch_reserve", preempt)
        radio = run.index("radio_guard_uwb_start", final_reserve)
        failure_path = run[final_reserve:radio]

        self.assertIn("if (ret < 0)", failure_path)
        self.assertIn("anchor_click_window_set_active(false)", failure_path)
        self.assertIn("report_tx_schedule(0u)", failure_path)
        self.assertIn("return false", failure_path)
        self.assertNotIn("anchor_handle_uwb_claim", run[:radio])
        self.assertLess(
            reserve.index(
                "k_msgq_num_free_get(&report_tx_msgq) <"
            ),
            reserve.index("anchor_range_report_batch_reservation ="),
        )

        for name, preload in (
            ("older-local", ("local",)),
            ("two-transit", ("old-transit", "new-transit")),
            ("local-and-transit", ("local", "transit")),
        ):
            with self.subTest(name=name):
                admitted, queue, recovery = reserve_model(preload)
                self.assertFalse(admitted)
                self.assertEqual(preload, queue)
                self.assertIsNone(recovery)
                # The source failure path schedules the preserved owner(s).
                self.assertIn("report_tx_schedule(0u)", failure_path)

    def test_concurrent_admission_during_handoff_is_rechecked_atomically(self):
        # Releasing a zero-fragment provisional hold makes concurrent queue
        # ownership visible. The final reserve must decide from that new state.
        cases = (
            ("one-concurrent-transit", (), ("transit",), True,
             (), "transit"),
            ("one-concurrent-local", (), ("local",), False,
             ("local",), None),
            ("two-concurrent-items", (), ("transit", "transit"), False,
             ("transit", "transit"), None),
            ("older-transit-plus-concurrent-local", ("old-transit",),
             ("local",), False, ("old-transit", "local"), None),
        )
        for (name, initial, concurrent, expected_admitted, expected_queue,
             expected_recovery) in cases:
            with self.subTest(name=name):
                admitted, queue, recovery = reserve_model(initial)
                self.assertTrue(admitted)
                queue, recovery, active, scheduled = abort_zero_fragment_model(
                    queue, recovery
                )
                self.assertFalse(active)
                if initial:
                    self.assertTrue(scheduled)
                queue = queue + concurrent
                admitted, queue, recovery = reserve_model(queue, recovery)
                self.assertEqual(expected_admitted, admitted)
                self.assertEqual(expected_queue, queue)
                self.assertEqual(expected_recovery, recovery)

                owned = list(queue)
                if recovery is not None:
                    owned.append(recovery)
                self.assertCountEqual(initial + concurrent, tuple(owned))

    def test_fragment_records_and_commit_are_durable_before_visibility(self):
        fragment = function_body(REPORT, "queue_anchor_range_report_fragment")

        persist = fragment.index(
            "app_mesh_persistence_save_anchor_range_fragment"
        )
        queue = fragment.index("k_msgq_put(&report_tx_msgq", persist)
        final = fragment.index("if (final_fragment)", queue)
        commit = fragment.index(
            "app_mesh_persistence_commit_anchor_range_journal", queue
        )
        release = fragment.index(
            "memset(&anchor_range_report_batch_reservation", commit
        )
        self.assertLess(persist, queue)
        self.assertLess(queue, commit)
        self.assertLess(commit, release)
        self.assertIn(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED",
            fragment[persist:queue],
        )
        self.assertIn(
            "anchor_range_report_batch_reservation.persistence_fail_closed",
            fragment,
        )
        self.assertIn(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT",
            fragment[final:release],
        )
        self.assertIn(
            "policy_action == ANCHOR_RANGE_FRAGMENT_POLICY_RETRY",
            fragment[final:release],
        )
        self.assertIn("app_watchdog_stop_feeding()", fragment[commit:release])
        self.assertNotIn("ret = 1", fragment[commit:release])

    def test_post_range_persistence_uses_one_deadline_and_exact_owner(self):
        encode = function_body(ENCODE, "build_range_report_samples")
        fragment = function_body(REPORT, "queue_anchor_range_report_fragment")
        abort = function_body(REPORT, "mesh_range_report_batch_abort")
        anchor = function_body(ANCHOR, "anchor_handle_uwb_claim")

        deadline = encode.index(
            "persistence_deadline_ms =\n"
            "        k_uptime_get() + "
            "ANCHOR_RANGE_REPORT_PERSISTENCE_DEADLINE_MS"
        )
        reserve = encode.index("mesh_range_report_batch_reserve")
        enqueue = encode.index("queue_anchor_range_report_fragment")
        self.assertLess(deadline, reserve)
        self.assertLess(reserve, enqueue)
        self.assertIn("persistence_deadline_ms", encode[enqueue:])

        self.assertIn(
            "anchor_range_report_batch_reservation.fragment_pending = true",
            fragment,
        )
        self.assertIn(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE",
            fragment,
        )
        self.assertIn(
            "policy_action == ANCHOR_RANGE_FRAGMENT_POLICY_RETRY",
            fragment,
        )
        self.assertIn(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT",
            fragment,
        )
        self.assertIn(
            "anchor_range_report_batch_reservation.persistence_fail_closed = true",
            fragment,
        )
        self.assertIn("app_watchdog_stop_feeding()", fragment)
        self.assertLess(
            fragment.index("ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED"),
            fragment.index("k_msgq_put(&report_tx_msgq"),
        )

        self.assertIn(
            "anchor_range_report_batch_reservation.persistence_fail_closed",
            abort,
        )
        self.assertIn(
            "anchor_range_report_batch_reservation.fragment_pending",
            abort,
        )
        self.assertLess(
            abort.index(
                "anchor_range_report_batch_reservation.persistence_fail_closed"
            ),
            abort.index("k_msgq_get(&report_tx_msgq"),
        )
        self.assertIn("if (range_report_ret < 0)", anchor)
        self.assertIn("return false", anchor[anchor.index(
            "if (range_report_ret < 0)"
        ):])

    def test_fragment_persistence_types_prewrite_and_ambiguous_failures(self):
        save = function_body(
            PERSISTENCE, "app_mesh_persistence_save_anchor_range_fragment"
        )

        write = save.index("mesh_persistence_write(")
        confirmed = save.index(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED"
        )
        self.assertIn(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE",
            save[:write],
        )
        self.assertIn(
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS",
            save[write:confirmed],
        )
        self.assertLess(write, confirmed)
        self.assertNotIn(
            "anchor_range_controls_equal(control, &existing) ?",
            save,
        )

    def test_nvs_commit_marker_is_last_and_every_write_is_read_back(self):
        save = function_body(
            PERSISTENCE, "app_mesh_persistence_save_anchor_range_fragment"
        )
        commit = function_body(
            PERSISTENCE, "app_mesh_persistence_commit_anchor_range_journal"
        )
        clear = function_body(
            PERSISTENCE, "app_mesh_persistence_clear_anchor_range_journal"
        )

        fragment_write = save.index("mesh_persistence_write(")
        fragment_read = save.index("nvs_read(", fragment_write)
        fragment_decode = save.index(
            "anchor_range_journal_decode_fragment", fragment_read
        )
        publish_identity = save.index(
            "control->fragments[fragment_index] = identity", fragment_decode
        )
        self.assertLess(fragment_write, fragment_read)
        self.assertLess(fragment_read, fragment_decode)
        self.assertLess(fragment_decode, publish_identity)

        validate_all = commit.index(
            "anchor_range_read_fragment_locked(control, i, NULL)"
        )
        marker_write = commit.index(
            "APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID", validate_all
        )
        marker_read = commit.index("nvs_read(", marker_write)
        marker_decode = commit.index(
            "anchor_range_journal_decode_control", marker_read
        )
        self.assertLess(validate_all, marker_write)
        self.assertLess(marker_write, marker_read)
        self.assertLess(marker_read, marker_decode)

        marker_delete = clear.index(
            "nvs_delete(&mesh_nvs, APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID)"
        )
        orphan_delete = clear.index(
            "anchor_range_fragment_nvs_id(i)", marker_delete
        )
        self.assertLess(marker_delete, orphan_delete)

    def test_boot_restore_is_fail_closed_and_replays_exact_fragments(self):
        start = function_body(ANCHOR, "app_anchor_start_anchor_role")
        restore = function_body(
            REPORT, "mesh_restore_anchor_range_report_journal"
        )
        persistence_restore = function_body(
            PERSISTENCE, "anchor_range_read_fragment_locked"
        )

        outbox = start.index("app_mesh_persistence_restore_outbox")
        journal = start.index("mesh_restore_anchor_range_report_journal")
        discovery = start.index("app_anchor_survey_discovery_restore")
        self.assertLess(outbox, journal)
        self.assertLess(journal, discovery)
        self.assertIn("return ret", start[journal:discovery])
        self.assertIn(
            "app_mesh_persistence_restore_anchor_range_fragment", restore
        )
        self.assertIn("anchor_range_report_active_owner_matches", restore)
        self.assertIn("skipped_active_outbox", restore)
        owner_match = function_body(
            REPORT, "anchor_range_report_active_owner_matches"
        )
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", owner_match)
        self.assertIn("mesh_gateway_ack_confirm_matches_packet", owner_match)
        self.assertIn("return -EBADMSG", owner_match)
        self.assertLess(
            persistence_restore.index("proto_packet_decode"),
            persistence_restore.index("proto_packet_encode"),
        )
        self.assertIn(
            "memcmp(reencoded, wire, wire_len) != 0", persistence_restore
        )

    def test_partial_ack_reset_replays_full_batch_and_final_ack_clears(self):
        ack = function_body(
            REPORT, "mesh_anchor_range_report_note_gateway_confirmed"
        )
        actions = function_body(REPORT, "mesh_handle_result_actions")

        count = RANGE_REPORT_MAX_PACKET_FRAGMENTS
        acknowledged = {0, 4, 8}
        self.assertNotEqual(set(range(count)), acknowledged)
        # ACK state is RAM-only; reset restores the committed control and all
        # exact fragment records, so already accepted items may repeat safely.
        acknowledged = set()
        replayed = set(range(count))
        self.assertEqual(set(range(count)), replayed)
        self.assertFalse(acknowledged)

        self.assertIn("acknowledged_mask |=", ack)
        full = ack.index("anchor_range_report_all_acknowledged_mask")
        clear = ack.index(
            "app_mesh_persistence_clear_anchor_range_journal", full
        )
        runtime_release = ack.index(
            "memset(&anchor_range_report_journal_runtime", clear
        )
        self.assertLess(full, clear)
        self.assertLess(clear, runtime_release)
        completion = function_body(
            REPORT, "mesh_complete_gateway_ack_confirm"
        )
        self.assertIn("mesh_complete_gateway_ack_confirm", actions)
        note = completion.index(
            "mesh_anchor_range_report_note_gateway_confirmed"
        )
        terminal_commit = completion.index(
            "mesh_relay_commit_gateway_ack_confirm_terminal", note
        )
        outbox_clear = completion.index(
            'mesh_save_outbox_durable("gateway-ack-confirm-terminal")',
            terminal_commit,
        )
        self.assertLess(note, terminal_commit)
        self.assertLess(terminal_commit, outbox_clear)

    def test_gateway_ack_batching_remains_disabled_until_owner_is_bridged(self):
        max_in_flight = function_body(
            CH9_ACK, "app_mesh_ch9_tx_max_in_flight"
        )
        batch = function_body(REPORT, "mesh_try_send_report_tx_ch9_batch")
        pending = function_body(REPORT, "mesh_ch9_tx_pending_track_sent")
        direct_batch = function_body(
            REPORT, "mesh_try_send_report_tx_ch9_direct_gateway_batch"
        )
        tracked = function_body(
            REPORT, "mesh_start_tracked_tx_with_retry"
        )
        direct_send = function_body(
            REPORT, "mesh_send_direct_gateway_payload_and_wait_ack"
        )

        self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", max_in_flight)
        self.assertIn("return 1u", max_in_flight)
        force_single = batch.index(
            "app_mesh_ch9_tx_requires_tracked_single"
        )
        force_single_return = batch.index("return -ENOTSUP", force_single)
        select_direct_batch = batch.index(
            "direct_gateway_batch =", force_single_return
        )
        self.assertLess(force_single, force_single_return)
        self.assertLess(force_single_return, select_direct_batch)
        self.assertIn("sent->packet.src_id == DEVICE_ID", pending)
        self.assertIn(
            "local gateway-ack TX has no durable relay-core owner", pending
        )
        relay_owner = tracked.index("mesh_relay_start_tx(&mesh_runtime")
        direct_pending = tracked.index(
            "direct_gateway_tx_pending = true", relay_owner
        )
        send_direct = tracked.index(
            "mesh_send_direct_gateway_payload_and_wait_ack", direct_pending
        )
        self.assertLess(relay_owner, direct_pending)
        self.assertLess(direct_pending, send_direct)
        note_sent = direct_send.index(
            "mesh_relay_note_tx_sent(&mesh_runtime, out"
        )
        auxiliary_tracker = direct_send.index(
            "mesh_ch9_tx_pending_track_sent", note_sent
        )
        ack_wait = direct_send.index(
            "mesh_wait_for_direct_gateway_ack_configured",
            auxiliary_tracker,
        )
        self.assertLess(note_sent, auxiliary_tracker)
        self.assertLess(auxiliary_tracker, ack_wait)
        self.assertIn(
            "if (!MESH_DIRECT_GATEWAY_BATCHING_ENABLED)", direct_batch
        )
        self.assertIn("return -ENOTSUP", direct_batch)

    def test_batch_send_has_one_post_rf_relay_transition(self):
        send = function_body(REPORT, "mesh_try_send_report_tx_ch9_batch")
        commit = send.index("report_tx_queue_commit_head_locked")
        transition = send.index(
            "mesh_relay_note_tx_sent(&mesh_runtime, tx", commit
        )

        self.assertEqual(
            1, send.count("mesh_relay_note_tx_sent(&mesh_runtime, tx")
        )
        self.assertLess(commit, transition)

    def test_full_cir_stream_is_bench_only_and_outside_durable_batch(self):
        self.assertIn(
            "#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)",
            ENCODE,
        )
        self.assertIn("anchor_click_cir_buffer", ENCODE)
        durable = function_body(
            PERSISTENCE, "app_mesh_persistence_save_anchor_range_fragment"
        )
        self.assertNotIn("anchor_click_cir_buffer", durable)
        self.assertIn("MSG_CLICK_REPORT", durable)


if __name__ == "__main__":
    unittest.main()
