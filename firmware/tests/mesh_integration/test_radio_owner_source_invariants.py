#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"
SOURCE_SUFFIXES = {".c", ".h", ".inc"}
RECEIVE_CALL = re.compile(
    r"\bdwm3000_driver_receive_frame"
    r"(?:_detailed_quiet|_detailed|_continuous_extend_on_activity|"
    r"_continuous_timed|_continuous)?"
    r"\s*\("
)


def app_source_paths():
    return sorted(
        path
        for path in APP_SRC.rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth != 0:
                continue
            next_index = index + 1
            while next_index < len(source) and source[next_index].isspace():
                next_index += 1
            if next_index >= len(source) or source[next_index] != "{":
                break
            brace_depth = 0
            for end in range(next_index, len(source)):
                brace_depth += source[end] == "{"
                brace_depth -= source[end] == "}"
                if brace_depth == 0:
                    return source[candidate.start() : end + 1]
            raise AssertionError(f"unterminated function {name}")
    raise AssertionError(f"missing function {name}")


def source_hits(needle: str):
    hits = []
    for path in app_source_paths():
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if needle in line:
                hits.append(f"{path.relative_to(ROOT)}:{line_number}")
    return hits


def mask_non_code(source: str) -> str:
    masked = list(source)
    state = "code"
    index = 0

    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                masked[index] = masked[index + 1] = " "
                state = "line_comment"
                index += 2
                continue
            if char == "/" and next_char == "*":
                masked[index] = masked[index + 1] = " "
                state = "block_comment"
                index += 2
                continue
            if char == '"':
                masked[index] = " "
                state = "string"
            elif char == "'":
                masked[index] = " "
                state = "character"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
            else:
                masked[index] = " "
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                masked[index] = masked[index + 1] = " "
                state = "code"
                index += 2
                continue
            if char != "\n":
                masked[index] = " "
        elif state in {"string", "character"}:
            if char == "\\" and index + 1 < len(source):
                masked[index] = " "
                if source[index + 1] != "\n":
                    masked[index + 1] = " "
                index += 2
                continue
            terminator = '"' if state == "string" else "'"
            if char == terminator:
                masked[index] = " "
                state = "code"
            elif char != "\n":
                masked[index] = " "
        index += 1
    return "".join(masked)


def matching_delimiter(source: str, start: int, opening: str, closing: str) -> int:
    depth = 0

    for index in range(start, len(source)):
        if source[index] == opening:
            depth += 1
        elif source[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError(f"unterminated {opening} at offset {start}")


def next_code_index(source: str, start: int) -> int:
    index = start

    while index < len(source) and source[index].isspace():
        index += 1
    return index


def loop_blocks(masked: str):
    blocks = []

    for match in re.finditer(r"\b(?:while|for)\s*\(", masked):
        paren = masked.index("(", match.start())
        paren_end = matching_delimiter(masked, paren, "(", ")")
        brace = next_code_index(masked, paren_end + 1)
        if brace < len(masked) and masked[brace] == "{":
            blocks.append(
                (brace, matching_delimiter(masked, brace, "{", "}"))
            )
    for match in re.finditer(r"\bdo\s*\{", masked):
        brace = masked.index("{", match.start())
        blocks.append((brace, matching_delimiter(masked, brace, "{", "}")))
    return blocks


def cancellation_condition_matches(condition: str) -> bool:
    if re.search(r"\bret\s*!=\s*-ECANCELED\b", condition):
        return False
    return any(
        re.search(pattern, condition)
        for pattern in (
            r"\bret\s*==\s*-ECANCELED\b",
            r"\bret\s*<\s*0\b",
            r"\bret\s*!=\s*0\b",
            r"\bret\s*!=\s*PROTO_OK\b",
        )
    )


def cancellation_exits_iteration(masked: str, start: int, end: int) -> bool:
    for match in re.finditer(r"\bif\s*\(", masked[start:end]):
        if_start = start + match.start()
        paren = masked.index("(", if_start)
        paren_end = matching_delimiter(masked, paren, "(", ")")
        if paren_end >= end:
            break
        condition = masked[paren + 1 : paren_end]
        if not cancellation_condition_matches(condition):
            continue
        statement = next_code_index(masked, paren_end + 1)
        if statement >= end:
            return False
        if masked[statement] == "{":
            statement_end = matching_delimiter(masked, statement, "{", "}")
        else:
            statement_end = masked.find(";", statement, end)
            if statement_end < 0:
                return False
        action = masked[statement : statement_end + 1]
        if re.search(r"\b(?:break|return|goto)\b", action):
            return True
        if re.search(r"\bcontinue\b", action):
            return False
    return False


def braced_block_at(source: str, marker: int) -> str:
    brace = source.index("{", marker)
    end = matching_delimiter(mask_non_code(source), brace, "{", "}")
    return source[brace : end + 1]


class RadioOwnerSourceInvariantTests(unittest.TestCase):
    def test_legacy_radio_guard_is_absent_from_application_sources(self):
        hits = source_hits("radio_guard")
        self.assertEqual(
            [],
            hits,
            "retired radio-guard spelling remains at: " + ", ".join(hits),
        )

    def test_raw_receive_abort_calls_stay_at_driver_binding_boundary(self):
        allowed_names = {
            "app_state.c",
            "dwm3000_driver.h",
            "dwm3000_driver_io.inc",
        }
        violations = []

        for symbol in (
            "dwm3000_driver_request_receive_abort",
            "dwm3000_driver_clear_receive_abort",
        ):
            for hit in source_hits(symbol):
                relative_path = hit.rsplit(":", 1)[0]
                if Path(relative_path).name not in allowed_names:
                    violations.append(hit)

        self.assertEqual(
            [],
            violations,
            "business code bypasses generation-owned abort leases at: "
            + ", ".join(violations),
        )

    def test_driver_abort_level_is_cleared_only_by_explicit_release(self):
        radio = (
            APP_SRC / "dwm3000_driver_radio.inc"
        ).read_text(encoding="utf-8")
        io = (
            APP_SRC / "dwm3000_driver_io.inc"
        ).read_text(encoding="utf-8")
        wait = function_body(radio, "wait_status_internal")
        request = function_body(io, "dwm3000_driver_request_receive_abort")
        clear = function_body(io, "dwm3000_driver_clear_receive_abort")

        self.assertIn(
            "atomic_get(&receive_abort_requested) != 0",
            wait,
        )
        self.assertNotIn(
            "atomic_cas(&receive_abort_requested",
            wait,
            "an RX observation must not consume another owner's abort level",
        )
        abort_check = wait.index(
            "atomic_get(&receive_abort_requested) != 0"
        )
        cancel = wait.index("return -ECANCELED", abort_check)
        self.assertLess(abort_check, cancel)
        self.assertIn("atomic_set(&receive_abort_requested, 1)", request)
        self.assertIn("atomic_set(&receive_abort_requested, 0)", clear)

    def test_every_repeated_receive_exits_on_cancellation(self):
        violations = []
        loop_call_count = 0

        for path in app_source_paths():
            if path.name.startswith("dwm3000_driver"):
                continue
            source = path.read_text(encoding="utf-8")
            masked = mask_non_code(source)
            loops = loop_blocks(masked)
            calls = list(RECEIVE_CALL.finditer(masked))
            for call_index, call in enumerate(calls):
                containing = [
                    block
                    for block in loops
                    if block[0] < call.start() < block[1]
                ]
                if not containing:
                    continue
                loop_call_count += 1
                loop_start, loop_end = min(
                    containing, key=lambda block: block[1] - block[0]
                )
                del loop_start
                call_paren = masked.index("(", call.start())
                call_end = matching_delimiter(masked, call_paren, "(", ")")
                statement_end = masked.find(";", call_end, loop_end)
                self.assertGreaterEqual(
                    statement_end,
                    0,
                    f"missing receive statement terminator in {path}",
                )
                next_call = (
                    calls[call_index + 1].start()
                    if call_index + 1 < len(calls)
                    else loop_end
                )
                check_end = min(loop_end, next_call)
                if not cancellation_exits_iteration(
                    masked, statement_end + 1, check_end
                ):
                    line = source.count("\n", 0, call.start()) + 1
                    violations.append(
                        f"{path.relative_to(ROOT)}:{line}"
                    )

        self.assertGreater(
            loop_call_count,
            10,
            "receive-loop discovery unexpectedly stopped covering production",
        )
        self.assertEqual(
            [],
            violations,
            "-ECANCELED can reach another receive iteration at: "
            + ", ".join(violations),
        )

    def test_clicker_politeness_helper_unwinds_its_whole_owner_lease_on_cancel(
        self,
    ):
        clicker = (APP_SRC / "app_clicker.c").read_text(encoding="utf-8")
        sample = function_body(clicker, "clicker_sample_uwb_gate")
        phase = function_body(clicker, "clicker_politeness_phase")

        receive = sample.index("dwm3000_driver_receive_frame_detailed(")
        preserve = sample.index("if (ret == -ECANCELED)", receive)
        preserve_return = sample.index("return ret;", preserve)
        normalize_error = sample.index(
            "else if (ret != -ETIMEDOUT)", preserve_return
        )
        self.assertLess(receive, preserve)
        self.assertLess(preserve, preserve_return)
        self.assertLess(preserve_return, normalize_error)

        call = phase.index("clicker_sample_uwb_gate(")
        cancel = phase.index("if (ret == -ECANCELED)", call)
        cancel_block = braced_block_at(phase, cancel)
        self.assertIn("break;", cancel_block)
        release = phase.index(
            "app_mesh_radio_owner_release(&radio_lease)", cancel
        )
        courtesy_stop = phase.index(
            "app_clicker_ble_courtesy_stop();", release
        )
        propagated = phase.index("if (ret == -ECANCELED)", courtesy_stop)
        propagated_return = phase.index("return ret;", propagated)
        self.assertLess(call, cancel)
        self.assertLess(cancel, release)
        self.assertLess(release, courtesy_stop)
        self.assertLess(courtesy_stop, propagated)
        self.assertLess(propagated, propagated_return)

    def test_scan_workers_never_rearm_after_cancellation(self):
        anchor_radio = (
            APP_SRC / "app_anchor_radio.inc"
        ).read_text(encoding="utf-8")
        report_rx = (
            APP_SRC / "app_mesh_report_rx.inc"
        ).read_text(encoding="utf-8")
        report_transport = (
            APP_SRC / "app_mesh_report_transport.inc"
        ).read_text(encoding="utf-8")
        anchor_worker = function_body(
            anchor_radio, "anchor_uwb_scan_work_handler"
        )
        mesh_worker = function_body(
            report_rx, "mesh_uwb_rx_work_handler"
        )

        focused = anchor_worker.index("if (focused_logs &&")
        focused_end = anchor_worker.index(") {", focused)
        self.assertIn(
            "ret != -ECANCELED",
            anchor_worker[focused:focused_end],
            "focused scan retries must not consume a live abort level",
        )

        general_start = mesh_worker.rindex("ret = mesh_rx_radio_start(")
        start_failure = mesh_worker.index("if (ret < 0)", general_start)
        start_failure_block = braced_block_at(mesh_worker, start_failure)
        cancel = start_failure_block.index("if (ret == -ECANCELED)")
        inactive = start_failure_block.index(
            "mesh_uwb_rx_active = false;", cancel
        )
        boundary = start_failure_block.index(
            "app_node_comm_gateway_delivery_safe_boundary()", cancel
        )
        stop = start_failure_block.index("return;", boundary)
        rearm = start_failure_block.index("mesh_schedule_uwb_rx(", stop)
        self.assertLess(cancel, inactive)
        self.assertLess(inactive, boundary)
        self.assertLess(boundary, stop)
        self.assertLess(stop, rearm)

        radio_stop = mesh_worker.rindex(
            "mesh_rx_radio_stop(&radio_lease);"
        )
        post_cancel = mesh_worker.index(
            "if (ret == -ECANCELED)", radio_stop
        )
        post_inactive = mesh_worker.index(
            "mesh_uwb_rx_active = false;", post_cancel
        )
        post_boundary = mesh_worker.index(
            "app_node_comm_gateway_delivery_safe_boundary()", post_cancel
        )
        post_return = mesh_worker.index("return;", post_boundary)
        post_rearm = mesh_worker.index("mesh_schedule_uwb_rx(", post_return)
        self.assertLess(post_cancel, post_inactive)
        self.assertLess(post_inactive, post_boundary)
        self.assertLess(post_boundary, post_return)
        self.assertLess(post_return, post_rearm)

        for function in (
            "mesh_uwb_rx_rearm_work_handler",
            "mesh_schedule_uwb_rx",
            "mesh_restart_role_scan",
        ):
            with self.subTest(function=function):
                body = function_body(report_transport, function)
                self.assertIn(
                    "app_mesh_radio_owner_rx_scan_rearm_allowed()",
                    body,
                )

    def test_granted_gateway_workers_unconditionally_restore_scan_liveness(self):
        control = (
            APP_SRC / "app_anchor_gateway_control.inc"
        ).read_text(encoding="utf-8")
        worker_expectations = (
            (
                "gateway_discovery_assignment_publish_work_handler",
                "GATEWAY_ASSIGNMENT_PUBLISH_RETURN",
                (
                    "!gateway_discovery_assignment_state.active",
                    "!current_generation",
                    "gateway_discovery_assignment_state.claim_count == 0u",
                    "if (ret < 0",
                ),
                True,
            ),
            (
                "gateway_host_command_work_handler",
                "GATEWAY_HOST_COMMAND_RETURN",
                (
                    "k_msgq_peek(&gateway_host_command_msgq",
                    "APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED",
                    "app_mesh_command_orchestrator_activate(",
                    "if (ret < 0",
                ),
                False,
            ),
        )

        for function, unwind, terminal_markers, unlocks_mutex in (
            worker_expectations
        ):
            with self.subTest(function=function):
                body = function_body(control, function)
                begin = body.index(
                    "mesh_gateway_radio_handoff_begin("
                )
                rejected = body.index("if (ret < 0)", begin)
                rejected_block = braced_block_at(body, rejected)
                self.assertIn("return;", rejected_block)
                self.assertIn(
                    "mesh_restart_role_scan();",
                    rejected_block,
                    "a grant rejected before worker execution must restore "
                    "scanning",
                )

                macro = body.index(f"#define {unwind}()")
                macro_end = body.index("while (false)", macro)
                macro_definition = body[macro:macro_end]
                self.assertIn(
                    "mesh_restart_role_scan();", macro_definition
                )
                if unlocks_mutex:
                    self.assertIn(
                        "k_mutex_unlock(&gateway_discovery_assignment_mutex)",
                        macro_definition,
                    )

                granted_logic = body[macro_end:]
                self.assertNotIn(
                    "return;",
                    granted_logic,
                    "a granted worker exit bypasses its scan-restart unwind",
                )
                self.assertGreater(
                    granted_logic.count(f"{unwind}();"),
                    2,
                )
                for marker in terminal_markers:
                    self.assertIn(marker, granted_logic)

                undef = granted_logic.rindex(f"#undef {unwind}")
                if function == "gateway_host_command_work_handler":
                    self.assertIn(
                        "mesh_restart_role_scan();",
                        granted_logic[:undef],
                        "the fallthrough success path must restore scanning",
                    )
                else:
                    final_unwind = granted_logic.rindex(
                        f"{unwind}();", 0, undef
                    )
                    self.assertLess(final_unwind, undef)

        for callback in (
            "gateway_discovery_assignment_schedule_failed",
            "gateway_host_command_schedule_failed",
        ):
            with self.subTest(schedule_failure=callback):
                failure = function_body(control, callback)
                self.assertIn(
                    "mesh_restart_role_scan();",
                    failure,
                    "a failed granted-work schedule must restore the "
                    "scan that was stopped for handoff",
                )

    def test_gateway_failure_cleanup_is_token_bounded_and_rearmed(self):
        control = (
            APP_SRC / "app_anchor_gateway_control.inc"
        ).read_text(encoding="utf-8")
        submit = function_body(
            control, "gateway_host_command_submit_priority"
        )
        failure = function_body(
            control, "gateway_host_command_schedule_failed"
        )
        retry = function_body(
            control, "gateway_host_command_retry_work_handler"
        )
        delayed = function_body(
            control,
            "gateway_host_command_resubmit_after_failure_callback",
        )

        self.assertIn(
            "gateway_host_command_next_admission_id", submit
        )
        eagain = submit.index(
            "app_anchor_host_command_submit_needs_retry(ret)"
        )
        arm = submit.index(
            "gateway_host_command_resubmit_after_failure_callback()",
            eagain,
        )
        retained = submit.index(
            "app_anchor_host_command_ingress_result(ret)", arm
        )
        self.assertLess(eagain, arm)
        self.assertLess(arm, retained)
        self.assertNotIn("return ret;", submit)

        impossible = submit.index(
            "app_anchor_host_command_submit_is_contract_failure(ret)",
            eagain,
        )
        fail_closed = submit.index(
            "app_watchdog_stop_feeding();", impossible
        )
        self.assertLess(impossible, fail_closed)
        self.assertLess(fail_closed, arm)

        peek = failure.index("k_msgq_peek(&gateway_host_command_msgq")
        cutoff = failure.index(
            "app_anchor_host_command_within_failure_cutoff(", peek
        )
        dequeue = failure.index(
            "k_msgq_get(&gateway_host_command_msgq", cutoff
        )
        final_peek = failure.rindex(
            "k_msgq_peek(&gateway_host_command_msgq"
        )
        resubmit = failure.index(
            "gateway_host_command_resubmit_after_failure_callback()",
            final_peek,
        )
        self.assertLess(peek, cutoff)
        self.assertLess(cutoff, dequeue)
        self.assertLess(dequeue, final_peek)
        self.assertLess(final_peek, resubmit)

        self.assertIn("K_MSEC(1u)", delayed)
        schedule_error = delayed.index("if (ret < 0)")
        fail_closed = delayed.index(
            "app_watchdog_stop_feeding();", schedule_error
        )
        logged = delayed.index("LOG_ERR(", fail_closed)
        self.assertLess(schedule_error, fail_closed)
        self.assertLess(fail_closed, logged)
        retry_eagain = retry.index(
            "app_anchor_host_command_submit_needs_retry(ret)"
        )
        retry_return = retry.index("return;", retry_eagain)
        cleanup = retry.index(
            "gateway_host_command_schedule_failed(", retry_return
        )
        self.assertLess(retry_eagain, retry_return)
        self.assertLess(retry_return, cleanup)

    def test_assignment_cancel_and_rejected_grant_restore_scanning(self):
        control = (
            APP_SRC / "app_anchor_gateway_control.inc"
        ).read_text(encoding="utf-8")
        fail = function_body(
            control, "gateway_discovery_assignment_fail_locked"
        )
        delivery = function_body(
            control,
            "gateway_discovery_assignment_service_delivery_locked",
        )
        finalize = function_body(
            control,
            "gateway_discovery_assignment_finalize_work_handler",
        )

        self.assertIn(
            "mesh_gateway_radio_handoff_cancel(", fail
        )
        for marker in (
            "COMMAND_INTERNAL_ERROR",
            "NODE_COMM_TERMINAL_DEADLINE_EXPIRED",
            "NODE_COMM_TERMINAL_PERMANENT_FAILURE",
        ):
            failure_path = delivery.index(marker)
            restart_flag = delivery.index(
                "*restart_scan = true;",
                failure_path,
            )
            returned = delivery.index("return true;", restart_flag)
            self.assertLess(restart_flag, returned)

        service = finalize.index(
            "gateway_discovery_assignment_service_delivery_locked("
        )
        service_unlock = finalize.index(
            "k_mutex_unlock(&gateway_discovery_assignment_mutex)",
            service,
        )
        service_restart = finalize.index(
            "mesh_restart_role_scan();", service_unlock
        )
        self.assertLess(service, service_unlock)
        self.assertLess(service_unlock, service_restart)

        expired = finalize.index(
            "app_discovery_assignment_operation_expired("
        )
        fail_call = finalize.index(
            "gateway_discovery_assignment_fail_locked(", expired
        )
        unlock = finalize.index(
            "k_mutex_unlock(&gateway_discovery_assignment_mutex)",
            fail_call,
        )
        restart = finalize.index("mesh_restart_role_scan();", unlock)
        self.assertLess(fail_call, unlock)
        self.assertLess(unlock, restart)

    def test_ds_twr_loops_unwind_whole_operation_on_cancellation(self):
        anchor_radio = (
            APP_SRC / "app_anchor_radio.inc"
        ).read_text(encoding="utf-8")
        clicker = (APP_SRC / "app_clicker.c").read_text(encoding="utf-8")
        clicker_power = (
            APP_SRC / "app_clicker_radio_power.c"
        ).read_text(encoding="utf-8")
        ml = (APP_SRC / "app_ml.c").read_text(encoding="utf-8")
        survey_runtime = (
            APP_SRC / "app_anchor_survey_runtime.c"
        ).read_text(encoding="utf-8")

        scheduled = function_body(
            anchor_radio, "anchor_run_scheduled_uwb_ranges"
        )
        responder = scheduled.index(
            "dwm3000_driver_responder_poll_expected("
        )
        cancel = scheduled.index("if (ret == -ECANCELED)", responder)
        unwind = scheduled.index(
            "goto scheduled_range_cancelled;", cancel
        )
        next_sample_logic = scheduled.index(
            "if (range_result.exchange_started)", unwind
        )
        cleanup = scheduled.index("scheduled_range_cancelled:")
        quiet_release = scheduled.index(
            'gateway_ble_exit_uwb_quiet("ml-anchor-scheduled-uwb")',
            cleanup,
        )
        self.assertLess(responder, cancel)
        self.assertLess(cancel, unwind)
        self.assertLess(unwind, next_sample_logic)
        self.assertLess(cleanup, quiet_release)

        pair = function_body(
            anchor_radio, "anchor_run_clicker_pair_survey"
        )
        initiator_call = pair.index(
            "dwm3000_driver_range_initiator("
        )
        initiator_idle = pair.index(
            '"pair-survey-initiator"', initiator_call
        )
        initiator_cancel = pair.index(
            "if (ret == -ECANCELED)", initiator_idle
        )
        initiator_return = pair.index(
            "return retained_sleep_us;", initiator_cancel
        )
        result_send = pair.index(
            "anchor_send_pair_survey_result(", initiator_return
        )
        self.assertLess(initiator_call, initiator_idle)
        self.assertLess(initiator_idle, initiator_cancel)
        self.assertLess(initiator_cancel, initiator_return)
        self.assertLess(initiator_return, result_send)

        responder_call = pair.index(
            "dwm3000_driver_responder_poll_expected("
        )
        responder_idle = pair.index(
            '"pair-survey-responder"', responder_call
        )
        responder_cancel = pair.index(
            "if (ret == -ECANCELED)", responder_idle
        )
        responder_return = pair.index(
            "return retained_sleep_us;", responder_cancel
        )
        self.assertLess(responder_call, responder_idle)
        self.assertLess(responder_idle, responder_cancel)
        self.assertLess(responder_cancel, responder_return)

        for function, driver_call in (
            ("run_pair_initiator", "dwm3000_driver_range_initiator("),
            (
                "run_pair_responder",
                "dwm3000_driver_responder_poll_expected(",
            ),
        ):
            with self.subTest(function=function):
                body = function_body(survey_runtime, function)
                call = body.index(driver_call)
                cancel = body.index("if (ret == -ECANCELED)", call)
                returned = body.index("return ret;", cancel)
                self.assertLess(call, cancel)
                self.assertLess(cancel, returned)

        clicker_ranges = function_body(
            clicker, "app_clicker_range_scheduled_anchors"
        )
        clicker_finish = function_body(
            clicker_power, "app_clicker_radio_finish_scheduled_burst"
        )
        finish_standby = clicker_finish.index(
            "dwm3000_driver_standby()"
        )
        finish_release = clicker_finish.index(
            "app_mesh_radio_owner_release(radio_lease)", finish_standby
        )
        self.assertNotIn("app_mesh_radio_owner_try_claim(", clicker_finish)
        self.assertLess(finish_standby, finish_release)

        clicker_claim = clicker_ranges.index(
            "app_mesh_radio_owner_try_claim("
        )
        clicker_loop = clicker_ranges.index(
            "while (session->state == UWB_CLICKER_RANGING)"
        )
        clicker_call = clicker_ranges.index(
            "dwm3000_driver_range_initiator("
        )
        clicker_idle = clicker_ranges.index(
            "clicker_release_scheduled_range_radio()", clicker_call
        )
        clicker_cancel = clicker_ranges.index(
            "if (ret == -ECANCELED)", clicker_idle
        )
        clicker_abort = clicker_ranges.index(
            "uwb_clicker_abort_attempt(session)", clicker_cancel
        )
        clicker_break = clicker_ranges.index("break;", clicker_abort)
        clicker_result = clicker_ranges.index(
            "if (!range_result.exchange_started)", clicker_break
        )
        clicker_finish_call = clicker_ranges.index(
            "app_clicker_radio_finish_scheduled_burst(&radio_lease)"
        )
        post_burst_diagnostics = clicker_ranges.index(
            "clicker_callbacks.ml_run_post_burst_diagnostics("
        )
        self.assertEqual(
            1,
            clicker_ranges.count("app_mesh_radio_owner_try_claim("),
            "a scheduled DS-TWR burst must hold one exact lease",
        )
        self.assertGreaterEqual(
            clicker_ranges.count("app_mesh_radio_owner_abort_pending()"),
            2,
            "scheduled waits must observe pause/abort before another sample",
        )
        self.assertNotIn(
            "app_mesh_radio_owner_release(&radio_lease)",
            clicker_ranges,
            "scheduled range code must park through its power boundary",
        )
        self.assertLess(clicker_claim, clicker_loop)
        self.assertLess(clicker_call, clicker_idle)
        self.assertLess(clicker_idle, clicker_cancel)
        self.assertLess(clicker_cancel, clicker_abort)
        self.assertLess(clicker_abort, clicker_break)
        self.assertLess(clicker_break, clicker_result)
        self.assertLess(clicker_result, clicker_finish_call)
        self.assertLess(clicker_finish_call, post_burst_diagnostics)

        normal_click = function_body(clicker, "app_clicker_run_normal_click")
        propagated_power_error = normal_click.index(
            "if (ret < 0 && session.state == UWB_CLICKER_SUCCEEDED)"
        )
        propagated_return = normal_click.index(
            "return ret;", propagated_power_error
        )
        retry = normal_click.index(
            "uwb_clicker_prepare_retry(&session)", propagated_return
        )
        self.assertLess(propagated_power_error, propagated_return)
        self.assertLess(propagated_return, retry)
        self.assertIn(
            "if (ret >= 0 && session.state == UWB_CLICKER_SUCCEEDED)",
            normal_click,
            "successful ranging state must not hide a park/release error",
        )

        ml_diagnostics = function_body(
            ml, "ml_clicker_run_post_burst_diagnostics"
        )
        claim = ml_diagnostics.index("app_mesh_radio_owner_try_claim(")
        claim_cancel = ml_diagnostics.index(
            "ret == -ECANCELED || ret == -ESHUTDOWN", claim
        )
        claim_exit = ml_diagnostics.index("goto out;", claim_cancel)
        claim_continue = ml_diagnostics.index("continue;", claim_exit)
        range_call = ml_diagnostics.index(
            "dwm3000_driver_range_initiator(", claim_continue
        )
        range_release = ml_diagnostics.index(
            "app_mesh_radio_owner_release(&radio_lease)", range_call
        )
        range_cancel = ml_diagnostics.index(
            "if (ret == -ECANCELED)", range_release
        )
        range_exit = ml_diagnostics.index("goto out;", range_cancel)
        result_processing = ml_diagnostics.index(
            "if (range_result.exchange_started)", range_exit
        )
        cleanup = ml_diagnostics.index("out:", result_processing)
        quiet_release = ml_diagnostics.index(
            "gateway_ble_exit_uwb_quiet(", cleanup
        )
        self.assertLess(claim, claim_cancel)
        self.assertLess(claim_cancel, claim_exit)
        self.assertLess(claim_exit, claim_continue)
        self.assertLess(range_call, range_release)
        self.assertLess(range_release, range_cancel)
        self.assertLess(range_cancel, range_exit)
        self.assertLess(range_exit, result_processing)
        self.assertLess(result_processing, cleanup)
        self.assertLess(cleanup, quiet_release)

    def test_transport_pause_and_resume_fail_closed_on_owner_errors(self):
        gate = (
            APP_SRC / "app_mesh_transport_gate.c"
        ).read_text(encoding="utf-8")
        expectations = (
            (
                "app_mesh_transport_gate_pause",
                (
                    "app_mesh_radio_owner_pause(",
                ),
            ),
            (
                "app_mesh_transport_gate_request_abort",
                (
                    "app_mesh_radio_owner_abort_request(",
                ),
            ),
            (
                "app_mesh_transport_gate_resume",
                (
                    "app_mesh_radio_owner_abort_release(",
                    "app_mesh_radio_owner_resume(",
                ),
            ),
        )

        for function, owner_operations in expectations:
            body = function_body(gate, function)
            for operation in owner_operations:
                with self.subTest(function=function, operation=operation):
                    operation_index = body.index(operation)
                    failure = body.index("fail_closed(", operation_index)
                    self.assertGreater(failure, operation_index)

        fail_closed = function_body(gate, "fail_closed")
        self.assertIn("LOG_ERR(", fail_closed)
        self.assertIn("app_watchdog_stop_feeding();", fail_closed)

    def test_high_debug_raw_probe_and_power_commands_hold_one_exact_lease(self):
        high_debug = (
            APP_SRC / "app_high_debug.c"
        ).read_text(encoding="utf-8")
        main = (APP_SRC / "main.c").read_text(encoding="utf-8")

        owned_probe = function_body(
            high_debug, "high_debug_probe_dwm3000_owned"
        )
        for operation in (
            "dwm3000_port_init()",
            "dwm3000_port_wakeup()",
            "dwm3000_port_hw_reset()",
            "dwm3000_driver_probe(&dev_id)",
            "dwm3000_port_set_fast_spi()",
        ):
            self.assertIn(operation, owned_probe)
        self.assertNotIn("app_mesh_radio_owner_try_claim(", owned_probe)
        self.assertNotIn("app_mesh_radio_owner_release(", owned_probe)
        self.assertEqual(
            3,
            high_debug.count("high_debug_probe_dwm3000_owned("),
            "the raw owned probe may only be defined and called by its two "
            "exact HIGH_DEBUG lease wrappers",
        )

        public_probe = function_body(
            high_debug, "high_debug_probe_dwm3000"
        )
        public_claim = public_probe.index(
            "app_mesh_radio_owner_try_claim("
        )
        public_client = public_probe.index(
            "APP_MESH_RADIO_CLIENT_HIGH_DEBUG", public_claim
        )
        public_raw = public_probe.index(
            "high_debug_probe_dwm3000_owned()", public_client
        )
        public_standby = public_probe.index(
            "dwm3000_driver_standby()", public_raw
        )
        public_release = public_probe.index(
            "app_mesh_radio_owner_release(&radio_lease)", public_standby
        )
        self.assertLess(public_claim, public_client)
        self.assertLess(public_client, public_raw)
        self.assertLess(public_raw, public_standby)
        self.assertLess(public_standby, public_release)

        stage0 = function_body(
            high_debug, "high_debug_stage0_hardware_self_test"
        )
        stage_claim = stage0.index("app_mesh_radio_owner_try_claim(")
        stage_client = stage0.index(
            "APP_MESH_RADIO_CLIENT_HIGH_DEBUG", stage_claim
        )
        stage_probe = stage0.index(
            "high_debug_probe_dwm3000_owned()", stage_client
        )
        stage_sleep = stage0.index(
            "dwm3000_driver_standby()", stage_probe
        )
        stage_wake = stage0.index(
            "dwm3000_driver_configure_default()", stage_sleep
        )
        stage_release = stage0.rindex(
            "app_mesh_radio_owner_release(&radio_lease)"
        )
        self.assertLess(stage_claim, stage_client)
        self.assertLess(stage_client, stage_probe)
        self.assertLess(stage_probe, stage_sleep)
        self.assertLess(stage_sleep, stage_wake)
        self.assertLess(stage_wake, stage_release)
        self.assertNotIn("high_debug_probe_dwm3000()", stage0)

        power = function_body(
            high_debug, "high_debug_radio_power_command"
        )
        power_claim = power.index("app_mesh_radio_owner_try_claim(")
        power_client = power.index(
            "APP_MESH_RADIO_CLIENT_HIGH_DEBUG", power_claim
        )
        power_operation = power.index(
            "dwm3000_driver_configure_default()", power_client
        )
        self.assertIn("dwm3000_driver_standby()", power[power_operation:])
        power_release = power.index(
            "app_mesh_radio_owner_release(&radio_lease)", power_operation
        )
        self.assertLess(power_claim, power_client)
        self.assertLess(power_client, power_operation)
        self.assertLess(power_operation, power_release)

        command = function_body(high_debug, "high_debug_handle_command")
        self.assertIn(
            'strcmp(command, "uwb_probe")', command
        )
        self.assertIn("ret = high_debug_probe_dwm3000();", command)
        self.assertIn(
            "ret = high_debug_radio_power_command(false);", command
        )
        self.assertIn(
            "ret = high_debug_radio_power_command(true);", command
        )
        self.assertNotRegex(command, r"\bdwm3000_(?:driver|port)_")

        boot = function_body(main, "main")
        boot_probe = boot.index("ret = high_debug_probe_dwm3000();")
        self.assertNotIn(
            "dwm3000_driver_standby()", boot[boot_probe:],
            "the public probe owns and parks the DWM3000 before returning",
        )

    def test_clicker_preflight_and_low_power_paths_use_exact_leases(self):
        clicker = (APP_SRC / "app_clicker.c").read_text(encoding="utf-8")
        power = (
            APP_SRC / "app_clicker_radio_power.c"
        ).read_text(encoding="utf-8")

        preflight = function_body(
            power, "app_clicker_radio_self_test_preflight"
        )
        preflight_claim = preflight.index(
            "app_mesh_radio_owner_try_claim("
        )
        preflight_client = preflight.index(
            "APP_MESH_RADIO_CLIENT_CLICKER", preflight_claim
        )
        preflight_init = preflight.index(
            "dwm3000_port_init()", preflight_client
        )
        preflight_standby = preflight.index(
            "dwm3000_driver_standby()", preflight_init
        )
        preflight_release = preflight.index(
            "app_mesh_radio_owner_release(&radio_lease)",
            preflight_standby,
        )
        self.assertLess(preflight_claim, preflight_client)
        self.assertLess(preflight_client, preflight_init)
        self.assertLess(preflight_init, preflight_standby)
        self.assertLess(preflight_standby, preflight_release)
        for failure in (
            "self-test DWM3000 port init failed",
            "self-test DWM3000 wake failed",
            "self-test DWM3000 reset failed",
            "self-test DWM3000 DEV_ID probe failed",
            "self-test DWM3000 fast SPI config failed",
        ):
            marker = preflight.index(failure)
            self.assertIn("goto out;", preflight[marker:marker + 240])

        self_test = function_body(clicker, "app_clicker_run_self_test")
        preflight_call = self_test.index(
            "app_clicker_radio_self_test_preflight()"
        )
        diagnostic_call = self_test.index(
            "app_clicker_run_uwb_diagnostic_click(event_seq)"
        )
        self.assertLess(preflight_call, diagnostic_call)

        retained_owned = function_body(power, "retained_standby_owned")
        self.assertIn(
            "dwm3000_driver_force_recovery()", retained_owned
        )
        self.assertGreaterEqual(
            retained_owned.count("retained_standby_transition()"),
            2,
        )
        self.assertNotIn(
            "app_mesh_radio_owner_try_claim(", retained_owned
        )
        self.assertNotIn(
            "app_mesh_radio_owner_release(", retained_owned
        )
        self.assertEqual(
            2,
            power.count("retained_standby_owned("),
            "raw retained recovery may only be defined and called by its "
            "exact CLICKER lease wrapper",
        )

        retained = function_body(
            power, "app_clicker_radio_enter_retained_standby"
        )
        retained_claim = retained.index(
            "app_mesh_radio_owner_try_claim("
        )
        retained_client = retained.index(
            "APP_MESH_RADIO_CLIENT_CLICKER", retained_claim
        )
        retained_operation = retained.index(
            "retained_standby_owned()", retained_client
        )
        retained_release = retained.index(
            "app_mesh_radio_owner_release(&radio_lease)",
            retained_operation,
        )
        self.assertLess(retained_claim, retained_client)
        self.assertLess(retained_client, retained_operation)
        self.assertLess(retained_operation, retained_release)

        retained_idle = function_body(
            clicker, "clicker_enter_systemon_retained_idle"
        )
        retained_call = retained_idle.index(
            "app_clicker_radio_enter_retained_standby()"
        )
        deferred = retained_idle.index("if (ret == 0)", retained_call)
        wake_arm = retained_idle.index(
            "click_button_arm_idle_interrupt()", deferred
        )
        self.assertIn("return;", retained_idle[deferred:wake_arm])
        self.assertNotRegex(
            retained_idle, r"\bdwm3000_(?:driver|port)_"
        )
        self.assertNotIn("dwm3000_port_float_pins", clicker)
        self.assertNotIn("dwm3000_port_float_pins", power)

        systemoff = function_body(
            power, "app_clicker_radio_prepare_systemoff"
        )
        systemoff_claim = systemoff.index(
            "app_mesh_radio_owner_try_claim("
        )
        systemoff_client = systemoff.index(
            "APP_MESH_RADIO_CLIENT_CLICKER", systemoff_claim
        )
        systemoff_init = systemoff.index(
            "dwm3000_port_init()", systemoff_client
        )
        systemoff_park = systemoff.index(
            "dwm3000_port_prepare_systemoff()", systemoff_init
        )
        self.assertLess(systemoff_claim, systemoff_client)
        self.assertLess(systemoff_client, systemoff_init)
        self.assertLess(systemoff_init, systemoff_park)
        self.assertNotIn(
            "app_mesh_radio_owner_release(",
            systemoff,
            "terminal system-off must retain its exact lease through poweroff",
        )

        systemoff_idle = function_body(
            clicker, "app_clicker_enter_systemoff_idle"
        )
        prepare = systemoff_idle.index(
            "app_clicker_radio_prepare_systemoff(&radio_lease)"
        )
        lease_check = systemoff_idle.index(
            "radio_lease.generation == 0u", prepare
        )
        poweroff = systemoff_idle.rindex("clicker_systemoff_now()")
        self.assertLess(prepare, lease_check)
        self.assertLess(lease_check, poweroff)
        self.assertIn("return;", systemoff_idle[lease_check:poweroff])
        self.assertNotRegex(
            systemoff_idle, r"\bdwm3000_(?:driver|port)_"
        )
        self.assertNotIn(
            "app_mesh_radio_owner_release(", systemoff_idle
        )

    def test_self_test_report_reaches_radio_quiescence_before_idle(self):
        clicker = (APP_SRC / "app_clicker.c").read_text(encoding="utf-8")
        main = (APP_SRC / "main.c").read_text(encoding="utf-8")
        node_comm = (
            APP_SRC / "app_node_comm.c"
        ).read_text(encoding="utf-8")
        transport = (
            APP_SRC / "app_mesh_report_transport.inc"
        ).read_text(encoding="utf-8")

        emit = function_body(clicker, "app_clicker_emit_self_test_report")
        synchronous_send = emit.index(
            "clicker_callbacks.send_mesh_outbound("
        )
        send_failure = emit.index("if (ret < 0)", synchronous_send)
        success_return = emit.rindex("return 0;")
        self.assertLess(synchronous_send, send_failure)
        self.assertLess(send_failure, success_return)

        action = function_body(
            clicker, "app_clicker_handle_button_action"
        )
        self_test_start = action.index(
            "case BUTTON_ACTION_SELF_TEST_START:"
        )
        self_test_end = action.index(
            "case BUTTON_ACTION_SELF_TEST_CANCELLED:", self_test_start
        )
        self_test_action = action[self_test_start:self_test_end]
        emit_call = self_test_action.index(
            "app_clicker_emit_self_test_report("
        )
        terminal_idle = self_test_action.index(
            "app_clicker_enter_idle()", emit_call
        )
        self.assertLess(emit_call, terminal_idle)

        boot = function_body(main, "main")
        self.assertIn(
            ".send_mesh_outbound = app_node_comm_send", boot
        )
        adapter = function_body(node_comm, "app_node_comm_send")
        self.assertIn(
            "return mesh_send_outbound(envelope, reason);", adapter
        )
        synchronous_transport = function_body(
            transport, "mesh_send_outbound_with_release_on_channel"
        )
        owner = synchronous_transport.index(
            "mesh_transport_tx_start("
        )
        rf_send = synchronous_transport.index(
            "dwm3000_driver_send_frame(", owner
        )
        quiesced = synchronous_transport.index(
            "app_mesh_radio_owner_release(&radio_lease)", rf_send
        )
        self.assertLess(owner, rf_send)
        self.assertLess(rf_send, quiesced)
        self.assertIn(
            "return ret;", synchronous_transport[quiesced:]
        )

    def test_retired_arbitration_and_priority_sources_cannot_return(self):
        retired = (
            "app_mesh_arbitration_zephyr",
            "app_mesh_gateway_command_priority",
        )
        for stem in retired:
            with self.subTest(stem=stem):
                self.assertFalse((APP_SRC / f"{stem}.c").exists())
                self.assertFalse((APP_SRC / f"{stem}.h").exists())
                hits = source_hits(stem)
                self.assertEqual(
                    [],
                    hits,
                    f"retired owner is still referenced at: {', '.join(hits)}",
                )

    def test_gateway_scheduled_workers_consume_their_exact_grants(self):
        control = (
            APP_SRC / "app_anchor_gateway_control.inc"
        ).read_text(encoding="utf-8")
        report_rx = (
            APP_SRC / "app_mesh_report_rx.inc"
        ).read_text(encoding="utf-8")
        gateway_control = (
            APP_SRC / "app_node_comm_gateway_control.c"
        ).read_text(encoding="utf-8")
        owner = (
            APP_SRC / "app_mesh_radio_owner.c"
        ).read_text(encoding="utf-8")

        worker_expectations = (
            (
                "gateway_discovery_assignment_publish_work_handler",
                "&gateway_discovery_assignment_publish_work",
                "k_mutex_lock(&gateway_discovery_assignment_mutex",
            ),
            (
                "gateway_host_command_work_handler",
                "&gateway_host_command_work",
                "k_msgq_peek(&gateway_host_command_msgq",
            ),
        )
        for function, exact_work, first_operation in worker_expectations:
            with self.subTest(function=function):
                body = function_body(control, function)
                begin = body.index(
                    "mesh_gateway_radio_handoff_begin("
                )
                identity = body.index(exact_work, begin)
                rejection = body.index("if (ret < 0)", identity)
                operation = body.index(first_operation, rejection)
                self.assertLess(begin, identity)
                self.assertLess(identity, rejection)
                self.assertLess(rejection, operation)
                self.assertIn("return;", body[rejection:operation])

        report_begin = function_body(
            report_rx, "mesh_gateway_radio_handoff_begin"
        )
        self.assertIn(
            "app_node_comm_gateway_control_radio_handoff_begin(work)",
            report_begin,
        )

        control_begin = function_body(
            gateway_control,
            "app_node_comm_gateway_control_radio_handoff_begin",
        )
        self.assertIn(
            "app_mesh_radio_owner_gateway_command_begin(",
            control_begin,
        )
        self.assertIn("work, &gateway_radio_handoff", control_begin)

        owner_begin = function_body(
            owner, "app_mesh_radio_owner_gateway_command_begin"
        )
        identity = owner_begin.index(
            "lease_identity_matches(lease_in_out, work)"
        )
        take_grant = owner_begin.index(
            "app_mesh_radio_owner_policy_handoff_take_grant("
        )
        abort_release = owner_begin.index(
            "gateway_abort_release_locked()", take_grant
        )
        self.assertLess(identity, take_grant)
        self.assertLess(take_grant, abort_release)

        release_helper = function_body(
            owner, "gateway_abort_release_locked"
        )
        self.assertIn("abort_release_locked(&gateway_abort)", release_helper)
        release_helper = function_body(owner, "abort_release_locked")
        self.assertIn(
            "app_mesh_radio_owner_policy_abort_release(",
            release_helper,
        )
        self.assertIn("sync_receive_abort_locked()", release_helper)


if __name__ == "__main__":
    unittest.main()
