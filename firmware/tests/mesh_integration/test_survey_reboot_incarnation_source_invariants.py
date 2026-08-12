#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
APP = FIRMWARE / "app/src"
ANCHOR = (APP / "app_anchor.c").read_text()
DISCOVERY = (APP / "app_anchor_survey_discovery.c").read_text()
DISCOVERY_HEADER = (APP / "app_anchor_survey_discovery.h").read_text()
RESULT = (APP / "app_anchor_survey_result_delivery.c").read_text()
RESULT_HEADER = (APP / "app_anchor_survey_result_delivery.h").read_text()
RUNTIME = (APP / "app_anchor_survey_runtime.c").read_text()
RUNTIME_HEADER = (APP / "app_anchor_survey_runtime.h").read_text()
GATEWAY_SURVEY = (APP / "app_anchor_gateway_survey.inc").read_text()
GATEWAY_CONTROL = (APP / "app_anchor_gateway_control.inc").read_text()
MESH_DELIVERY = (APP / "app_mesh_report_delivery.inc").read_text()
MESH_COORDINATION = (APP / "app_mesh_report_coordination.inc").read_text()
MESH_HEADER = (APP / "app_mesh_report.h").read_text()
LOCAL_DELIVERY = (APP / "app_mesh_local_delivery.c").read_text()
ROUTE_WAIT = (APP / "app_mesh_route_wait_tx.c").read_text()
MESH_EVENT_TX = (APP / "app_mesh_report_event_tx.inc").read_text()
RADIO = (APP / "app_anchor_radio.inc").read_text()
ANCHOR_INIT = (APP / "app_anchor_init.inc").read_text()
MESH = (FIRMWARE / "src/mesh.c").read_text()
SURVEY = (FIRMWARE / "src/survey.c").read_text()
SURVEY_HEADER = (FIRMWARE / "include/survey.h").read_text()
DEADLINE_HEADER = (FIRMWARE / "include/survey_anchor_deadline.h").read_text()
DEADLINE_SOURCE = (FIRMWARE / "src/survey_anchor_deadline.c").read_text()


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


