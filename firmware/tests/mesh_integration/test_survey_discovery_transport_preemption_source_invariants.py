#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text(
    encoding="utf-8"
)
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text(
    encoding="utf-8"
)
DISCOVERY_HEADER = (ROOT / "app/src/app_anchor_survey_discovery.h").read_text(
    encoding="utf-8"
)
ANCHOR_INIT = (ROOT / "app/src/app_anchor_init.inc").read_text(
    encoding="utf-8"
)
ANCHOR_COMMANDS = (ROOT / "app/src/app_anchor_commands.inc").read_text(
    encoding="utf-8"
)
NODE_COMM = (ROOT / "app/src/app_node_comm.c").read_text(encoding="utf-8")
NODE_COMM_HEADER = (ROOT / "app/src/app_node_comm.h").read_text(
    encoding="utf-8"
)
ROUTE_CONTROL = (ROOT / "app/src/app_mesh_report_route_control.inc").read_text(
    encoding="utf-8"
)
TRANSPORT = (ROOT / "app/src/app_mesh_report_transport.inc").read_text(
    encoding="utf-8"
)
CONFIG = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")


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


def braced_block_after(source: str, marker: str, start_at: int = 0) -> str:
    marker_at = source.index(marker, start_at)
    start = source.index("{", marker_at)
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated block after: {marker}")


