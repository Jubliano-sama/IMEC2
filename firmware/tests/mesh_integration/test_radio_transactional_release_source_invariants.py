#!/usr/bin/env python3
"""Keep radio release owned until the physical DWM3000 park succeeds."""

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
RADIO_GUARD = (ROOT / "app" / "src" / "app_radio_guard.c").read_text(
    encoding="utf-8"
)
STATE_HEADER = (ROOT / "app" / "src" / "app_state.h").read_text(
    encoding="utf-8"
)
TRANSPORT = (ROOT / "app" / "src" / "app_mesh_report_transport.inc").read_text(
    encoding="utf-8"
)
RX = (ROOT / "app" / "src" / "app_mesh_report_rx.inc").read_text(
    encoding="utf-8"
)
ANCHOR = (ROOT / "app" / "src" / "app_anchor_radio.inc").read_text(
    encoding="utf-8"
)
ANCHOR_APP = (ROOT / "app" / "src" / "app_anchor.c").read_text(
    encoding="utf-8"
)
ANCHOR_CLICK_EVENT_RUNTIME = (
    ROOT / "app" / "src" / "app_anchor_click_event_runtime.c"
).read_text(encoding="utf-8")
COORDINATION = (
    ROOT / "app" / "src" / "app_mesh_report_coordination.inc"
).read_text(encoding="utf-8")
DIRECT_GATEWAY = (
    ROOT / "app" / "src" / "app_mesh_report_direct_gateway.inc"
).read_text(encoding="utf-8")
ROUTE_CONTROL = (
    ROOT / "app" / "src" / "app_mesh_report_route_control.inc"
).read_text(encoding="utf-8")
SURVEY_RUNTIME = (
    ROOT / "app" / "src" / "app_anchor_survey_runtime.c"
).read_text(encoding="utf-8")
CLICKER = (ROOT / "app" / "src" / "app_clicker.c").read_text(
    encoding="utf-8"
)
MESH_TEST = (ROOT / "app" / "src" / "app_mesh_test.c").read_text(
    encoding="utf-8"
)
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function {name}")


def braced_block_after(source: str, marker: str, start: int = 0) -> str:
    marker_index = source.index(marker, start)
    brace = source.index("{", marker_index)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated block after {marker}")