class SurveyRebootIncarnationSourceInvariants(unittest.TestCase):
    def test_volatile_reset_has_no_restore_owner_or_startup_branch(self) -> None:
        false_restore_surface = "\n".join(
            (
                RESULT,
                RESULT_HEADER,
                DISCOVERY,
                DISCOVERY_HEADER,
                RUNTIME,
                RUNTIME_HEADER,
                ANCHOR_INIT,
                MESH_COORDINATION,
                MESH_HEADER,
                DEADLINE_HEADER,
                DEADLINE_SOURCE,
            )
        )
        for forbidden in (
            "app_anchor_survey_" + "discovery_restore",
            "app_anchor_survey_" + "result_delivery_restore",
            "delivery_" + "restored",
            "result_delivery_" + "restored",
            "resume_" + "restored_outbox",
            "DBG_OUTBOX_" + "RESTORE",
            "DBG_SURVEY_PAIR_RESULT_" + "RESTORED",
            "seed_" + "sequence",
            "app_anchor_survey_runtime_" + "schedule_ms",
        ):
            self.assertNotIn(forbidden, false_restore_surface)
        self.assertNotIn(
            "SURVEY_ANCHOR_DEADLINE_" + "GENERAL_POLL", false_restore_surface
        )
        self.assertNotIn(
            "APP_MESH_LOCAL_DELIVERY_" + "RECOVERY_WAIT", DISCOVERY
        )
        self.assertNotIn("report_" + "durable", RUNTIME)
        self.assertNotIn("ALREADY_" + "DURABLE", RUNTIME)
        self.assertNotIn("deferred_" + "ret", MESH_COORDINATION)
        for source in (RESULT, DISCOVERY):
            self.assertNotRegex(source, r"\bnvs_(?:read|write|delete|clear)\s*\(")
        self.assertIn("bounded RAM slot", RESULT_HEADER)
        self.assertIn(
            "APP_MESH_SURVEY_PAIR_RESULT_DELIVERY_SLOTS", RESULT_HEADER
        )
        self.assertNotIn("anchor-local NVS slots", SURVEY_HEADER)

        discovery_service = function_body(
            DISCOVERY, "app_anchor_survey_discovery_retry_report"
        )
        discovery_match = discovery_service.index(
            "mesh_report_active_owner_matches_outbound"
        )
        discovery_wake = discovery_service.index(
            "mesh_report_wake_active_outbox", discovery_match
        )
        result_service = function_body(RESULT, "result_delivery_service_slot")
        result_match = result_service.index("active_owner_matches_outbound")
        result_wake = result_service.index("wake_active_outbox", result_match)
        self.assertLess(discovery_match, discovery_wake)
        self.assertLess(result_match, result_wake)

        wake = function_body(
            MESH_COORDINATION, "mesh_report_wake_active_outbox"
        )
        self.assertIn("mesh_relay_tx_active", wake)
        self.assertIn("mesh_schedule_tx_timeout()", wake)
        anchor_start = function_body(ANCHOR_INIT, "app_anchor_start_anchor_role")
        self.assertNotIn("mesh_report_wake_active_outbox", anchor_start)

    def test_discovery_report_carries_mandatory_boot_baseline(self) -> None:
        producer = function_body(DISCOVERY, "prepare_discovery_report")
        boot_get = producer.index(
            "discovery_ops.boot_incarnation(&boot_incarnation)"
        )
        boot_nonzero = producer.index("if (boot_incarnation == 0u)", boot_get)
        boot_tlv = producer.index("TLV_NODE_BOOT_COUNTER", boot_nonzero)
        status_tlv = producer.index("TLV_COMMAND_STATUS", boot_tlv)
        packet_init = producer.index(
            "survey_init_discovery_report_packet", status_tlv
        )
        self.assertLess(boot_get, boot_nonzero)
        self.assertLess(boot_nonzero, boot_tlv)
        self.assertLess(boot_tlv, status_tlv)
        self.assertLess(status_tlv, packet_init)
        self.assertRegex(
            producer[packet_init:],
            r"survey_init_discovery_report_packet\([^;]*?"
            r"operation_generation,\s*boot_incarnation,\s*sequence,",
        )

        validator = function_body(
            MESH, "mesh_survey_discovery_report_payload_validate"
        )
        self.assertRegex(
            validator,
            r"tlv_find_unique\(payload,\s*payload_len,\s*"
            r"TLV_NODE_BOOT_COUNTER",
        )
        self.assertIn("boot_incarnation == 0u", validator)
        self.assertIn("packet->session_id != boot_incarnation", validator)
        canonical = function_body(
            LOCAL_DELIVERY, "delivery_discovery_report_valid"
        )
        self.assertIn("TLV_NODE_BOOT_COUNTER", canonical)
        self.assertRegex(
            canonical,
            r"survey_init_discovery_report_packet\([^;]*?"
            r"operation_generation,\s*boot_incarnation,",
        )
        self.assertRegex(
            SURVEY_HEADER,
            r"SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN\s+\\\n\s*"
            r"\(2u \* PROTO_TLV_U32_ENCODED_LEN",
        )

        preflight = function_body(
            GATEWAY_SURVEY, "gateway_survey_preflight_discovery_report"
        )
        commit = function_body(
            GATEWAY_SURVEY, "gateway_handle_survey_discovery_report"
        )
        self.assertIn("packet->session_id != boot_incarnation", preflight)
        self.assertIn("packet->session_id != boot_incarnation", commit)

        retry_identity = function_body(
            ROUTE_WAIT, "app_mesh_route_retry_identity_select"
        )
        self.assertNotIn("generation != packet->session_id", retry_identity)
        self.assertRegex(
            retry_identity,
            r"OWNER_RETAINED_LOCAL\s*\?\s*generation\s*:\s*"
            r"packet->session_id",
        )
        owned_preflight = function_body(
            MESH_EVENT_TX, "mesh_owned_tracked_tx_preflight"
        )
        self.assertNotIn("generation != out->packet.session_id", owned_preflight)

    def test_discovery_sequence_exhaustion_is_nonwrapping(self) -> None:
        sequence = function_body(SURVEY, "survey_discovery_sequence_next")
        self.assertIn("*sequence_state == UINT16_MAX", sequence)
        self.assertIn("return 0u", sequence)
        self.assertNotIn("*sequence_state = 1u", sequence)
        next_sequence = function_body(
            RUNTIME, "app_anchor_survey_runtime_next_sequence"
        )
        self.assertIn("survey_discovery_sequence_next", next_sequence)
        producer = function_body(DISCOVERY, "prepare_discovery_report")
        exhausted = producer.index("if (sequence == 0u)")
        fail_closed = producer.index("return -EOVERFLOW", exhausted)
        packet_init = producer.index(
            "survey_init_discovery_report_packet", fail_closed
        )
        self.assertLess(exhausted, fail_closed)
        self.assertLess(fail_closed, packet_init)

    def test_boot_observation_precedes_host_custody_and_has_a_wake_owner(self) -> None:
        drain = function_body(MESH_DELIVERY, "mesh_drain_rx_queue_locked")
        preflight = drain.index("mesh_gateway_preflight_semantic_delivery(pending)")
        nonnegative = drain.index("if (semantic_ret >= 0", preflight)
        observe = drain.index("gateway_note_anchor_boot_observation", nonnegative)
        reserve = drain.index("gateway_ble_reserve_stream_packet", observe)
        self.assertLess(preflight, nonnegative)
        self.assertLess(nonnegative, observe)
        self.assertLess(observe, reserve)
        self.assertIn(".gateway_note_anchor_boot_observation", RADIO)

        callback = function_body(
            GATEWAY_SURVEY, "gateway_note_anchor_boot_observation"
        )
        note = callback.index("app_gateway_survey_incarnation_tracker_note")
        retain = callback.index("gateway_survey_incarnation_event =", note)
        schedule = callback.index("gateway_survey_work_schedule(", retain)
        fail_check = callback.index("< 0", schedule)
        fail_stop = callback.index("app_watchdog_stop_feeding()", fail_check)
        self.assertLess(note, retain)
        self.assertLess(retain, schedule)
        self.assertLess(schedule, fail_check)
        self.assertLess(fail_check, fail_stop)

    def test_discovery_baseline_commits_before_roster_mutation(self) -> None:
        preflight = function_body(
            GATEWAY_SURVEY, "gateway_survey_preflight_discovery_report"
        )
        boot = preflight.index("gateway_survey_discovery_boot_incarnation")
        classify = preflight.index(
            "app_gateway_survey_incarnation_tracker_classify", boot
        )
        compare = preflight.index("survey_gateway_reach_report_compare", classify)
        self.assertLess(boot, classify)
        self.assertLess(classify, compare)

        commit = function_body(
            GATEWAY_SURVEY, "gateway_handle_survey_discovery_report"
        )
        observe = commit.index("gateway_note_anchor_boot_observation")
        reject_doomed = commit.index(
            "gateway_survey_incarnation_event_is_current", observe
        )
        roster = commit.index(
            "survey_gateway_note_reach_report_with_reverse_hint_status",
            reject_doomed,
        )
        self.assertLess(observe, reject_doomed)
        self.assertLess(reject_doomed, roster)

        accepted = commit.index(
            "survey_gateway_note_reach_report_with_reverse_hint_status",
            roster,
        )
        sequence = commit.index(
            "gateway_survey_discovery_report_sequences[i] = packet->seq",
            accepted,
        )
        self.assertLess(accepted, sequence)

    def test_discovery_ack_confirm_matches_boot_and_sequence(self) -> None:
        self.assertIn(
            "gateway_survey_discovery_report_sequences", ANCHOR
        )
        self.assertIn(
            "sizeof(gateway_survey_discovery_report_sequences) == 100u",
            ANCHOR,
        )
        ack = function_body(GATEWAY_SURVEY, "gateway_note_survey_ack_confirm")
        bypass = ack.index(
            "identity->msg_type != MSG_SURVEY_DISCOVERY_REPORT"
        )
        operation_session = ack.index(
            "identity->session_id != expected_session", bypass
        )
        boot = ack.index(
            "app_gateway_survey_incarnation_tracker_classify",
            operation_session,
        )
        sequence = ack.index(
            "gateway_survey_discovery_report_sequences[i] != identity->seq",
            boot,
        )
        confirm = ack.index(
            "gateway_survey_discovery_ack_confirm_mask |=", sequence
        )
        self.assertLess(bypass, operation_session)
        self.assertLess(operation_session, boot)
        self.assertLess(boot, sequence)
        self.assertLess(sequence, confirm)

    def test_worker_revalidates_frozen_event_before_exact_cleanup(self) -> None:
        worker = function_body(GATEWAY_CONTROL, "gateway_survey_work_handler")
        due = worker.index("gateway_survey_work_consume_due()")
        reboot = worker.index("gateway_survey_consume_incarnation_event()", due)
        manual = worker.index("gateway_manual_survey_control_service()", reboot)
        delivery = worker.index("gateway_survey_service_active_delivery()", reboot)
        self.assertLess(due, reboot)
        self.assertLess(reboot, manual)
        self.assertLess(reboot, delivery)

        consume = function_body(
            GATEWAY_SURVEY, "gateway_survey_consume_incarnation_event"
        )
        clear = consume.index("memset(&gateway_survey_incarnation_event")
        generation = consume.index(
            "gateway_survey_context.operation_generation", clear
        )
        lease = consume.index("event.owner_lease_generation", generation)
        interval = consume.index("gateway_survey_receive_in_interval", lease)
        membership = consume.index("gateway_survey_anchor_affects_active", interval)
        finish = consume.index("gateway_survey_finish_status", membership)
        self.assertLess(clear, generation)
        self.assertLess(generation, lease)
        self.assertLess(lease, interval)
        self.assertLess(interval, membership)
        self.assertLess(membership, finish)

        anchor_start = function_body(ANCHOR_INIT, "app_anchor_start_anchor_role")
        radio = anchor_start.index("anchor_start_uwb_scan()")
        heartbeat = anchor_start.index("anchor_heartbeat_request_startup()", radio)
        self.assertLess(radio, heartbeat)


if __name__ == "__main__":
    unittest.main()
