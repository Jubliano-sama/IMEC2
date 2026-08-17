#!/usr/bin/env python3
"""Keep immutable survey phase clocks ahead of progress-only barriers."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text()
CONTROL = (ROOT / "app/src/app_anchor_gateway_control.inc").read_text()
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text()
ROUND = (ROOT / "app/src/app_anchor_gateway_survey_round.inc").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end()
    depth = 1
    index = start
    while index < len(source) and depth != 0:
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        index += 1
    if depth != 0:
        raise AssertionError(f"unterminated function {name}")
    return source[start : index - 1]


class GatewaySurveyPhaseOwnerOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.worker = function_body(CONTROL, "gateway_survey_work_handler")
        cls.deadline = cls.worker.index("if (uptime_deadline_reached")
        cls.cleanup = cls.worker.index(
            "gateway_survey_service_cleanup()", cls.deadline
        )
        cls.collection = cls.worker.index(
            "gateway_survey_wait_for_discovery_collection()", cls.cleanup
        )
        cls.boundary = cls.worker.index(
            "gateway_survey_flush_boundary_event()"
        )
        cls.round_drive = cls.worker.index(
            "gateway_survey_round_drive()", cls.boundary
        )

    def test_missing_pair_result_confirm_cannot_mask_observation_deadline(
        self,
    ) -> None:
        self.assertNotIn(
            "gateway_survey_operation_ack_confirm_blocks_progress",
            self.worker,
        )

    def test_pending_command_result_confirm_cannot_mask_cleanup_deadline(
        self,
    ) -> None:
        self.assertLess(self.cleanup, self.round_drive)

    def test_terminal_control_confirm_cannot_mask_observation_or_cleanup(
        self,
    ) -> None:
        self.assertLess(self.deadline, self.round_drive)
        self.assertLess(self.cleanup, self.round_drive)

    def test_ble_boundary_backpressure_cannot_starve_phase_owners(self) -> None:
        self.assertLess(self.deadline, self.boundary)
        self.assertLess(self.cleanup, self.boundary)
        self.assertLess(self.collection, self.boundary)

    def test_round_is_the_only_pair_progress_owner(self) -> None:
        self.assertLess(self.boundary, self.round_drive)
        for retired in (
            "gateway_survey_auto",
            "survey_gateway_auto_next_action",
            "survey_gateway_auto_",
            "gateway_survey_finalize_pair_observation",
        ):
            self.assertNotIn(retired, self.worker)

    def test_parallel_round_completion_uses_responder_samples_only(
        self,
    ) -> None:
        finalize = function_body(
            ROUND, "gateway_survey_round_finalize_observation"
        )
        preferred = finalize.index(
            "survey_pair_round_lane_preferred_results_complete"
        )

        self.assertNotIn("ack_confirm", finalize)
        self.assertNotIn(
            "survey_pair_round_lane_results_complete",
            finalize,
            "deadline fallback must not accept non-responder evidence",
        )

    def test_fully_observed_unusable_attempt_reruns_before_terminal_failure(
        self,
    ) -> None:
        note_sample = function_body(
            ROUND, "gateway_survey_round_note_sample"
        )
        finalize = function_body(
            ROUND, "gateway_survey_round_finalize_observation"
        )

        self.assertIn(
            "survey_pair_round_lane_missing_samples_all_unusable(lane)",
            note_sample,
        )
        self.assertIn(
            "SURVEY_GATEWAY_DUE_ROUND_OBSERVATION, 0u", note_sample
        )
        self.assertIn(
            "if (!deadline && !all_missing_samples_unusable)", finalize
        )
        decision = finalize.index("final_failure = lane->reruns_started >=")
        retry = finalize.index("SURVEY_PAIR_ROUND_CLEANUP_RETRY")
        self.assertLess(decision, retry)
        self.assertNotIn(
            "all_missing_samples_unusable ||",
            finalize[decision:retry],
            "responder timeouts are missing initiator evidence, not a spent rerun",
        )
        self.assertIn(
            "gateway_survey_max_pair_reruns",
            finalize[decision:retry],
        )

    def test_round_drive_gates_successor_on_exact_terminal_proof(self) -> None:
        drive = function_body(ROUND, "gateway_survey_round_drive")
        apply_confirmation = drive.index(
            "gateway_survey_round_apply_control_confirmation()"
        )
        confirmation_pending = drive.index(
            "app_gateway_survey_round_control_confirmation_pending(",
            apply_confirmation,
        )
        exact_transaction = drive.index(
            "gateway_survey_control_inflight()", confirmation_pending
        )
        current_control = drive.index(
            "app_gateway_survey_round_current_control(", exact_transaction
        )
        send = drive.index("gateway_survey_send_control(&control)", current_control)

        self.assertLess(apply_confirmation, confirmation_pending)
        self.assertLess(confirmation_pending, exact_transaction)
        self.assertLess(exact_transaction, current_control)
        self.assertLess(current_control, send)

    def test_generic_barrier_cannot_reblock_phase_owned_pair_results(
        self,
    ) -> None:
        self.assertNotIn(
            "gateway_survey_operation_ack_confirm_blocks_progress",
            SURVEY + CONTROL,
        )

    def test_cleanup_command_ok_is_not_a_generic_command_confirm_barrier(
        self,
    ) -> None:
        cleanup_terminal = function_body(
            SURVEY, "gateway_survey_cleanup_note_command_terminal"
        )

        self.assertNotIn("gateway_survey_control_ack_confirm_note", SURVEY)
        self.assertNotIn(
            "gateway_survey_control_ack_confirm_pending", cleanup_terminal
        )

    def test_auto_admission_reserves_volatile_owners_before_durable_generation(
        self,
    ) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        runway = route.index(
            "app_gateway_control_sequence_admission_available("
        )
        claim = route.index(
            "gateway_operation_owner_claim(", runway
        )
        reservation = route.index(
            "app_node_comm_reserve_bounded_control(", claim
        )
        durable_generation = route.index(
            "gateway_durable_reserve_survey_generation(", reservation
        )
        successor = route.index(
            "survey_gateway_begin_operation(", durable_generation
        )
        active = route.index("gateway_survey_active = true", successor)
        commit = route.index(
            "app_node_comm_commit_bounded_control_reservation(", active
        )

        self.assertLess(runway, claim)
        self.assertLess(claim, reservation)
        self.assertLess(reservation, durable_generation)
        self.assertLess(durable_generation, successor)
        self.assertLess(successor, active)
        self.assertLess(active, commit)
        self.assertIn(
            "&discovery_reservation",
            route[commit : commit + 300],
            "the accepted discovery must commit the exact capacity lease",
        )

    def test_auto_post_reservation_failures_cancel_before_owner_release(
        self,
    ) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        durable_generation = route.index(
            "gateway_durable_reserve_survey_generation("
        )
        commit = route.index(
            "app_node_comm_commit_bounded_control_reservation(",
            durable_generation,
        )
        cursor = durable_generation
        releases = 0

        for match in re.finditer(
            r"gateway_operation_owner_release\s*\(\s*"
            r"&gateway_auto_survey_operation_lease\s*\)",
            route[durable_generation:commit],
        ):
            release = durable_generation + match.start()
            self.assertIn(
                "gateway_survey_cancel_control_reservation(",
                route[cursor:release],
                "every failure after capacity reservation must retire that "
                "exact reservation before releasing the operation lease",
            )
            releases += 1
            cursor = durable_generation + match.end()

        self.assertGreaterEqual(
            releases,
            5,
            "the invariant must cover every fallible construction step before commit",
        )

        commit_failure = route.index("if (ret < 0)", commit)
        commit_failure_end = route.index(
            "return gateway_reject_survey_request(", commit_failure
        )
        failure = route[commit_failure:commit_failure_end]
        cancel = failure.index("gateway_survey_cancel_control_reservation(")
        schedule_reset = failure.index("gateway_survey_work_reset_schedule()")
        release = failure.index("gateway_operation_owner_release(")
        self.assertLess(cancel, schedule_reset)
        self.assertLess(schedule_reset, release)

    def test_auto_terminal_teardown_cannot_cancel_a_successor_schedule(
        self,
    ) -> None:
        finish = function_body(
            SURVEY, "gateway_survey_finish_cleanup_if_complete"
        )
        inactive = finish.index("if (!gateway_survey_active)")
        reset_round = finish.index("gateway_survey_round_reset()", inactive)
        reset_schedule = finish.index(
            "gateway_survey_work_reset_schedule()", reset_round
        )
        release = finish.index(
            "gateway_operation_owner_release(", reset_schedule
        )

        self.assertLess(inactive, reset_round)
        self.assertLess(reset_round, reset_schedule)
        self.assertLess(reset_schedule, release)

    def test_exact_owner_query_is_serialized_with_claim_and_release(self) -> None:
        claim = function_body(ANCHOR, "gateway_operation_owner_claim")
        claim_lock = claim.index(
            "k_spin_lock(&gateway_operation_owner_lock)"
        )
        claim_policy = claim.index(
            "app_gateway_operation_owner_claim(", claim_lock
        )
        claim_unlock = claim.index(
            "k_spin_unlock(&gateway_operation_owner_lock", claim_policy
        )
        self.assertLess(claim_lock, claim_policy)
        self.assertLess(claim_policy, claim_unlock)

        release = function_body(ANCHOR, "gateway_operation_owner_release")
        release_lock = release.index(
            "k_spin_lock(&gateway_operation_owner_lock)"
        )
        release_policy = release.index(
            "app_gateway_operation_owner_release(", release_lock
        )
        release_clear = release.index("memset(lease, 0", release_policy)
        release_unlock = release.index(
            "k_spin_unlock(&gateway_operation_owner_lock", release_clear
        )
        self.assertLess(release_lock, release_policy)
        self.assertLess(release_policy, release_clear)
        self.assertLess(release_clear, release_unlock)

        matches = function_body(ANCHOR, "gateway_operation_owner_matches")
        lock = matches.index(
            "k_spin_lock(&gateway_operation_owner_lock)"
        )
        lease_valid = matches.index(
            "app_gateway_operation_lease_valid(lease)", lock
        )
        active_owner = matches.index(
            "gateway_operation_owner.active.owner == lease->owner",
            lease_valid,
        )
        active_generation = matches.index(
            "gateway_operation_owner.active.generation == lease->generation",
            active_owner,
        )
        unlock = matches.index(
            "k_spin_unlock(&gateway_operation_owner_lock", active_generation
        )

        self.assertLess(lock, lease_valid)
        self.assertLess(lease_valid, active_owner)
        self.assertLess(active_owner, active_generation)
        self.assertLess(active_generation, unlock)

        cleanup_owner = function_body(
            SURVEY, "gateway_survey_cleanup_custody_owned"
        )
        self.assertIn("gateway_operation_owner_matches(", cleanup_owner)
        self.assertNotIn("gateway_operation_owner.active", cleanup_owner)


if __name__ == "__main__":
    unittest.main()