class SurveyDiscoveryTransportPreemptionSourceInvariantTests(unittest.TestCase):
    def test_admission_keeps_mesh_live_until_the_physical_prep_boundary(self):
        admission = function_body(
            DISCOVERY, "app_anchor_survey_discovery_handle_start"
        )

        self.assertNotIn("preempt_radio", DISCOVERY_HEADER)
        self.assertNotIn("preempt_radio", admission)
        self.assertNotIn("anchor_preempt_for_survey_discovery", ANCHOR_INIT)
        self.assertNotIn("anchor_preempt_for_survey_discovery", ANCHOR_COMMANDS)
        for early_preemption in (
            "app_node_comm_stop_role_scan",
            "k_work_cancel_delayable(&anchor_uwb_scan_work)",
            "uwb_anchor_abort_epoch",
            "survey_transport_preempt_begin",
        ):
            with self.subTest(early_preemption=early_preemption):
                self.assertNotIn(early_preemption, admission)

        abort_pair = admission.index("discovery_ops.abort_pair()")
        queue = admission.index("discovery_ops.queue_start(", abort_pair)
        self.assertLess(abort_pair, queue)

    def test_facade_is_declared_and_used_only_at_physical_survey_prep(self):
        admission = function_body(
            DISCOVERY, "app_anchor_survey_discovery_handle_start"
        )
        worker = function_body(RUNTIME, "survey_work_handler")
        discovery_run = braced_block_after(worker, "if (run_discovery)")

        for api in (
            "app_node_comm_transport_preempt_begin",
            "app_node_comm_transport_preempt_ready",
            "app_node_comm_transport_preempt_end",
        ):
            with self.subTest(api=api):
                self.assertRegex(NODE_COMM_HEADER, rf"\b{api}\s*\(")
                self.assertNotIn(api, admission)

        preempt = function_body(
            RUNTIME, "survey_transport_preempt_begin"
        )
        preempt_end = function_body(
            RUNTIME, "survey_transport_preempt_end"
        )
        begin_facade = preempt.index("app_node_comm_transport_preempt_begin()")
        ready_facade = preempt.index(
            "app_node_comm_transport_preempt_ready()", begin_facade
        )
        self.assertLess(begin_facade, ready_facade)
        self.assertIn("app_node_comm_transport_preempt_end()", preempt_end)
        for transport_detail in (
            "mesh_transport_pause_preserving_queued",
            "mesh_transport_quiesced",
            "mesh_transport_resume",
            "dwm3000_driver_request_receive_abort",
            "dwm3000_driver_clear_receive_abort",
            "radio_guard_uwb_admission_resume",
        ):
            with self.subTest(runtime_detail=transport_detail):
                self.assertNotIn(transport_detail, preempt)
                self.assertNotIn(transport_detail, preempt_end)

        deadline = discovery_run.index("survey_discovery_radio_deadline_ms(")
        stop_scan = discovery_run.index("app_node_comm_stop_role_scan()", deadline)
        begin = discovery_run.index(
            "survey_transport_preempt_begin()", stop_scan
        )
        claim = discovery_run.index(
            "radio_guard_uwb_claim(", begin
        )
        self.assertLess(deadline, stop_scan)
        self.assertLess(stop_scan, begin)
        self.assertLess(begin, claim)
        self.assertIn("if (ret == 0)", discovery_run[begin:claim])

    def test_begin_serializes_with_lifecycle_and_preserves_transport_custody(self):
        begin = function_body(
            NODE_COMM, "app_node_comm_transport_preempt_begin"
        )

        lock = begin.index("app_node_comm_sync_lock()")
        lifecycle = begin.index("node_comm_state(", lock)
        preexisting_pause = begin.index(
            "mesh_transport_pause_active()", lifecycle
        )
        pause = begin.index(
            "mesh_transport_pause_preserving_queued()", preexisting_pause
        )
        unlock = begin.index("app_node_comm_sync_unlock()", pause)
        self.assertLess(lock, lifecycle)
        self.assertLess(lifecycle, preexisting_pause)
        self.assertLess(preexisting_pause, pause)
        self.assertLess(pause, unlock)
        self.assertIn("NODE_COMM_RUNNING", begin[lock:pause])
        self.assertIn("node_comm_backend_ready", begin[lock:pause])
        for custody_mutation in (
            "mesh_relay_cancel_tx",
            "k_msgq_get",
            "report_tx_queue",
        ):
            with self.subTest(mutation=custody_mutation):
                self.assertNotIn(custody_mutation, begin)

    def test_ready_aborts_only_exact_mesh_owners_then_reopens_at_quiescence(self):
        ready = function_body(
            NODE_COMM, "app_node_comm_transport_preempt_ready"
        )

        request = ready.index("dwm3000_driver_request_receive_abort(")
        owner_gate = ready.rfind("if (", 0, request)
        request_context = ready[owner_gate:request]
        self.assertIn("RADIO_GUARD_UWB_CLIENT_MESH_RX", request_context)
        self.assertIn("RADIO_GUARD_UWB_CLIENT_MESH_TX", request_context)
        self.assertIn("anchor_uwb_window_active()", request_context)
        self.assertIn("anchor_click_window_active()", request_context)
        self.assertIn(
            "DWM3000_RECEIVE_ABORT_MESH_CONTROL",
            ready[request : request + 160],
        )
        self.assertNotIn("DWM3000_RECEIVE_ABORT_NODE_COMM", ready)
        self.assertNotIn("DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY", ready)

        quiesced = ready.index("mesh_transport_quiesced()")
        clear = ready.index(
            "dwm3000_driver_clear_receive_abort(", quiesced
        )
        clear_owner = ready.index(
            "DWM3000_RECEIVE_ABORT_MESH_CONTROL", clear
        )
        reopen = ready.index("radio_guard_uwb_admission_resume()", clear_owner)
        self.assertLess(quiesced, clear)
        self.assertLess(clear_owner, reopen)
        self.assertNotIn("mesh_transport_resume", ready)

    def test_quiescence_parks_retry_custody_but_waits_for_physical_response(self):
        quiesced = function_body(TRANSPORT, "mesh_transport_quiesced")

        self.assertIn("mesh_transport_paused()", quiesced)
        self.assertIn("!radio_guard_uwb_busy()", quiesced)
        self.assertIn(
            "atomic_get(&mesh_rx_response_active_state) == 0", quiesced
        )
        self.assertIn(
            "atomic_get(&mesh_rx_handler_active_state) == 0", quiesced
        )
        self.assertNotIn("mesh_rx_response_active()", quiesced)

    def test_route_reply_listener_releases_receive_abort_before_noise_recovery(self):
        listen = function_body(ROUTE_CONTROL, "mesh_listen_for_route_reply")

        receive = listen.index("dwm3000_driver_receive_frame_continuous(")
        record = listen.index("last_ret = ret;", receive)
        timeout = listen.index("if (ret == -ETIMEDOUT)", record)
        abort = listen.index(
            "if (app_mesh_c5_route_capture_receive_aborted(ret))", timeout
        )
        abort_block = braced_block_after(
            listen,
            "if (app_mesh_c5_route_capture_receive_aborted(ret))",
            timeout,
        )
        generic_failure = listen.index("if (ret < 0)", abort)

        self.assertLess(record, timeout)
        self.assertLess(timeout, abort)
        self.assertLess(abort, generic_failure)
        self.assertIn("DBG_ROUTE_REPLY_LISTEN_ABORTED", abort_block)
        self.assertIn("break;", abort_block)
        for forbidden in (
            "continue;",
            "mesh_probe_standard_wake_claim",
            "dwm3000_driver_last_rx_debug_get",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, abort_block)

    def test_active_wake_train_yields_at_frame_boundary_to_transport_pause(self):
        wake_train = function_body(ROUTE_CONTROL, "mesh_send_route_wake_train")
        loop = braced_block_after(
            wake_train, "while (k_uptime_get() < close_ms)"
        )

        pause = loop.index("if (mesh_transport_paused())")
        build = loop.index("uwb_clicker_build_wake_claim(")
        first_send = loop.index("dwm3000_driver_send_frame(")
        pause_block = braced_block_after(
            loop, "if (mesh_transport_paused())"
        )
        self.assertLess(pause, build)
        self.assertLess(pause, first_send)
        self.assertIn("ret = -ESHUTDOWN;", pause_block)
        self.assertIn("break;", pause_block)
        for forbidden in (
            "mesh_relay_cancel_tx",
            "mesh_route_waiting_tx_valid = false",
            "mesh_c5_contact_accept",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, pause_block)

    def test_end_is_lifecycle_serialized_and_cannot_steal_a_pause(self):
        end = function_body(NODE_COMM, "app_node_comm_transport_preempt_end")

        lock = end.index("app_node_comm_sync_lock()")
        lifecycle = end.index("node_comm_state(", lock)
        resume = end.index("mesh_transport_resume()", lifecycle)
        unlock = end.index("app_node_comm_sync_unlock()", resume)
        self.assertLess(lock, lifecycle)
        self.assertLess(lifecycle, resume)
        self.assertLess(resume, unlock)
        resume_branch_start = end.index("if (node_comm_state(")
        resume_branch = braced_block_after(
            end, "if (node_comm_state(", resume_branch_start
        )
        self.assertIn(
            "NODE_COMM_RUNNING", end[resume_branch_start:resume]
        )
        self.assertIn(
            "node_comm_backend_ready", end[resume_branch_start:resume]
        )
        self.assertIn("mesh_transport_resume()", resume_branch)
        self.assertNotIn("mesh_transport_resume()", end[:resume_branch_start])
        self.assertNotIn("DWM3000_RECEIVE_ABORT_NODE_COMM", end)
        self.assertNotIn("DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY", end)

    def test_transport_resumes_only_after_successful_radio_release(self):
        worker = function_body(RUNTIME, "survey_work_handler")
        discovery_run = braced_block_after(worker, "if (run_discovery)")

        run = discovery_run.index("app_anchor_survey_discovery_run(")
        release = discovery_run.index("survey_radio_release(&radio_lease", run)
        release_failure = discovery_run.index("if (release_ret < 0)", release)
        fail_closed = discovery_run.index("return;", release_failure)
        clear_busy = discovery_run.index("runtime_ops.set_uwb_busy(false)", fail_closed)
        resume = discovery_run.index(
            "survey_transport_preempt_end()", clear_busy
        )
        self.assertLess(run, release)
        self.assertLess(release, release_failure)
        self.assertLess(release_failure, fail_closed)
        self.assertLess(fail_closed, clear_busy)
        self.assertLess(clear_busy, resume)
        self.assertNotIn(
            "survey_transport_preempt_end()",
            discovery_run[release:fail_closed],
            "an unparked radio must retain both its lease and transport pause",
        )

    def test_no_radio_report_is_driven_only_after_transport_resume(self):
        finish = function_body(RUNTIME, "finish_discovery_without_radio")

        stage = finish.index(
            "app_anchor_survey_discovery_stage_empty_report("
        )
        end_preemption = finish.index(
            "survey_transport_preempt_end()", stage
        )
        service_owner = finish.index(
            "app_anchor_survey_discovery_retry_report()", end_preemption
        )
        schedule_report = finish.index(
            "runtime_ops.report_schedule(0u)", service_owner
        )
        self.assertLess(stage, end_preemption)
        self.assertLess(end_preemption, service_owner)
        self.assertLess(service_owner, schedule_report)
        self.assertNotIn(
            "runtime_ops.report_schedule(0u)",
            finish[stage:end_preemption],
            "the paused transport must not consume the staged owner's only "
            "report-work edge",
        )

    def test_pair_preempts_mesh_before_radio_and_resumes_after_parking(self):
        worker = function_body(RUNTIME, "survey_work_handler")
        pair_ready = function_body(RUNTIME, "pair_start_delivery_ready")
        lead = function_body(RUNTIME, "pair_start_transport_preempt_lead_ms")
        delay = function_body(RUNTIME, "pair_start_release_delay_ms")

        self.assertIn("SURVEY_DISCOVERY_TRANSPORT_PREEMPT_BUDGET_MS", lead)
        self.assertIn("SURVEY_PAIR_START_SKEW_MARGIN_MS", lead)
        self.assertIn("!as_responder", lead)
        self.assertIn("pair_start_release_delay_ms(", pair_ready)
        self.assertIn("survey_transport_preempt_begin()", delay)
        self.assertIn("deadline_schedule(", pair_ready)

        pair_start = worker.index("app_node_comm_stop_role_scan()",
                                  worker.index("if (!pair_due)"))
        begin = worker.index("survey_transport_preempt_begin()", pair_start)
        claim = worker.index("radio_guard_uwb_claim(", begin)
        run = worker.index("run_pair_responder(", claim)
        release = worker.index("survey_radio_release(&radio_lease", run)
        resume = worker.index("survey_transport_preempt_end()", release)
        report = worker.index("runtime_ops.report_schedule(0u)", resume)
        self.assertLess(pair_start, begin)
        self.assertLess(begin, claim)
        self.assertLess(claim, run)
        self.assertLess(run, release)
        self.assertLess(release, resume)
        self.assertLess(resume, report)

    def test_pair_rf_does_not_yield_the_shared_start_window_to_queued_relay_tx(
        self,
    ) -> None:
        worker = function_body(RUNTIME, "survey_work_handler")
        pair_start = worker.index(
            "if (app_anchor_survey_runtime_discovery_is_pending())"
        )
        pair_claim = worker.index(
            "survey_transport_preempt_begin()", pair_start
        )
        pair_gate = worker[pair_start:pair_claim]

        self.assertIn("if (anchor_uwb_window_active())", pair_gate)
        self.assertNotIn(
            "runtime_ops.relay_tx_active()",
            pair_gate.split("if (anchor_uwb_window_active())", 1)[0],
        )
        click_gate = pair_gate[
            pair_gate.index("if (anchor_uwb_window_active())") :
        ]
        self.assertNotIn(
            "||",
            click_gate.split("{", 1)[0],
            "queued START-result ACK_CONFIRM must not own the pair RF window",
        )
        self.assertIn("survey_transport_preempt_begin()", worker[pair_claim:])

    def test_prep_budget_includes_bounded_transport_quiescence(self):
        self.assertRegex(
            CONFIG,
            r"#define\s+SURVEY_DISCOVERY_TRANSPORT_PREEMPT_BUDGET_MS\s+1000u\b",
        )
        self.assertRegex(
            CONFIG,
            r"#define\s+SURVEY_DISCOVERY_PHY_PREP_MEASURED_MAX_MS\s+63u\b",
        )
        self.assertRegex(
            CONFIG,
            r"#define\s+SURVEY_DISCOVERY_PHY_PREP_MARGIN_MS\s+40u\b",
        )
        self.assertRegex(
            CONFIG,
            r"#define\s+SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS\s+\\\s*"
            r"\(SURVEY_DISCOVERY_TRANSPORT_PREEMPT_BUDGET_MS\s*\+\s*\\\s*"
            r"SURVEY_DISCOVERY_PHY_PREP_MEASURED_MAX_MS\s*\+\s*\\\s*"
            r"SURVEY_DISCOVERY_PHY_PREP_MARGIN_MS\)",
        )
        self.assertEqual(1000 + 63 + 40, 1103)
        self.assertIn(
            "SURVEY_DISCOVERY_START_DELAY_MS >=\n"
            "             SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS",
            CONFIG,
        )


if __name__ == "__main__":
    unittest.main()