class RadioTransactionalReleaseSourceInvariantTests(unittest.TestCase):
    def test_guard_keeps_the_exact_lease_after_failed_parking(self) -> None:
        claim = function_body(RADIO_GUARD, "radio_guard_uwb_claim")
        begin = function_body(RADIO_GUARD, "radio_guard_uwb_release_begin")
        finish = function_body(RADIO_GUARD, "radio_guard_uwb_release_finish")

        self.assertIn("radio_guard_uwb_next_generation()", claim)
        self.assertIn("radio_guard_uwb_lease_matches(lease)", begin)
        self.assertIn("static struct k_spinlock uwb_rf_lock;", RADIO_GUARD)
        self.assertNotIn("uwb_rf_active", RADIO_GUARD)
        self.assertNotIn("uwb_rf_active", STATE_HEADER)
        self.assertNotIn("uwb_rf_lock", STATE_HEADER)
        poison = finish.index("uwb_rf_phase = RADIO_GUARD_UWB_POISONED")
        retain = finish.index("uwb_rf_poison_error = parking_result", poison)
        cleanup = finish.index("memset(&uwb_rf_owner", retain)
        watchdog = finish.index("app_watchdog_stop_feeding()")

        self.assertLess(poison, retain)
        self.assertLess(retain, cleanup)
        self.assertGreater(watchdog, cleanup)
        self.assertIn(
            "app_watchdog_stop_feeding()",
            braced_block_after(finish, "if (poisoned)"),
        )
        self.assertIn("uwb_rf_phase != RADIO_GUARD_UWB_POISONED", RADIO_GUARD)

    def test_legacy_stop_cannot_clear_a_scoped_lease(self) -> None:
        stop = function_body(RADIO_GUARD, "radio_guard_uwb_stop")

        self.assertIn("if (legacy_lease.generation == 0u)", stop)
        self.assertIn("radio_guard_uwb_release_begin(&legacy_lease)", stop)
        self.assertIn("radio_guard_uwb_release_finish(&legacy_lease, 0)", stop)
        self.assertNotIn("uwb_rf_active = false", stop)

    def test_migrated_adapters_do_not_use_unscoped_stop(self) -> None:
        for source in (
            TRANSPORT,
            RX,
            ANCHOR,
            DIRECT_GATEWAY,
            ROUTE_CONTROL,
            SURVEY_RUNTIME,
            CLICKER,
        ):
            self.assertNotIn("radio_guard_uwb_stop(", source)

    def test_normal_anchor_elides_role_inapplicable_radio_adjacent_state(self) -> None:
        clicker_init = function_body(CLICKER, "app_clicker_init")

        self.assertRegex(
            clicker_init,
            r"#if DEVICE_ROLE == ROLE_CLICKER\s+"
            r"app_clicker_event_runtime_init\(&clicker_event_runtime\);\s+"
            r"#endif",
        )
        self.assertIn(
            "#if DEVICE_ROLE == ROLE_GATEWAY || \\\n"
            "    defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)\n"
            "#define MESH_TEST_SMOKE_STATE_ENABLED 1\n"
            "#else\n"
            "#define MESH_TEST_SMOKE_STATE_ENABLED 0\n"
            "#endif",
            MESH_TEST,
        )
        declaration = "static struct mesh_smoke_fast_state mesh_test_gateway_state;"
        declaration_at = MESH_TEST.index(declaration)
        self.assertEqual(
            "#if MESH_TEST_SMOKE_STATE_ENABLED",
            MESH_TEST[MESH_TEST.rfind("#if", 0, declaration_at):declaration_at]
            .strip(),
        )
        for call in (
            "mesh_smoke_fast_note_c5_refresh(&mesh_test_gateway_state);",
            "mesh_smoke_fast_note_ch9_missed(&mesh_test_gateway_state);",
        ):
            for match in re.finditer(re.escape(call), MESH_TEST):
                guard = MESH_TEST.rfind("#if MESH_TEST_SMOKE_STATE_ENABLED", 0,
                                        match.start())
                end = MESH_TEST.find("#endif", guard)
                self.assertGreaterEqual(guard, 0)
                self.assertGreater(end, match.start())

        wake_event = function_body(MESH_TEST, "app_mesh_test_note_wake_event")
        self.assertIn("anchor_uwb_scan_interval_ms = target_scan_interval_ms;",
                      wake_event)
        self.assertIn("mesh-test wake event refreshed anchor channel-5 scan",
                      wake_event)

    def test_clicker_parks_between_scoped_release_phases(self) -> None:
        for name, parking_call in (
            ("clicker_release_radio_to_standby", "app_radio_standby_with_bounded_recovery"),
            ("clicker_release_radio_to_idle", "app_radio_idle_with_bounded_recovery"),
        ):
            helper = function_body(CLICKER, name)
            begin = helper.index("radio_guard_uwb_release_begin(radio_lease)")
            park = helper.index(parking_call, begin)
            finish = helper.index("radio_guard_uwb_release_finish(radio_lease, parking_ret)", park)

            self.assertLess(begin, park)
            self.assertLess(park, finish)

    def test_mesh_direct_and_route_adapters_hold_scoped_leases(self) -> None:
        direct_payload = function_body(
            DIRECT_GATEWAY, "mesh_send_direct_gateway_payload_and_wait_ack"
        )
        direct_probe = function_body(
            DIRECT_GATEWAY, "mesh_send_direct_gateway_probe_and_wait"
        )

        self.assertIn("RADIO_GUARD_UWB_CLIENT_MESH_TX", direct_payload)
        self.assertLess(
            direct_payload.index("mesh_transport_radio_claim("),
            direct_payload.index("mesh_transport_radio_finish("),
        )
        payload_finish = direct_payload.index("mesh_transport_radio_finish(")
        payload_failure = direct_payload.index("if (release_ret < 0)", payload_finish)
        payload_failure_block = braced_block_after(
            direct_payload, "if (release_ret < 0)", payload_finish
        )
        payload_rearm = direct_payload.index("mesh_restart_role_scan()", payload_failure)

        self.assertIn("return release_ret;", payload_failure_block)
        self.assertLess(payload_failure, payload_rearm)
        self.assertNotIn("mesh_restart_role_scan()", payload_failure_block)
        self.assertEqual(direct_probe.count("mesh_transport_radio_finish("), 2)
        self.assertIn("direct-gateway-probe-c5-yield", direct_probe)
        self.assertIn("return release_ret;", direct_probe)

        for name, client in (
            ("mesh_listen_for_route_reply_ack", "RADIO_GUARD_UWB_CLIENT_MESH_RX"),
            ("mesh_send_route_wake_train", "RADIO_GUARD_UWB_CLIENT_MESH_TX"),
            ("mesh_listen_for_route_reply", "RADIO_GUARD_UWB_CLIENT_MESH_RX"),
        ):
            body = function_body(ROUTE_CONTROL, name)
            self.assertIn(client, body)
            self.assertLess(
                body.index("mesh_transport_radio_claim("),
                body.index("mesh_transport_radio_finish("),
            )

    def test_survey_runtime_parks_before_clearing_owner_or_retrying(self) -> None:
        worker = function_body(SURVEY_RUNTIME, "survey_work_handler")
        release_helper = function_body(SURVEY_RUNTIME, "survey_radio_release")

        self.assertIn("radio_guard_uwb_release_begin(lease)", release_helper)
        self.assertIn("radio_guard_uwb_release_finish(lease, parking_ret)", release_helper)
        self.assertEqual(
            worker.count("RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY"), 2
        )

        discovery_release = worker.index(
            "release_ret = survey_radio_release(&radio_lease, low_power_ret)"
        )
        discovery_failure = worker.index("if (release_ret < 0)", discovery_release)
        discovery_failure_block = braced_block_after(
            worker, "if (release_ret < 0)", discovery_release
        )
        discovery_clear = worker.index(
            "runtime_ops.set_uwb_busy(false)", discovery_failure
        )
        discovery_rearm = worker.index(
            "app_node_comm_restart_role_scan()", discovery_clear
        )

        self.assertIn("return;", discovery_failure_block)
        self.assertLess(discovery_release, discovery_failure)
        self.assertLess(discovery_failure, discovery_clear)
        self.assertLess(discovery_clear, discovery_rearm)
        self.assertNotIn("runtime_ops.set_uwb_busy(false)", discovery_failure_block)

        pair_release = worker.rindex(
            "release_ret = survey_radio_release(&radio_lease, low_power_ret)"
        )
        pair_failure = worker.index("if (release_ret < 0)", pair_release)
        pair_failure_block = braced_block_after(
            worker, "if (release_ret < 0)", pair_release
        )
        pair_clear = worker.index("runtime_ops.set_uwb_busy(false)", pair_failure)
        pair_rearm = worker.index("app_node_comm_restart_role_scan()", pair_clear)

        self.assertIn("return;", pair_failure_block)
        self.assertLess(pair_release, pair_failure)
        self.assertLess(pair_failure, pair_clear)
        self.assertLess(pair_clear, pair_rearm)
        self.assertNotIn("runtime_ops.set_uwb_busy(false)", pair_failure_block)

        poison_discovery = worker.index("if (radio_guard_uwb_poisoned())")
        retry_discovery = worker.index("survey_rf_retry_delay_ms(", poison_discovery)
        poison_pair = worker.index(
            "if (radio_guard_uwb_poisoned())", poison_discovery + 1
        )
        retry_pair = worker.index("schedule_pair_rf_retry(", poison_pair)

        self.assertLess(poison_discovery, retry_discovery)
        self.assertLess(poison_pair, retry_pair)

        self.assertIn(
            "struct app_node_comm_reservation_lease delivery_reservation_leases",
            worker,
        )
        reserve = worker.index("app_node_comm_reserve_durable_reliable_uplinks(")
        reserve_call = worker[reserve:worker.index("if (ret < 0)", reserve)]
        self.assertIn("pair.operation_generation", reserve_call)
        self.assertIn("delivery_reservation_leases", reserve_call)
        self.assertLess(
            reserve_call.index("pair.operation_generation"),
            reserve_call.index("pair.sample_count"),
        )

        poison_pair_block = braced_block_after(
            worker, "if (radio_guard_uwb_poisoned())", poison_pair
        )
        self.assertIn(
            "app_anchor_survey_result_delivery_cancel_reservations(",
            poison_pair_block,
        )
        self.assertIn("delivery_reservation_leases", poison_pair_block)
        self.assertIn("return;", poison_pair_block)

        exit_cancel = worker.rindex(
            "app_anchor_survey_result_delivery_cancel_reservations("
        )
        self.assertLess(exit_cancel, pair_release)
        for reason in (
            '"lease-race-radio-release"',
            '"pair-commitment-missing-radio-release"',
            '"pair-role-race-radio-release"',
        ):
            self.assertIn(reason, worker)

    def test_transport_never_rearms_before_tx_parking_commits(self) -> None:
        send = function_body(
            TRANSPORT, "mesh_send_outbound_with_release_on_channel_until"
        )
        claim = send.index("mesh_transport_radio_claim(")
        begin = send.index("radio_guard_uwb_release_begin(&radio_lease)", claim)
        parking = send.index("parking_ret =", begin)
        finish = send.index(
            "radio_guard_uwb_release_finish(&radio_lease, parking_ret)",
            parking,
        )
        failure = send.index("if (release_ret < 0)", finish)
        restart = send.index("mesh_restart_role_scan()", failure)
        failure_block = braced_block_after(send, "if (release_ret < 0)", finish)

        self.assertLess(claim, begin)
        self.assertLess(begin, parking)
        self.assertLess(parking, finish)
        self.assertLess(finish, failure)
        self.assertLess(failure, restart)
        self.assertIn("ret = release_ret", failure_block)
        self.assertIn("goto out_unlock", failure_block)
        self.assertNotIn("mesh_restart_role_scan()", failure_block)

    def test_rx_parking_failure_returns_before_route_handoff(self) -> None:
        worker = function_body(REPORT, "mesh_uwb_rx_work_handler")
        finish = worker.rindex(
            "radio_release_ret = mesh_rx_radio_finish(&radio_lease, radio_release_ret)"
        )
        failure = worker.index("if (radio_release_ret < 0)", finish)
        failure_block = braced_block_after(worker, "if (radio_release_ret < 0)", finish)
        handoff = worker.index("if (route_control_handoff)", failure)
        failure_return = worker.index("return;", failure)

        self.assertLess(finish, failure)
        self.assertLess(failure, failure_return)
        self.assertLess(failure_return, handoff)
        self.assertIn("gateway_command_result_validation_release_reserved", failure_block)
        self.assertIn("mesh_uwb_rx_active = false", failure_block)
        self.assertNotIn("mesh_restart_role_scan()", worker[finish:handoff])

    def test_rx_handoff_scan_ends_only_after_the_physical_release(self) -> None:
        finish = function_body(RX, "mesh_rx_radio_finish")
        release = finish.index("mesh_transport_radio_finish(lease, parking_ret)")
        end_scan = finish.index("app_mesh_rx_handoff_end_scan", release)

        self.assertLess(release, end_scan)

    def test_anchor_click_and_scan_fail_closed_before_rearm_or_handoff(self) -> None:
        click = function_body(ANCHOR, "anchor_run_mesh_click_wake_claim")
        scan = function_body(ANCHOR, "anchor_uwb_scan_work_handler")

        self.assertIn("int low_power_ret = -EIO;", scan)

        click_begin = click.index("radio_guard_uwb_release_begin(radio_lease)")
        click_park = click.index("anchor_enter_low_power(", click_begin)
        click_finish = click.index("radio_guard_uwb_release_finish(radio_lease", click_park)
        click_failure = click.index("if (release_ret < 0)", click_finish)
        click_failure_block = braced_block_after(
            click, "if (release_ret < 0)", click_finish
        )
        click_clear = click.index("anchor_set_uwb_busy(false)", click_failure)
        click_schedule = click.index("anchor_uwb_scan_schedule_ms", click_clear)

        self.assertLess(click_begin, click_park)
        self.assertLess(click_park, click_finish)
        self.assertLess(click_finish, click_failure)
        self.assertLess(click_failure, click_clear)
        self.assertLess(click_clear, click_schedule)
        self.assertIn("return false;", click_failure_block)
        self.assertNotIn("anchor_set_uwb_busy(false)", click_failure_block)
        self.assertNotIn("anchor_uwb_scan_schedule_ms", click_failure_block)

        scan_begin = scan.index("radio_guard_uwb_release_begin(&radio_lease)")
        scan_park = scan.index("anchor_enter_low_power(", scan_begin)
        scan_finish = scan.index("radio_guard_uwb_release_finish(&radio_lease", scan_park)
        scan_failure = scan.index("if (release_ret < 0)", scan_finish)
        scan_failure_block = braced_block_after(
            scan, "if (release_ret < 0)", scan_finish
        )
        scan_clear = scan.index("anchor_set_uwb_busy(false)", scan_failure)
        scan_handoff = scan.index("if (route_wake_handoff)", scan_clear)

        self.assertLess(scan_begin, scan_park)
        self.assertLess(scan_park, scan_finish)
        self.assertLess(scan_finish, scan_failure)
        self.assertLess(scan_failure, scan_clear)
        self.assertLess(scan_clear, scan_handoff)
        self.assertIn("return;", scan_failure_block)
        self.assertNotIn("anchor_set_uwb_busy(false)", scan_failure_block)
        self.assertNotIn("mesh_anchor_handoff_route_wake_frame", scan_failure_block)

    def test_click_handoff_reserves_and_transfers_the_lease_before_queueing(self) -> None:
        handoff = function_body(ANCHOR, "anchor_handle_mesh_click_wake_claim")
        worker = function_body(ANCHOR, "anchor_click_handoff_work_handler")
        scan = function_body(ANCHOR, "anchor_uwb_scan_work_handler")
        pre_rf_release = function_body(
            ANCHOR, "anchor_release_unstarted_click_lease"
        )

        claim = handoff.index("radio_guard_uwb_claim(")
        phase_claim = handoff.index(
            "app_anchor_click_event_runtime_claim(", claim
        )
        reserve = handoff.index(
            "mesh_range_report_batch_reserve_capacity(", phase_claim
        )
        transfer = handoff.index(
            "anchor_pending_click_handoff.radio_lease = radio_lease", reserve
        )
        submit = handoff.index("k_work_submit_to_queue(", transfer)
        queue_failure = handoff.index(
            'anchor_release_unstarted_click_lease(&radio_lease,\n                                             "click-handoff-queue")',
            submit,
        )

        self.assertLess(claim, reserve)
        self.assertLess(claim, phase_claim)
        self.assertLess(phase_claim, reserve)
        self.assertLess(reserve, transfer)
        self.assertLess(transfer, submit)
        self.assertLess(submit, queue_failure)
        self.assertIn("radio_guard_uwb_release_begin(radio_lease)", pre_rf_release)
        self.assertIn(
            "radio_guard_uwb_release_finish(radio_lease, 0)", pre_rf_release
        )
        self.assertNotIn("radio_guard_uwb_claim(", worker)
        self.assertNotIn("mesh_preempt_for_click_event(", worker)
        self.assertNotIn("mesh_range_report_batch_reserve_capacity(", worker)
        self.assertIn("&pending.radio_lease", worker)
        self.assertIn("radio_generation = pending.radio_lease.generation", worker)
        self.assertLess(
            scan.index("if (anchor_click_handoff_pending() || anchor_click_window_active())"),
            scan.index("radio_guard_uwb_claim("),
        )
        self.assertIn(
            "if (!anchor_click_handoff_pending()) {\n"
            "        anchor_click_window_set_active(false);",
            scan,
        )
        self.assertIn("struct radio_guard_uwb_lease radio_lease;", ANCHOR_APP)

    def test_fast_click_detaches_old_phase_before_capacity_and_cleans_failures(self) -> None:
        handoff = function_body(ANCHOR, "anchor_handle_mesh_click_wake_claim")
        runtime_claim = function_body(
            ANCHOR_CLICK_EVENT_RUNTIME,
            "app_anchor_click_event_runtime_claim",
        )
        custody_release = function_body(
            ANCHOR_CLICK_EVENT_RUNTIME,
            "app_anchor_click_event_runtime_custody_released",
        )

        pending_duplicate = braced_block_after(
            handoff, "if (anchor_pending_click_handoff.active)"
        )
        radio_claim = handoff.index("radio_guard_uwb_claim(")
        phase_claim = handoff.index(
            "app_anchor_click_event_runtime_claim(", radio_claim
        )
        phase_failure = handoff.index("if (ret < 0)", phase_claim)
        phase_failure_block = braced_block_after(
            handoff, "if (ret < 0)", phase_claim
        )
        reserve = handoff.index(
            "mesh_range_report_batch_reserve_capacity(", phase_failure
        )
        reserve_failure = handoff.index("if (ret < 0)", reserve)
        reserve_failure_block = braced_block_after(
            handoff, "if (ret < 0)", reserve
        )
        transfer = handoff.index(
            "anchor_pending_click_handoff.radio_lease = radio_lease", reserve
        )
        replaced = handoff.index("if (!pending_matches)", transfer)
        replaced_block = braced_block_after(
            handoff, "if (!pending_matches)", transfer
        )
        submit = handoff.index("k_work_submit_to_queue(", replaced)
        queue_abort = handoff.index("mesh_range_report_batch_abort(", submit)
        queue_phase_abort = handoff.index(
            "anchor_click_event_abort_if_needed(", queue_abort
        )
        queue_release = handoff.index(
            "anchor_release_unstarted_click_lease(", queue_phase_abort
        )

        self.assertNotIn("radio_guard_uwb_claim(", pending_duplicate)
        self.assertNotIn(
            "app_anchor_click_event_runtime_claim(", pending_duplicate
        )
        self.assertNotIn(
            "mesh_range_report_batch_reserve_capacity(", pending_duplicate
        )
        self.assertIn("return true;", pending_duplicate)
        self.assertLess(radio_claim, phase_claim)
        self.assertLess(phase_claim, phase_failure)
        self.assertLess(phase_failure, reserve)
        self.assertIn(
            "anchor_release_unstarted_click_lease(", phase_failure_block
        )
        self.assertNotIn("mesh_range_report_batch_abort(", phase_failure_block)

        self.assertLess(reserve, reserve_failure)
        self.assertLess(reserve_failure, transfer)
        self.assertIn(
            'anchor_click_event_abort_if_needed("click-report-capacity")',
            reserve_failure_block,
        )
        self.assertIn(
            "anchor_release_unstarted_click_lease(", reserve_failure_block
        )
        self.assertIn("return false;", reserve_failure_block)
        self.assertNotIn("mesh_range_report_batch_abort(", reserve_failure_block)
        for cleanup in (
            "mesh_range_report_batch_abort(",
            "anchor_click_event_abort_if_needed(",
            "anchor_release_unstarted_click_lease(",
        ):
            self.assertIn(cleanup, replaced_block)
        self.assertLess(submit, queue_abort)
        self.assertLess(queue_abort, queue_phase_abort)
        self.assertLess(queue_phase_abort, queue_release)

        # The phase owner detaches RESULT_OWNED without reaching into the
        # independent report queue/relay custody. Replaying the accepted key
        # is a no-op, and a delayed old transport completion cannot mutate a
        # successor phase.
        exact_replay = runtime_claim.index("if (claim_key_matches(")
        replay_return = runtime_claim.index("return 0;", exact_replay)
        result_owned = runtime_claim.index(
            "if (app_anchor_click_event_runtime_result_owned())", replay_return
        )
        detach = runtime_claim.index(
            "FW_EVENT_RESULT_CUSTODY_RELEASED", result_owned
        )
        successor_generation = runtime_claim.index(
            "runtime.next_generation++", detach
        )
        self.assertLess(exact_replay, replay_return)
        self.assertLess(replay_return, result_owned)
        self.assertLess(result_owned, detach)
        self.assertLess(detach, successor_generation)
        self.assertNotIn("mesh_range_report", runtime_claim)
        self.assertNotIn("report_tx", runtime_claim)
        detached = custody_release.index(
            "runtime.machine.state != FW_ANCHOR_CLICK_RESULT_OWNED"
        )
        detached_return = custody_release.index("return 0;", detached)
        identity_check = custody_release.index("if (!runtime.key_valid", detached_return)
        self.assertLess(detached, detached_return)
        self.assertLess(detached_return, identity_check)

    def test_reserved_handoff_freezes_claim_identity_during_collection(self) -> None:
        collection = function_body(ANCHOR, "anchor_handle_uwb_claim")
        worker = function_body(ANCHOR, "anchor_run_mesh_click_wake_claim")
        scan = function_body(ANCHOR, "anchor_uwb_scan_work_handler")

        guard = collection.index("anchor_claim_collection_candidate_allowed(")
        candidate_accept = collection.index("uwb_anchor_accept_wake_claim(", guard)
        guard_block = braced_block_after(
            collection,
            "if (!anchor_claim_collection_candidate_allowed(",
            guard - 8,
        )

        self.assertIn("admitted_handoff_identity_frozen", collection)
        self.assertIn("continue;", guard_block)
        self.assertLess(guard, candidate_accept)
        self.assertIn(
            "received_at_ms,\n                                      true,",
            worker,
        )
        self.assertIn(
            "k_uptime_get(),\n                                        false,",
            scan,
        )

    def test_ack_repair_cannot_override_click_survey_or_gateway_rx(self) -> None:
        coordinator = function_body(
            COORDINATION, "mesh_coordinator_c5_tx_allowed_authorized_intent"
        )
        normal_admission = coordinator.index("if (decision.c5_tx_allowed)")
        exact_rx_owner = coordinator.index(
            "exact_ack_rx_repair_state =", normal_admission
        )
        repair_state_gate = coordinator.index(
            "if (decision.state != FW_RADIO_ACTIVITY_MESH_TX &&",
            exact_rx_owner,
        )
        authorization = coordinator.index(
            "if (authorization != NULL && authorization->valid)", repair_state_gate
        )
        repair = coordinator.index("app_mesh_ch9_c5_repair_allowed(", authorization)
        gate_block = braced_block_after(
            coordinator,
            "if (decision.state != FW_RADIO_ACTIVITY_MESH_TX &&",
            normal_admission,
        )

        self.assertLess(normal_admission, repair_state_gate)
        self.assertLess(repair_state_gate, authorization)
        self.assertLess(authorization, repair)
        self.assertIn("capture.rx_queue_used == 0u", coordinator)
        self.assertIn("!capture.click_active", coordinator)
        self.assertIn("!capture.survey_pending", coordinator)
        self.assertIn("!capture.gateway_continuous_ch9", coordinator)
        self.assertIn(
            "!exact_ack_rx_repair_state",
            coordinator[repair_state_gate:authorization],
        )
        self.assertIn("return false;", gate_block)

    def test_rearm_gate_lives_at_both_mesh_and_anchor_schedulers(self) -> None:
        restart = function_body(TRANSPORT, "mesh_restart_role_scan")
        schedule = function_body(ANCHOR, "anchor_uwb_scan_schedule_ms")

        self.assertIn("if (!radio_guard_uwb_rearm_allowed())", restart)
        self.assertIn("if (!radio_guard_uwb_rearm_allowed())", schedule)


if __name__ == "__main__":
    unittest.main()
