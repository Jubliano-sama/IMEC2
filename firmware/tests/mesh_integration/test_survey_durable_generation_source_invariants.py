#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
APP = FIRMWARE / "app/src"
GATEWAY_SURVEY = (APP / "app_anchor_gateway_survey.inc").read_text()
GATEWAY_CONTROL = (APP / "app_anchor_gateway_control.inc").read_text()
ANCHOR_RUNTIME = (APP / "app_anchor_survey_runtime.c").read_text()
ANCHOR_DISCOVERY = (APP / "app_anchor_survey_discovery.c").read_text()
ANCHOR_RESULT_DELIVERY = (
    APP / "app_anchor_survey_result_delivery.c"
).read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
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
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


class SurveyDurableGenerationSourceInvariants(unittest.TestCase):
    def test_gateway_reserves_once_at_each_operation_creation_boundary(self) -> None:
        reserve = function_body(
            GATEWAY_SURVEY, "gateway_durable_reserve_survey_generation"
        )
        self.assertIn("app_durable_state_reserve(", reserve)
        self.assertIn("APP_DURABLE_STATE_SURVEY_GENERATION", reserve)
        self.assertIn("gateway_id", reserve)
        self.assertIn("reservation.first", reserve)
        self.assertIn("reservation.reserved_through", reserve)
        self.assertNotIn("gateway_ram_survey_generation_cursor", GATEWAY_SURVEY)
        self.assertNotIn("gateway_ram_reserve_survey_generation", GATEWAY_SURVEY)
        self.assertEqual(GATEWAY_SURVEY.count("app_durable_state_reserve("), 1)

        automatic = function_body(
            GATEWAY_SURVEY, "gateway_route_survey_reachability"
        )
        manual = function_body(
            GATEWAY_CONTROL, "gateway_route_survey_pair_control"
        )
        self.assertEqual(
            automatic.count("gateway_durable_reserve_survey_generation("), 1
        )
        self.assertEqual(
            manual.count("gateway_durable_reserve_survey_generation("), 1
        )

    def test_rejected_automatic_starts_cannot_write_durable_generation(self) -> None:
        automatic = function_body(
            GATEWAY_SURVEY, "gateway_route_survey_reachability"
        )
        capacity = automatic.index(
            "app_gateway_control_sequence_admission_available("
        )
        claim = automatic.index("gateway_operation_owner_claim(")
        reserve = automatic.index("gateway_durable_reserve_survey_generation(")
        publish_generation = automatic.index(
            "config.operation_generation = operation_generation"
        )
        begin = automatic.index("survey_gateway_begin_operation(")
        publish_active = automatic.index("gateway_survey_active = true")

        self.assertLess(capacity, claim)
        self.assertLess(claim, reserve)
        self.assertLess(reserve, publish_generation)
        self.assertLess(publish_generation, begin)
        self.assertLess(begin, publish_active)
        busy_guard = automatic[claim:reserve]
        self.assertIn("if (ret < 0)", busy_guard)
        self.assertIn("return gateway_reject_survey_request(", busy_guard)
        reserve_failure = automatic.index("if (ret < 0)", reserve)
        release = automatic.index(
            "gateway_operation_owner_release(", reserve_failure
        )
        reject = automatic.index(
            "return gateway_reject_survey_request(", release
        )
        self.assertLess(reserve_failure, release)
        self.assertLess(release, reject)

        # Repeating either a runway rejection or a busy-owner rejection
        # re-enters only code before the sole reserve call, so its durable
        # write count remains exactly zero regardless of retry count.
        self.assertEqual(
            automatic[:claim].count(
                "gateway_durable_reserve_survey_generation("
            ),
            0,
        )
        self.assertEqual(
            automatic[claim:reserve].count(
                "gateway_durable_reserve_survey_generation("
            ),
            0,
        )

    def test_anchor_restores_scoped_high_water_before_admission(self) -> None:
        start = function_body(
            ANCHOR_RUNTIME, "app_anchor_survey_runtime_start"
        )
        restore = start.index("app_durable_state_restore_high_water(")
        scope = start.index("GATEWAY_ID", restore)
        error = start.index("if (ret < 0)", scope)
        publish = start.index(
            "survey_generation_high_watermark = restored_generation"
        )
        ready = start.index("survey_generation_restored = true")
        self.assertLess(restore, scope)
        self.assertLess(scope, error)
        self.assertLess(error, publish)
        self.assertLess(publish, ready)
        self.assertLess(
            start.index("survey_generation_restored = false"), restore
        )
        self.assertEqual(
            ANCHOR_RUNTIME.count("app_durable_state_restore_high_water("), 1
        )

    def test_new_generation_is_durable_before_any_successor_state(self) -> None:
        admit = function_body(
            ANCHOR_RUNTIME, "survey_generation_admit_locked"
        )
        advance_guard = admit.index(
            "generation > survey_generation_high_watermark"
        )
        durable = admit.index("app_durable_state_advance_high_water(")
        failure = admit.index("if (ret < 0)", durable)
        failure_return = admit.index("return ret", failure)
        high_water = admit.index(
            "survey_generation_high_watermark = generation", failure_return
        )
        supersede = admit.index("survey_generation_active != 0u", high_water)
        clear_old = admit.index("survey_generation_active = 0u", supersede)
        abandon_old = admit.index(
            "app_anchor_survey_runtime_abandon_pair_start_delivery(",
            clear_old,
        )
        active = admit.index("survey_generation_active = generation", abandon_old)
        retire_pairs = admit.index(
            "app_anchor_survey_result_delivery_supersede_before(", active
        )
        retire_discovery = admit.index(
            "app_anchor_survey_discovery_supersede_before(", retire_pairs
        )
        self.assertLess(advance_guard, durable)
        self.assertLess(durable, failure)
        self.assertLess(failure_return, high_water)
        self.assertLess(high_water, supersede)
        self.assertLess(supersede, clear_old)
        self.assertLess(clear_old, abandon_old)
        self.assertLess(abandon_old, active)
        self.assertLess(active, retire_pairs)
        self.assertLess(retire_pairs, retire_discovery)
        self.assertIn("APP_DURABLE_STATE_SURVEY_GENERATION", admit)
        self.assertIn("GATEWAY_ID", admit)
        self.assertEqual(
            ANCHOR_RUNTIME.count("app_durable_state_advance_high_water("), 1
        )

    def test_exact_replay_is_idempotent_but_rollback_fails_closed(self) -> None:
        admit = function_body(
            ANCHOR_RUNTIME, "survey_generation_admit_locked"
        )
        rollback = re.search(
            r"if \(generation < survey_generation_high_watermark\) \{\s*"
            r"return -ESTALE;\s*\}",
            admit,
        )
        self.assertIsNotNone(rollback)
        self.assertNotRegex(
            admit,
            r"if \(generation == survey_generation_active\) \{\s*"
            r"return 0;\s*\}",
        )
        # Exact replay must re-run the idempotent retirement checks so a
        # successor redrive can observe that old handle teardown completed.
        self.assertLess(
            admit.index("generation < survey_generation_high_watermark"),
            admit.index("app_anchor_survey_result_delivery_supersede_before("),
        )
        self.assertLess(
            admit.index("app_anchor_survey_result_delivery_supersede_before("),
            admit.index("app_anchor_survey_discovery_supersede_before("),
        )
        self.assertNotIn(
            "generation == survey_generation_high_watermark &&", admit
        )
        self.assertLess(
            admit.index("generation > survey_generation_high_watermark"),
            admit.rindex("survey_generation_active = generation"),
        )

    def test_new_generation_is_explicit_exact_handle_repair_boundary(self) -> None:
        pair_supersede = function_body(
            ANCHOR_RESULT_DELIVERY,
            "app_anchor_survey_result_delivery_supersede_before",
        )
        pair_stage = function_body(
            ANCHOR_RESULT_DELIVERY,
            "app_anchor_survey_result_delivery_stage_reserved",
        )
        pair_service = function_body(
            ANCHOR_RESULT_DELIVERY, "result_delivery_service_slot"
        )
        discovery_supersede = function_body(
            ANCHOR_DISCOVERY,
            "app_anchor_survey_discovery_supersede_before",
        )
        discovery_service = function_body(
            ANCHOR_DISCOVERY, "survey_delivery_service_retirement"
        )
        discovery_retry = function_body(
            ANCHOR_DISCOVERY, "app_anchor_survey_discovery_retry_report"
        )
        discovery_prepare = function_body(
            ANCHOR_DISCOVERY, "prepare_discovery_report"
        )

        self.assertIn(
            "operation_generation < result_delivery_generation_floor",
            pair_supersede,
        )
        self.assertLess(
            pair_supersede.index(
                "slot->reservation_owner_generation > operation_generation"
            ),
            pair_supersede.index(
                "result_delivery_generation_floor = operation_generation"
            ),
        )
        self.assertIn("slot->retirement_in_progress = true", pair_supersede)
        self.assertIn("return -EINPROGRESS", pair_supersede)
        self.assertIn(
            "sample.pair.operation_generation <\n"
            "        result_delivery_generation_floor",
            pair_stage,
        )
        self.assertLess(
            pair_service.index("app_node_comm_abandon_delivery(handle)"),
            pair_service.index("app_node_comm_delivery_handle_state(handle)"),
        )
        self.assertLess(
            pair_service.index("app_node_comm_delivery_handle_state(handle)"),
            pair_service.index("app_mesh_local_delivery_cancel(&slot->delivery)"),
        )

        self.assertLess(
            discovery_supersede.index(
                "active_generation > operation_generation"
            ),
            discovery_supersede.index(
                "survey_delivery_generation_floor = operation_generation"
            ),
        )
        self.assertIn(
            "survey_delivery_retirement_in_progress = true",
            discovery_supersede,
        )
        self.assertLess(
            discovery_service.index("app_node_comm_abandon_delivery(handle)"),
            discovery_service.index("app_node_comm_delivery_handle_state(handle)"),
        )
        self.assertLess(
            discovery_service.index("app_node_comm_delivery_handle_state(handle)"),
            discovery_service.index("app_mesh_local_delivery_cancel(delivery)"),
        )
        self.assertLess(
            discovery_retry.index("survey_delivery_service_retirement()"),
            discovery_retry.index("survey_delivery_poll_comm_result()"),
        )
        self.assertLess(
            discovery_retry.index("survey_delivery_service_retirement()"),
            discovery_retry.index("app_node_comm_submit_delivery("),
        )
        self.assertIn(
            "operation_generation < survey_delivery_generation_floor",
            discovery_prepare,
        )

    def test_discovery_admission_advances_then_drains_old_custody(self) -> None:
        admission = function_body(
            ANCHOR_RUNTIME, "app_anchor_survey_runtime_admit_discovery"
        )
        generation = admission.index("survey_generation_admit_locked(")
        pair_custody = admission.index(
            "app_anchor_survey_result_delivery_occupied_count()", generation
        )
        discovery_custody = admission.index(
            "app_anchor_survey_discovery_report_custody_status(", pair_custody
        )
        self.assertLess(generation, pair_custody)
        self.assertLess(pair_custody, discovery_custody)
        self.assertNotIn(
            "app_anchor_survey_result_delivery_occupied_count()",
            admission[:generation],
        )
        self.assertNotIn(
            "app_anchor_survey_discovery_report_custody_status(",
            admission[:generation],
        )

    def test_generation_io_is_out_of_retry_and_deadline_callbacks(self) -> None:
        for callback in (
            "survey_work_handler",
            "pair_lease_work_handler",
            "pair_start_kick_work_handler",
            "app_anchor_survey_runtime_schedule_discovery_custody_ms",
        ):
            body = function_body(ANCHOR_RUNTIME, callback)
            self.assertNotIn("app_durable_state_", body)


if __name__ == "__main__":
    unittest.main()
