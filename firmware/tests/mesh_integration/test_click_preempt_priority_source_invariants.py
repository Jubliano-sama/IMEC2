#!/usr/bin/env python3
"""Production-source ordering checks for click priority over timing setup."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app" / "src"
REPORT = (APP / "app_mesh_report.c").read_text()
COORDINATION = (APP / "app_mesh_report_coordination.inc").read_text()
ROUTE_CONTROL = (APP / "app_mesh_report_route_control.inc").read_text()
EVENT_TX = (APP / "app_mesh_report_event_tx.inc").read_text()
ANCHOR_RADIO = (APP / "app_anchor_radio.inc").read_text()
REPORT_RX = (APP / "app_mesh_report_rx.inc").read_text()


def function_body(source: str, name: str) -> str:
    marker = source.index(f"{name}(")
    begin = source.index("{", marker)
    depth = 0
    for offset in range(begin, len(source)):
        char = source[offset]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[begin : offset + 1]
    raise AssertionError(f"unterminated function {name}")


class ClickPreemptPrioritySourceInvariants(unittest.TestCase):
    def test_request_is_published_only_after_both_tokens_are_clean(self) -> None:
        body = function_body(COORDINATION, "mesh_preempt_for_click_event_until")
        preparing = body.index("MESH_CLICK_PREEMPT_REQUEST_PREPARING")
        reset_done = body.index("k_sem_reset(&mesh_click_preempt_done)", preparing)
        reset_wake = body.index(
            "k_sem_reset(&mesh_click_preempt_route_wake)", reset_done
        )
        queued = body.index(
            "MESH_CLICK_PREEMPT_REQUEST_QUEUED", reset_wake
        )
        wake = body.index("k_sem_give(&mesh_click_preempt_route_wake)", queued)
        submit = body.index("mesh_click_preempt_submit_route_kick()", wake)

        self.assertLess(preparing, reset_done)
        self.assertLess(reset_done, reset_wake)
        self.assertLess(reset_wake, queued)
        self.assertLess(queued, wake)
        self.assertLess(wake, submit)
        self.assertIn(
            "physical_deadline_ms - UWB_DISCOVERY_RX_GUARD_MS", body
        )

    def test_speculative_route_kick_does_not_use_accepted_state_watchdog(self) -> None:
        submit = function_body(
            COORDINATION, "mesh_click_preempt_submit_route_kick"
        )

        self.assertTrue(
            "k_work_submit_to_queue(" in submit or "k_work_submit(" in submit
        )
        self.assertNotIn("mesh_submit_owned_work", submit)
        self.assertNotIn("mesh_owned_schedule_result", submit)
        self.assertNotIn("app_watchdog_stop_feeding", submit)

    def test_rejected_route_kick_rolls_back_or_preserves_in_band_owner(self) -> None:
        body = function_body(COORDINATION, "mesh_preempt_for_click_event_until")
        submit = body.index("mesh_click_preempt_submit_route_kick()")
        rejected = body.index("if (ret < 0)", submit)
        queued = body.index("MESH_CLICK_PREEMPT_REQUEST_QUEUED", rejected)
        preparing = body.index("MESH_CLICK_PREEMPT_REQUEST_PREPARING", queued)
        reset_wake = body.index(
            "k_sem_reset(&mesh_click_preempt_route_wake)", preparing
        )
        idle = body.index("MESH_CLICK_PREEMPT_REQUEST_IDLE", reset_wake)
        running = body.index("MESH_CLICK_PREEMPT_REQUEST_RUNNING", idle)
        complete = body.index("MESH_CLICK_PREEMPT_REQUEST_COMPLETE", running)
        wait_loop = body.index("for (;;)", complete)

        self.assertLess(rejected, queued)
        self.assertLess(queued, preparing)
        self.assertLess(preparing, reset_wake)
        self.assertLess(reset_wake, idle)
        self.assertLess(idle, running)
        self.assertLess(running, complete)
        self.assertLess(complete, wait_loop)
        self.assertNotIn(
            "app_watchdog_stop_feeding", body[rejected:wait_loop]
        )

    def test_boundary_observes_only_live_queued_request(self) -> None:
        body = function_body(
            COORDINATION, "mesh_click_preempt_boundary_requested"
        )

        self.assertIn("MESH_CLICK_PREEMPT_REQUEST_QUEUED", body)
        self.assertIn("!uptime_deadline_reached(", body)
        self.assertIn("mesh_click_preempt_request.bridge_deadline_ms", body)

    def test_polite_backoff_has_no_lost_wake_gap(self) -> None:
        body = function_body(
            COORDINATION, "mesh_click_preempt_wait_route_backoff"
        )
        loop = body.index("for (;;)")
        first_check = body.index("mesh_click_preempt_boundary_requested()")
        wait = body.index("k_sem_take(&mesh_click_preempt_route_wake", first_check)

        self.assertLess(loop, first_check)
        self.assertLess(first_check, wait)
        self.assertIn("uptime_deadline_reached(now_ms, deadline_ms)", body)
        self.assertIn("uptime_ms_until_deadline(now_ms, deadline_ms)", body)
        route_backoff = function_body(ROUTE_CONTROL, "mesh_route_wake_backoff")
        self.assertIn("mesh_click_preempt_wait_route_backoff(delay_ms)", route_backoff)
        self.assertNotIn("k_msleep(delay_ms)", route_backoff)

    def test_wake_train_cancels_at_a_frame_boundary(self) -> None:
        body = function_body(
            ROUTE_CONTROL, "mesh_send_route_wake_train_with_duration"
        )
        loop = body.index("while (k_uptime_get() < close_ms)")
        boundary = body.index("mesh_click_preempt_boundary_requested()", loop)
        canceled = body.index("ret = -ECANCELED", boundary)
        leave_loop = body.index("break;", canceled)
        next_frame = body.index("uwb_clicker_build_wake_claim(", boundary)

        self.assertIn("!atomic_gateway_control", body[loop:boundary])
        self.assertLess(boundary, canceled)
        self.assertLess(canceled, leave_loop)
        self.assertLess(leave_loop, next_frame)

    def test_event_proposal_services_after_synchronous_sender_returns(self) -> None:
        body = function_body(
            EVENT_TX, "mesh_propose_event_after_channel5_contact_authorized"
        )
        send = body.index("mesh_send_event_control_record(")
        canceled = body.index("ret == -ECANCELED", send)
        boundary = body.index("mesh_click_preempt_boundary_requested()", canceled)
        service = body.index(
            "mesh_click_preempt_service_queued_route_owned()", boundary
        )

        self.assertLess(send, canceled)
        self.assertLess(canceled, boundary)
        self.assertLess(boundary, service)

    def test_in_band_service_is_route_owned_and_deadline_bounded(self) -> None:
        body = function_body(
            COORDINATION, "mesh_click_preempt_service_queued_route_owned"
        )
        owner = body.index("mesh_click_preempt_on_route_owner()")
        queued = body.index("MESH_CLICK_PREEMPT_REQUEST_QUEUED", owner)
        deadline = body.index("uptime_deadline_reached(", queued)
        running = body.index("MESH_CLICK_PREEMPT_REQUEST_RUNNING", deadline)
        apply = body.index("mesh_click_preempt_run_route_owned(", running)
        complete = body.index("MESH_CLICK_PREEMPT_REQUEST_COMPLETE", apply)
        signal = body.index("k_sem_give(&mesh_click_preempt_done)", complete)

        self.assertLess(owner, queued)
        self.assertLess(queued, deadline)
        self.assertLess(deadline, running)
        self.assertLess(running, apply)
        self.assertLess(apply, complete)
        self.assertLess(complete, signal)

    def test_route_owned_apply_must_finish_before_bridge_deadline(self) -> None:
        body = function_body(
            COORDINATION, "mesh_click_preempt_run_route_owned"
        )
        apply = body.index("app_mesh_apply_click_preempt_plan(")
        post_deadline = body.index("uptime_deadline_reached(", apply)
        fail_stop = body.index("app_watchdog_stop_feeding()", post_deadline)
        timeout = body.index("return -ETIMEDOUT", fail_stop)
        success = body.index("return ret", timeout)

        self.assertLess(apply, post_deadline)
        self.assertLess(post_deadline, fail_stop)
        self.assertLess(fail_stop, timeout)
        self.assertLess(timeout, success)

    def test_direct_route_owner_uses_the_post_apply_bounded_transaction(self) -> None:
        body = function_body(COORDINATION, "mesh_preempt_for_click_event_until")
        owner = body.index("mesh_click_preempt_on_route_owner()")
        run = body.index("mesh_click_preempt_run_route_owned(", owner)
        publish = body.index("mesh_click_preempt_request.generation", run)

        self.assertLess(owner, run)
        self.assertLess(run, publish)

    def test_anchor_request_state_and_wake_token_remain_bounded(self) -> None:
        self.assertIn("MESH_CLICK_PREEMPT_REQUEST_PREPARING", REPORT)
        self.assertIn("static struct k_sem mesh_click_preempt_route_wake;", REPORT)
        self.assertIn("2u * sizeof(struct k_sem)", REPORT)

    def test_frozen_click_handoff_does_not_repeat_deadline_admission(self) -> None:
        body = function_body(ANCHOR_RADIO, "anchor_handle_uwb_claim")
        preempt = body.index("mesh_preempt_for_click_event_until(")
        preempt_guard = body.rfind("if (", 0, preempt)
        reserve = body.index(
            "mesh_range_report_batch_reserve_capacity(", preempt
        )
        reserve_guard = body.rfind("if (", preempt, reserve)

        self.assertIn(
            "click_priority && !admitted_handoff_identity_frozen",
            body[preempt_guard:preempt],
        )
        self.assertIn(
            "click_priority && !admitted_handoff_identity_frozen",
            body[reserve_guard:reserve],
        )
        self.assertIn("DBG_CH_AP_ADMITTED", body[preempt:reserve])

    def test_connected_gap_uses_extended_followup_policy_not_control_bit(self) -> None:
        body = function_body(REPORT_RX, "mesh_uwb_rx_work_handler")
        gap = body.index("channel5_gap_scan")
        handoff = body.index(
            "app_mesh_c5_connected_gap_route_handoff_required(", gap
        )
        action = body.index("app_mesh_c5_connected_gap_rx_action(", handoff)

        self.assertNotIn(
            "app_mesh_c5_wake_followup_is_control", body[gap:action]
        )


if __name__ == "__main__":
    unittest.main()
