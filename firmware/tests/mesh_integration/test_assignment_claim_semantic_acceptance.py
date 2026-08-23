#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
ANCHOR_HEADER = (ROOT / "app/src/app_anchor.h").read_text(encoding="utf-8")
GATEWAY_CONTROL = (
    ROOT / "app/src/app_anchor_gateway_control.inc"
).read_text(encoding="utf-8")
SURVEY_DISCOVERY = read_composed_source(
    ROOT / "app/src/app_anchor_survey_discovery.c"
)
GATEWAY = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
RX_POLICY = (ROOT / "app/src/app_mesh_rx_policy.c").read_text(
    encoding="utf-8"
)
RX_POLICY_HEADER = (ROOT / "app/src/app_mesh_rx_policy.h").read_text(
    encoding="utf-8"
)
ROUTE_REFRESH_HEADER = (
    ROOT / "app/src/app_node_comm_gateway_route_refresh.h"
).read_text(encoding="utf-8")
RELAY = (ROOT / "src/mesh_relay.c").read_text(encoding="utf-8")
RELAY_CUSTODY = (ROOT / "src/mesh_relay_custody.inc").read_text(
    encoding="utf-8"
)
DISCOVERY_ASSIGNMENT = (
    ROOT / "src/discovery_assignment.c"
).read_text(encoding="utf-8")
DISCOVERY_ASSIGNMENT_HEADER = (
    ROOT / "include/discovery_assignment.h"
).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{re.escape(name)}\s*\(", source):
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
            brace = next_index
            depth = 0
            for end in range(brace, len(source)):
                depth += source[end] == "{"
                depth -= source[end] == "}"
                if depth == 0:
                    return source[brace : end + 1]
            raise AssertionError(f"unterminated function: {name}")
    raise AssertionError(f"function not found: {name}")


def has_owned_return(fragment: str, value: str) -> bool:
    return f"return {value};" in fragment or \
        f"GATEWAY_ASSIGNMENT_RETURN({value})" in fragment


class AssignmentClaimSemanticAcceptanceTests(unittest.TestCase):
    def test_survey_accepts_the_occupied_span_of_the_exact_assignment(self):
        match = function_body(
            SURVEY_DISCOVERY, "survey_assignment_matches_config"
        )

        self.assertIn("epoch != config->assignment_epoch", match)
        self.assertIn("table_seq != config->assignment_table_seq", match)
        self.assertIn(
            "discovery_assignment_table_commitment_equal", match
        )
        self.assertIn("config->slot_count > slot_count", match)
        self.assertIn("slot >= config->slot_count", match)
        self.assertNotIn("slot_count != config->slot_count", match)

    def test_first_contact_route_depth_can_only_postpone_before_submit(self):
        delay = function_body(ANCHOR, "anchor_discovery_response_delay_ms")
        postpone = function_body(
            ANCHOR, "anchor_discovery_postpone_for_deeper_route"
        )
        worker = function_body(ANCHOR, "anchor_discovery_claim_work_handler")
        schedule = function_body(ANCHOR, "anchor_schedule_discovery_response")

        self.assertIn("pending->first_contact_random", delay)
        self.assertIn("first_contact_origin_ms", schedule)
        self.assertIn("first_contact_random = sys_rand32_get()", schedule)
        self.assertIn("hop_count <= snapshot->hop_count", postpone)
        self.assertIn(
            "hop_count > anchor_discovery_claim_pending.hop_count", postpone
        )
        self.assertIn(
            "not_before_ms >\n            anchor_discovery_claim_pending.next_attempt_not_before_ms",
            postpone,
        )
        refresh = worker.index("anchor_discovery_postpone_for_deeper_route")
        submit = worker.index("anchor_send_discovery_response")
        self.assertLess(refresh, submit)

    def test_survey_report_depth_postpones_retained_not_before(self):
        retry = function_body(
            SURVEY_DISCOVERY, "app_anchor_survey_discovery_retry_report"
        )
        prepare = function_body(SURVEY_DISCOVERY, "prepare_discovery_report")

        depth = retry.index(
            "gateway_hop_count > survey_report_first_contact_schedule.hop_count"
        )
        postpone = retry.index(
            "app_mesh_local_delivery_postpone_not_before", depth
        )
        retire = retry.index(
            "app_mesh_local_delivery_retire_elapsed_not_before", postpone
        )
        submit = retry.index("app_node_comm_submit_delivery", retire)
        self.assertLess(depth, postpone)
        self.assertLess(postpone, retire)
        self.assertLess(retire, submit)
        self.assertIn("survey_report_first_contact_remember", prepare)

    def test_survey_converts_zero_based_route_hops_to_rf_depth(self):
        selected_depth = function_body(
            REPORT, "app_mesh_report_selected_gateway_hop_count"
        )
        survey_depth = function_body(
            SURVEY_DISCOVERY, "survey_report_gateway_hop_count"
        )

        select = selected_depth.index("route_selected(&mesh_runtime.upstream)")
        unknown = selected_depth.index(
            "selected == NULL || selected->hop_count == UINT8_MAX", select
        )
        unknown_return = selected_depth.index("return 0u", unknown)
        conversion = re.search(
            r"return\s+\(uint8_t\)\(selected->hop_count\s*\+\s*"
            r"(\d+)u\);",
            selected_depth,
        )
        self.assertIsNotNone(conversion)
        convert = conversion.start()
        lookup = survey_depth.index(
            "app_mesh_report_selected_gateway_hop_count()"
        )
        fallback = survey_depth.index(
            "hop_count == 0u || hop_count > SURVEY_DEFAULT_TTL", lookup
        )
        conservative = survey_depth.index(
            "SURVEY_DEFAULT_TTL : hop_count", fallback
        )

        self.assertLess(select, unknown)
        self.assertLess(unknown, unknown_return)
        self.assertLess(unknown_return, convert)
        self.assertNotIn("return selected->hop_count;", selected_depth)
        self.assertLess(lookup, fallback)
        self.assertLess(fallback, conservative)
        self.assertEqual(
            len(re.findall("selected->hop_count", selected_depth)), 2
        )
        self.assertEqual(survey_depth.count("SURVEY_DEFAULT_TTL"), 2)

        # A direct route stores zero intermediate relays, while its child
        # stores one. Survey scheduling needs their one- and two-hop RF depth.
        route_to_rf_offset = int(conversion.group(1))
        self.assertEqual(0 + route_to_rf_offset, 1)
        self.assertEqual(1 + route_to_rf_offset, 2)

    def test_table_ack_window_owns_the_shared_fast_retry_contract(self):
        self.assertRegex(
            DISCOVERY_ASSIGNMENT_HEADER,
            r"#define\s+DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES\s+3u\b",
        )
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS",
            DISCOVERY_ASSIGNMENT_HEADER,
        )
        self.assertIn(
            "discovery_assignment_table_collection_window_for_topology_ms",
            DISCOVERY_ASSIGNMENT_HEADER,
        )
        self.assertNotRegex(
            ANCHOR,
            r"#define\s+ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES\b",
            "anchor and gateway cannot maintain independent retry horizons",
        )
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES",
            ANCHOR,
        )

        window = function_body(
            ANCHOR, "gateway_discovery_assignment_window_ms_locked"
        )
        stage = window.index("GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS")
        table_window = window.index(
            "discovery_assignment_table_collection_window_for_topology_ms(",
            stage,
        )
        claim_return = window.index(
            "gateway_command_budget_weighted_window_ms(", table_window
        )
        table_return = window.index(
            "gateway_command_budget_window_ms(", table_window
        )
        self.assertLess(stage, table_window)
        self.assertLess(table_window, claim_return)
        self.assertLess(table_window, table_return)

        required_budget = re.search(
            r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS"
            r"\(response_spread_ms\)\s*\\(?P<body>.*?)"
            r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS",
            DISCOVERY_ASSIGNMENT_HEADER,
            re.DOTALL,
        )
        self.assertIsNotNone(required_budget)
        for required in (
            "DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES",
            "DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS",
        ):
            with self.subTest(required_budget_term=required):
                self.assertIn(required, required_budget.group("body"))

    def test_claim_handler_exposes_owned_tristate_contract(self):
        self.assertRegex(
            ANCHOR_HEADER,
            r"\bint\s+gateway_discovery_assignment_note_claim\s*\(",
        )
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")

        parse = claim.index("discovery_assignment_parse_result_tlvs")
        wrong_command = claim.index(
            "ret == PROTO_ERR_NOT_FOUND", parse
        )
        malformed = claim.index("ret != PROTO_OK", wrong_command)
        decoded = claim.index("phase = assignment_result.phase", malformed)
        lock = claim.index(
            "k_mutex_lock(&gateway_discovery_assignment_mutex", decoded
        )
        inactive = claim.index(
            "!gateway_discovery_assignment_state.active", lock
        )

        self.assertLess(parse, wrong_command)
        self.assertLess(wrong_command, malformed)
        self.assertLess(malformed, decoded)
        self.assertLess(decoded, lock)
        self.assertLess(lock, inactive)
        self.assertIn("return -ENOENT;", claim[:parse])
        self.assertIn("return -ENOENT;", claim[wrong_command:malformed])
        self.assertIn("return -EBADMSG;", claim[malformed:decoded])
        self.assertTrue(has_owned_return(
            claim[inactive:],
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
        ))
        self.assertIn("k_mutex_unlock(&gateway_discovery_assignment_mutex)", claim)

    def test_malformed_replies_fail_before_retired_results_are_accepted(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        decode = claim.index("discovery_assignment_parse_result_tlvs")
        lock = claim.index(
            "k_mutex_lock(&gateway_discovery_assignment_mutex", decode
        )
        validation = claim[decode:lock]
        parser = function_body(
            DISCOVERY_ASSIGNMENT,
            "discovery_assignment_parse_result_tlvs",
        )

        for required in (
            "tlv_find_unique",
            "TLV_COMMAND_ID",
            "CMD_ASSIGN_DISCOVERY_SLOTS",
            "TLV_COMMAND_STATUS",
            "COMMAND_OK",
            "TLV_REASON",
            "DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            "DISCOVERY_ASSIGNMENT_PHASE_ACK",
            "discovery_assignment_extract_claim_hash",
            "TLV_HOP_COUNT",
            "TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT",
        ):
            with self.subTest(required=required):
                self.assertIn(required, parser)
        for required in (
            "discovery_assignment_parse_result_tlvs",
            "discovery_assignment_hash(packet->src_id)",
            "packet->src_id == 0u",
            "assignment_result.phase",
            "assignment_result.epoch",
            "assignment_result.hash",
            "assignment_result.table_commitment",
            "assignment_result.hop_count",
        ):
            with self.subTest(validation_required=required):
                self.assertIn(required, validation)
        self.assertTrue(has_owned_return(validation, "-EBADMSG"))

        inactive = claim.index(
            "!gateway_discovery_assignment_state.active", lock
        )
        wrong_epoch = claim.index(
            "epoch != gateway_discovery_assignment_state.epoch", inactive
        )
        deadline = claim.index(
            "!uptime_deadline_reached(", wrong_epoch
        )
        lookup = claim.index("for (size_t i = 0u", deadline)
        inactive_branch = claim[inactive:wrong_epoch]
        epoch_branch = claim[wrong_epoch:deadline]
        deadline_branch = claim[deadline:lookup]
        for branch in (inactive_branch, epoch_branch):
            self.assertIn(
                "gateway_discovery_assignment_ack_proof_admission(", branch
            )
            self.assertTrue(has_owned_return(
                branch, "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE"
            ))
        self.assertIn(
            "gateway_discovery_assignment_ack_proof_admission(",
            deadline_branch,
        )
        self.assertIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", deadline_branch
        )
        self.assertIn(
            "gateway_discovery_assignment_state\n"
            "                .operation_deadline_ms",
            deadline_branch,
        )
        self.assertNotIn("APP_GATEWAY_SEMANTIC_ACCEPT", validation)

    def test_every_assignment_state_gate_is_visible_in_hardware_traces(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")

        self.assertIn("DBG_ENUM_REJECT reason=envelope", claim)
        self.assertIn("DBG_ENUM_REJECT phase=%u reason=rf", claim)
        for reason in (
            "reason=inactive",
            "reason=epoch",
            "reason=deadline",
            "reason=state",
        ):
            with self.subTest(retired_reason=reason):
                self.assertIn(
                    f"DBG_ENUM_RETIRE phase=%u {reason}",
                    claim,
                )

    def test_valid_and_duplicate_claims_and_table_acks_are_distinguished(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        capacity_start = claim.index(
            "gateway_discovery_assignment_state.claim_count >=",
        )
        lookup = claim.index("for (size_t i = 0u")
        validation_start = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)", lookup
        )
        reserve = claim.index(
            "mesh_relay_reserve_gateway_ack_candidate(", capacity_start
        )
        ack_start = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)", reserve
        )
        duplicate_start = claim.index("if (anchor_index != SIZE_MAX)", ack_start)
        insertion_start = claim.index(
            "gateway_discovery_assignment_state.anchor_ids[",
            duplicate_start,
        )
        ack = claim[ack_start:duplicate_start]
        validation = claim[validation_start:capacity_start]
        duplicate = claim[duplicate_start:insertion_start]
        capacity_and_insert = claim[capacity_start:]

        for required in (
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS",
            "table_command_seq",
            "table_commitment",
            "anchor_index == SIZE_MAX",
        ):
            with self.subTest(ack_validation_required=required):
                self.assertIn(required, validation)
        for required in (
            "ack_mask |=",
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW",
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
        ):
            with self.subTest(ack_required=required):
                self.assertIn(required, ack)
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", ack)
        first_ack = ack.index("ack_mask |=")
        new_accept = ack.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW", first_ack
        )
        self.assertLess(first_ack, new_accept)
        durable_gate = ack.index(
            "gateway_registered_membership_proves_assignment_ack("
        )
        retry = ack.index("GATEWAY_ASSIGNMENT_RETURN(-EAGAIN)", durable_gate)
        self.assertLess(durable_gate, retry)
        self.assertLess(retry, first_ack)
        self.assertIn(
            "discovery_assignment_ack_quorum_settle_should_arm(",
            ack,
        )
        self.assertIn("response_ack_settle_armed", ack)
        self.assertIn("duplicate_count", duplicate)
        self.assertTrue(has_owned_return(
            duplicate, "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE"
        ))
        self.assertTrue(has_owned_return(capacity_and_insert, "-ENOSPC"))
        self.assertIn("claim_count++", capacity_and_insert)
        self.assertTrue(has_owned_return(
            capacity_and_insert, "APP_GATEWAY_SEMANTIC_ACCEPT_NEW"
        ))

    def test_clean_slate_does_not_seed_prior_roster_or_hop_projection(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        reset = start.index(
            "memset(&gateway_discovery_assignment_state"
        )
        activate = start.index(
            "gateway_discovery_assignment_state.active = true", reset
        )
        live_start = start[reset:activate]

        self.assertNotIn("prior_anchor_count", start)
        self.assertNotIn("memcpy(gateway_discovery_assignment_state.anchor_", live_start)
        self.assertIn("gateway_clear_registered_membership_roster()", start)
        self.assertIn("&mesh_runtime, NULL, 0u", start)

        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        decoded = claim.index(
            "hop_count = assignment_result.hop_count_present"
        )
        valid_depth = claim.index(
            "if (hop_count != 0u && "
            "hop_count <= DISCOVERY_ASSIGNMENT_MAX_HOPS)",
            decoded,
        )
        observed = claim.index("observed_hop_count = hop_count", valid_depth)
        roster_lookup = claim.index("for (size_t i = 0u", observed)
        ack_validation = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)", roster_lookup
        )
        reserve = claim.index(
            "mesh_relay_reserve_gateway_ack_candidate(", ack_validation
        )
        ack_start = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)", reserve
        )
        ack_end = claim.index("if (anchor_index != SIZE_MAX)", ack_start)
        ack = claim[ack_start:ack_end]
        duplicate_check = ack.index("ack_already_recorded =")
        nonzero_guard = ack.index(
            "if (!ack_already_recorded && observed_hop_count != 0u)",
            duplicate_check,
        )
        projection = ack.index(
            "gateway_discovery_assignment_state.anchor_hop_counts[",
            nonzero_guard,
        )
        projection_value = ack.index(
            "anchor_index] = observed_hop_count", projection
        )
        ack_commit = ack.index("ack_mask |=", projection_value)

        self.assertLess(decoded, valid_depth)
        self.assertLess(valid_depth, observed)
        self.assertLess(observed, roster_lookup)
        self.assertLess(ack_validation, reserve)
        self.assertLess(reserve, ack_start)
        self.assertLess(duplicate_check, nonzero_guard)
        self.assertLess(nonzero_guard, projection)
        self.assertLess(projection_value, ack_commit)
        self.assertNotIn(
            "anchor_hop_counts[\n                anchor_index] = hop_count",
            ack,
            "zero or out-of-range wire values cannot become route evidence",
        )

        prepare_table = function_body(
            ANCHOR,
            "gateway_discovery_assignment_prepare_durable_table_locked",
        )
        prepare = prepare_table.index(
            "app_gateway_assignment_publisher_prepare_table("
        )
        persist = prepare_table.index(
            "gateway_set_registered_membership_roster(", prepare
        )
        publisher_call = prepare_table[prepare:persist]
        self.assertIn(
            "gateway_discovery_assignment_state.anchor_hop_counts",
            publisher_call,
        )

    def test_expected_count_uses_current_operation_claim_responders(self):
        self.assertIsNotNone(
            re.search(r"\buint64_t\s+claim_response_mask\s*;", ANCHOR),
            "assignment state is missing current-operation CLAIM responders",
        )
        current_count = function_body(
            ANCHOR,
            "gateway_discovery_assignment_current_claim_count_locked",
        )
        self.assertIn("claim_response_mask", current_count)
        self.assertIn("__builtin_popcountll", current_count)

        complete = function_body(
            ANCHOR,
            "gateway_discovery_assignment_expected_claims_complete_locked",
        )
        self.assertIn(
            "gateway_discovery_assignment_current_claim_count_locked()",
            complete,
        )
        self.assertNotRegex(
            complete,
            r"\bclaim_count\s*>=\s*"
            r"gateway_discovery_assignment_state\.expected_claim_count",
        )

        service = function_body(
            ANCHOR,
            "gateway_discovery_assignment_service_delivery",
        )
        self.assertIn(
            "gateway_discovery_assignment_expected_claims_complete_locked()",
            service,
        )
        self.assertNotIn(
            "app_discovery_assignment_semantic_terminal_success(",
            service,
            "a failed bounded RF delivery must not be rewritten as success",
        )

        window = function_body(
            ANCHOR,
            "gateway_discovery_assignment_window_ms_locked",
        )
        hop_start = window.index(
            "app_discovery_assignment_collection_hop_count("
        )
        hop_end = window.index(");", hop_start)
        self.assertIn(
            "gateway_discovery_assignment_current_claim_count_locked()",
            window[hop_start:hop_end],
        )

        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        duplicate_start = claim.index("if (anchor_index != SIZE_MAX)")
        insertion_start = claim.index(
            "gateway_discovery_assignment_state.anchor_ids[",
            duplicate_start,
        )
        new_accept = claim.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW",
            insertion_start,
        )
        self.assertIn(
            "claim_response_mask",
            claim[duplicate_start:insertion_start],
            "a prior-roster CLAIM did not count as a current response",
        )
        self.assertIn(
            "claim_response_mask",
            claim[insertion_start:new_accept],
            "a newly admitted CLAIM did not count as a current response",
        )

    def test_table_correlated_late_claim_fails_without_gateway_redrive(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        publish = function_body(
            ANCHOR, "gateway_discovery_assignment_publish_work_handler"
        )
        build = function_body(
            ANCHOR, "gateway_build_discovery_assignment_command"
        )
        late = claim.index("late_table_recovery_claim =")
        redrive = claim.index("late-claim-table-redrive", late)
        held = claim.index("GATEWAY_ASSIGNMENT_RETURN(-EAGAIN)", redrive)

        correlated = claim[late:redrive]
        for required in (
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS",
            "gateway_discovery_assignment_rf_started_locked(",
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE",
            "table_command_seq != 0u",
            "packet->session_id ==",
            "anchor_index != SIZE_MAX",
            "ack_mask &",
        ):
            with self.subTest(required=required):
                self.assertIn(required, correlated)
        self.assertLess(redrive, held)
        self.assertNotIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
            claim[redrive:held],
        )

        missing = publish.index(
            "gateway_discovery_assignment_missing_ack_count_locked() =="
        )
        pending = publish.index("late_table_redrive_pending", missing)
        fail = publish.index(
            "gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT", pending
        )
        normal_limit = publish.index(
            "gateway_discovery_assignment_state.table_round >=", fail
        )
        self.assertLess(missing, pending)
        self.assertLess(pending, fail)
        self.assertLess(fail, normal_limit)
        self.assertNotIn(
            "gateway_discovery_assignment_publish_table()",
            publish[pending:normal_limit],
        )

        finalize = function_body(
            ANCHOR, "gateway_discovery_assignment_finalize_work_handler"
        )
        urgent = finalize.index(
            "gateway_discovery_assignment_state.late_table_redrive_pending"
        )
        response_wait = finalize.index(
            "response-window-deadline", urgent
        )
        submit = finalize.index(
            "mesh_gateway_command_priority_submit(", urgent
        )
        self.assertLess(urgent, response_wait)
        self.assertLess(response_wait, submit)
        self.assertIn(
            "gateway_discovery_assignment_state.round_open = false",
            finalize[urgent:response_wait],
        )

        self.assertRegex(
            ANCHOR,
            r"gateway_build_discovery_assignment_command\s*\("
            r"[^)]*\buint16_t\s+packet_seq\s*\)",
        )
        self.assertIn("outbound->packet.seq = packet_seq", build)
        self.assertNotIn(
            "outbound->packet.seq = gateway_next_command_seq()", build
        )
        self.assertIn("flood_attempt_id = gateway_next_broadcast_command_seq()", build)
        self.assertIn("TLV_FLOOD_EPOCH_ID", build)
        self.assertIn("flood_attempt_id", build)

        table_publish = function_body(
            ANCHOR, "gateway_discovery_assignment_publish_table"
        )
        retry_branch = table_publish.index(
            "gateway_discovery_assignment_state.table_round <"
        )
        retry_schedule = table_publish.index("table-admission-retry", retry_branch)
        self.assertLess(retry_branch, retry_schedule)

        submit = function_body(
            ANCHOR, "gateway_discovery_assignment_submit_control_flood_locked"
        )
        self.assertIn(
            "NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN", submit
        )
        self.assertIn(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM", submit
        )
        table_limit = function_body(
            ANCHOR, "gateway_discovery_assignment_table_round_limit_locked"
        )
        self.assertIn("DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS", table_limit)
        self.assertNotIn("+ 1u", table_limit)

        service = function_body(
            ANCHOR, "gateway_discovery_assignment_service_delivery"
        )
        table_delivery = service.index(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE"
        )
        round_update = service.index(
            "app_discovery_assignment_table_round_after_delivery(",
            table_delivery,
        )
        self.assertIn(
            "table_delivery_is_redrive",
            service[table_delivery:round_update + 200],
        )
        self.assertNotIn(
            "table_round++",
            service[table_delivery:round_update + 200],
        )

    def test_assignment_host_command_retains_async_result_reservation(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        worker = function_body(ANCHOR, "gateway_host_command_work_handler")

        for required in (
            "host_options.scope != CMD_SCOPE_SINGLE_NODE",
            "host_options.response_mode != CMD_RESPONSE_SMALL_RESULT",
            "host_options.flood_required",
            "host_options.collection_required",
        ):
            self.assertIn(required, start)
        self.assertIn(
            "item.command_id != CMD_ASSIGN_DISCOVERY_SLOTS",
            worker,
        )
        self.assertRegex(ANCHOR, r"\buint16_t\s+table_packet_seq\s*;")
        self.assertNotIn("table_" + "fingerprint", ANCHOR)

    def test_durable_assignment_proof_reconciles_gateway_ack_membership(self):
        proof = function_body(
            ANCHOR,
            "gateway_discovery_assignment_ack_proof_admission",
        )
        durable = proof.index(
            "gateway_registered_membership_proves_assignment_ack("
        )
        accepted = proof.index("if (proof_ret == 1)", durable)
        roster = proof.index(
            "gateway_get_registered_membership_roster_with_slots(",
            accepted,
        )
        reconcile = proof.index(
            "mesh_relay_reconcile_gateway_ack_membership(",
            roster,
        )
        terminal = proof.index(
            "return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;",
            reconcile,
        )

        self.assertLess(durable, accepted)
        self.assertLess(accepted, roster)
        self.assertLess(roster, reconcile)
        self.assertLess(reconcile, terminal)
        self.assertIn(
            "uint64_t member_ids[MESH_CONNECTED_MAX_ANCHORS]",
            proof,
        )
        self.assertIn("if (ret != 0)", proof[roster:reconcile])
        self.assertIn("if (ret != PROTO_OK)", proof[reconcile:terminal])

        projection = function_body(
            GATEWAY,
            "gateway_registered_membership_proves_assignment_ack",
        )
        for durable_guard in (
            "gateway_membership_durable_receipt_valid",
            "gateway_membership_snapshot_state.assignment_proof_valid",
            "gateway_membership_snapshot_state.assignment_epoch",
            "gateway_membership_snapshot_state.assignment_table_seq",
            "gateway_membership_snapshot_state.assignment_table_commitment",
            "gateway_membership_snapshot_state.node_ids[slot] == node_id",
        ):
            self.assertIn(durable_guard, projection)
        member = projection.index(
            "gateway_membership_snapshot_state.node_ids[slot] == node_id"
        )
        member_reject = projection.index("if (!current_member)", member)
        exact = projection.index(
            "gateway_membership_snapshot_state.assignment_epoch ==",
            member_reject,
        )
        superseded = projection.index(
            "discovery_assignment_epoch_strictly_newer(", exact
        )
        self.assertLess(member, member_reject)
        self.assertLess(member_reject, exact)
        self.assertLess(exact, superseded)
        self.assertIn(
            "gateway_membership_snapshot_state.assignment_epoch,\n"
            "               assignment_epoch",
            projection[superseded:],
        )
        self.assertNotIn("gateway_membership_snapshot_state =", projection)
        self.assertNotIn("gateway_membership_roster_state =", projection)
        self.assertNotIn("app_durable_state_", projection)

    def test_table_is_durable_before_rf_and_before_ack_acceptance(self):
        prepare = function_body(
            ANCHOR,
            "gateway_discovery_assignment_prepare_durable_table_locked",
        )
        publisher = prepare.index(
            "app_gateway_assignment_publisher_prepare_table("
        )
        durable = prepare.index(
            "gateway_set_registered_membership_roster(", publisher
        )
        marker = prepare.index(
            "GATEWAY_MEMBERSHIP_TABLE_CONFIRM_PENDING", publisher
        )
        self.assertLess(publisher, marker)
        self.assertLess(marker, durable)
        self.assertIn(
            "publication.acknowledged_mask = responder_slot_mask",
            prepare,
        )

        publish = function_body(
            ANCHOR, "gateway_discovery_assignment_publish_table"
        )
        durable_prepare = publish.index(
            "gateway_discovery_assignment_prepare_durable_table_locked()"
        )
        rf_submit = publish.index(
            "gateway_discovery_assignment_submit_control_flood_locked(",
            durable_prepare,
        )
        self.assertLess(durable_prepare, rf_submit)

        claim = function_body(
            ANCHOR, "gateway_discovery_assignment_note_claim"
        )
        ack_branch = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)"
        )
        proof_gate = claim.index(
            "gateway_registered_membership_proves_assignment_ack(",
            ack_branch,
        )
        ack_mutation = claim.index(
            "gateway_discovery_assignment_state.ack_mask |=", proof_gate
        )
        accepted = claim.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW", ack_mutation
        )
        self.assertLess(proof_gate, ack_mutation)
        self.assertLess(ack_mutation, accepted)

        replay = function_body(
            GATEWAY, "gateway_replay_pending_assignment_publication"
        )
        pending = replay.index(
            "GATEWAY_MEMBERSHIP_TABLE_CONFIRM_PENDING"
        )
        rf_resume = replay.index(
            "app_anchor_gateway_assignment_resume_pending_table(", pending
        )
        host_prepare = replay.index(
            "app_gateway_assignment_publisher_prepare_table(", rf_resume
        )
        self.assertLess(pending, rf_resume)
        self.assertLess(rf_resume, host_prepare)

    def test_confirm_pending_replay_restores_valid_table_policy_before_retry(self):
        resume = function_body(
            ANCHOR, "app_anchor_gateway_assignment_resume_pending_table"
        )
        reset = resume.index("memset(&gateway_discovery_assignment_state,")
        spread = resume.index(
            "gateway_discovery_assignment_state.response_spread_ms =",
            reset,
        )
        default_budget = resume.index(
            "gateway_discovery_assignment_state.command_budget_ms =",
            spread,
        )
        retry = resume.index(
            "gateway_discovery_assignment_reschedule(", default_budget
        )

        self.assertIn(
            "DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS",
            resume[spread:default_budget],
        )
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS",
            resume[default_budget:retry],
        )
        self.assertLess(reset, spread)
        self.assertLess(spread, default_budget)
        self.assertLess(default_budget, retry)

        command = function_body(
            ANCHOR, "gateway_build_discovery_assignment_command"
        )
        self.assertIn(
            ".response_spread_ms =\n"
            "                    gateway_discovery_assignment_state.response_spread_ms",
            command,
        )

    def test_table_entry_scratch_is_not_live_during_delivery(self):
        self.assertIn(
            "static __attribute__((noinline)) int\n"
            "gateway_discovery_assignment_build_table_locked",
            ANCHOR,
        )
        builder = function_body(
            ANCHOR,
            "gateway_discovery_assignment_build_table_locked",
        )
        publish = function_body(
            ANCHOR,
            "gateway_discovery_assignment_publish_table",
        )

        self.assertNotIn("struct discovery_assignment_entry entries", builder)
        self.assertIn(
            "discovery_assignment_append_table_from_roster(", builder
        )
        self.assertIn(
            "discovery_assignment_table_commitment_from_roster(", builder
        )
        self.assertNotIn(
            "gateway_discovery_assignment_submit_control_flood_locked(",
            builder,
        )
        self.assertNotIn("struct discovery_assignment_entry entries", publish)
        materialize = publish.index(
            "gateway_discovery_assignment_build_table_locked(&outbound)"
        )
        durable = publish.index(
            "gateway_discovery_assignment_prepare_durable_table_locked()",
            materialize,
        )
        delivery = publish.index(
            "gateway_discovery_assignment_submit_control_flood_locked(",
            durable,
        )
        self.assertLess(materialize, durable)
        self.assertLess(durable, delivery)

    def test_candidate_ack_capacity_is_reserved_after_validation_before_mutation(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        state_gate = claim.index(
            "gateway_discovery_assignment_state.stage !="
        )
        rf_gate = claim.index(
            "gateway_discovery_assignment_rf_started_locked(", state_gate
        )
        deadline_gate = claim.index(
            "response_window_deadline_valid", rf_gate
        )
        capacity_gate = claim.index(
            "gateway_discovery_assignment_state.claim_count >=", deadline_gate
        )
        reserve = claim.index(
            "mesh_relay_reserve_gateway_ack_candidate(", capacity_gate
        )
        hop_mutation = claim.index(
            "gateway_discovery_assignment_state.max_hop_count =", reserve
        )
        ack_mutation = claim.index(
            "gateway_discovery_assignment_state.ack_mask |=", reserve
        )
        roster_mutation = claim.index(
            "gateway_discovery_assignment_state.anchor_ids[", ack_mutation
        )

        self.assertLess(state_gate, rf_gate)
        self.assertLess(rf_gate, deadline_gate)
        self.assertLess(deadline_gate, capacity_gate)
        self.assertLess(capacity_gate, reserve)
        self.assertLess(reserve, hop_mutation)
        self.assertLess(reserve, ack_mutation)
        self.assertLess(reserve, roster_mutation)
        reserve_guard_start = claim.rfind("if (", capacity_gate, reserve)
        reserve_guard = claim[reserve_guard_start:reserve]
        self.assertNotIn("anchor_index == SIZE_MAX", reserve_guard)
        self.assertNotIn("prior_anchor_count", reserve_guard)
        self.assertIn(
            "GATEWAY_ASSIGNMENT_RETURN(mesh_errno_from_proto(ret))",
            claim[reserve:hop_mutation],
        )

    def test_active_table_ack_commit_is_terminal_before_full_quorum(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        capacity = claim.index(
            "gateway_discovery_assignment_state.claim_count >="
        )
        reserve = claim.index(
            "mesh_relay_reserve_gateway_ack_candidate(", capacity
        )
        ack_start = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)", reserve
        )
        ack_end = claim.index("if (anchor_index != SIZE_MAX)", ack_start)
        ack = claim[ack_start:ack_end]
        mutation = ack.index("ack_mask |=")
        duplicate = ack.index("ack_mask &")
        new_accept = ack.index("APP_GATEWAY_SEMANTIC_ACCEPT_NEW", mutation)

        self.assertLess(reserve, ack_start)
        self.assertLess(duplicate, mutation)
        self.assertLess(mutation, new_accept)
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", ack)
        self.assertNotIn("missing_ack_count", ack[new_accept:])

        drain = function_body(REPORT, "mesh_drain_rx_queue_locked")
        semantic = drain.index("mesh_gateway_accept_semantic_delivery")
        rejected = drain.index("if (semantic_ret < 0)", semantic)
        commit = drain.index("mesh_relay_commit_gateway_delivery", rejected)
        actions = drain.index("mesh_handle_result_actions", commit)
        self.assertLess(semantic, rejected)
        self.assertLess(rejected, commit)
        self.assertLess(commit, actions)

    def test_transport_preflight_cannot_bind_an_assignment_candidate(self):
        preflight = function_body(RELAY, "gateway_ack_history_can_accept")
        assignment = preflight.index("CMD_ASSIGN_DISCOVERY_SLOTS")
        assignment_branch = preflight[assignment:]

        self.assertNotIn(
            "mesh_relay_reserve_gateway_ack_candidate(",
            preflight,
        )
        self.assertNotIn(
            "GATEWAY_ACK_HISTORY_ASSIGNMENT_CANDIDATE",
            preflight,
        )
        self.assertIn("return PROTO_OK;", assignment_branch)

    def test_new_enumeration_reconciles_only_after_prior_owners_are_terminal(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        publication = start.index(
            "gateway_assignment_publication_pending()"
        )
        diagnostics = start.index(
            "app_gateway_assignment_publisher_get_diagnostics(", publication
        )
        inactive = start.index(
            "gateway_discovery_assignment_state.active", diagnostics
        )
        publisher = start.index("publisher_diag.active", inactive)
        operation_claim = start.index(
            "gateway_operation_owner_claim(", publisher
        )
        roster = start.index(
            "gateway_get_registered_membership_roster_with_slots(",
            operation_claim,
        )
        reconcile = start.index(
            "mesh_relay_reconcile_gateway_ack_membership(", roster
        )
        claim_identity = start.index(
            "claim_command_seq = gateway_next_broadcast_command_seq()",
            reconcile,
        )
        zero_guard = start.index(
            "if (claim_command_seq == 0u)", claim_identity
        )
        assignment_epoch = start.index(
            "reserved_epoch = claim_command_seq", zero_guard
        )
        policy_commit = start.index(
            "app_operation_policy_commit_prepared(&policy_candidate)",
            assignment_epoch,
        )
        epoch_publish = start.index(
            "gateway_discovery_assignment_state.epoch = reserved_epoch",
            policy_commit,
        )
        claim_publish = start.index(
            "gateway_discovery_assignment_state.claim_command_seq =",
            epoch_publish,
        )
        activate = start.index(
            "gateway_discovery_assignment_state.active = true",
            claim_publish,
        )

        self.assertLess(publication, diagnostics)
        self.assertLess(diagnostics, inactive)
        self.assertLess(inactive, publisher)
        self.assertLess(publisher, operation_claim)
        self.assertLess(operation_claim, roster)
        self.assertLess(roster, reconcile)
        self.assertLess(reconcile, claim_identity)
        self.assertLess(claim_identity, zero_guard)
        self.assertLess(zero_guard, assignment_epoch)
        self.assertLess(claim_identity, assignment_epoch)
        self.assertLess(assignment_epoch, policy_commit)
        self.assertLess(policy_commit, epoch_publish)
        self.assertLess(epoch_publish, claim_publish)
        self.assertLess(claim_publish, activate)

    def test_active_enumeration_isolates_only_nonassignment_sources(self):
        admission = function_body(
            ANCHOR,
            "gateway_discovery_assignment_admit_nonassignment_source",
        )
        semantic = function_body(
            REPORT,
            "mesh_gateway_accept_semantic_delivery",
        )
        internal = semantic.index(
            "mesh_gateway_delivery_is_internal_control("
        )
        admission_call = semantic.index(
            "gateway_discovery_assignment_admit_nonassignment_source(",
            internal,
        )
        dispatch = semantic.index("switch (pending->packet.msg_type)")

        self.assertIn(
            "!gateway_discovery_assignment_state.active",
            admission,
        )
        self.assertIn(
            "!gateway_discovery_assignment_state.active",
            admission,
        )
        self.assertNotIn("prior_anchor_count", admission)
        self.assertLess(internal, admission_call)
        self.assertLess(admission_call, dispatch)
        self.assertIn("return -EAGAIN;", semantic[admission_call:dispatch])

    def test_assignment_control_preflights_stale_result_before_ack(self):
        delivery = function_body(
            REPORT,
            "mesh_drain_rx_queue_locked",
        )
        preflight = delivery.index(
            "semantic_ret = mesh_gateway_preflight_semantic_delivery(pending)"
        )
        internal = delivery.index("if (internal_control) {")
        ordinary = delivery.index("} else {", internal)
        internal_block = delivery[internal:ordinary]
        finalize = delivery.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW ?",
            ordinary,
        )
        relay_commit = delivery.index(
            "mesh_relay_commit_gateway_delivery(",
            finalize,
        )

        self.assertLess(preflight, internal)
        self.assertIn("host_custody_ready = true;", internal_block)
        self.assertIn(
            "semantic_preflight_complete = true;",
            delivery[preflight:internal],
        )
        self.assertLess(internal, finalize)
        self.assertLess(finalize, relay_commit)

    def test_each_run_clears_the_active_roster_before_claim(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        prepare = function_body(
            ANCHOR,
            "gateway_discovery_assignment_prepare_durable_table_locked",
        )
        clear = start.index("gateway_clear_registered_membership_roster()")
        verify = start.index(
            "gateway_get_registered_membership_roster_with_slots(", clear
        )
        claim = start.index(
            "claim_command_seq = gateway_next_broadcast_command_seq()", verify
        )
        self.assertLess(clear, verify)
        self.assertLess(verify, claim)
        self.assertNotIn("prior_anchor_count", start)
        self.assertIn("published_slot_mask |=", prepare)
        self.assertNotIn("gateway_discovery_assignment_state.ack_mask", prepare)
        self.assertNotIn("!acknowledged", prepare)

    def test_table_adaptive_wait_covers_one_lost_response_retry(self):
        control_prefix = ANCHOR[
            ANCHOR.index(
                "#define GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS"
            ) : ANCHOR.index(
                "/* Exact active delivery whose physical RF-start edge",
            )
        ]
        adaptive = function_body(
            ANCHOR,
            "gateway_discovery_assignment_adaptive_deadline_offset_ms_locked",
        )

        self.assertRegex(
            DISCOVERY_ASSIGNMENT_HEADER,
            r"#define\s+DISCOVERY_ASSIGNMENT_ADAPTIVE_RX_MARGIN_MS\s+850u\b",
        )

        self.assertIn("ROUTE_GATEWAY_ACK_TIMEOUT_MS", control_prefix)
        self.assertIn("ROUTE_RETRY_BACKOFF_FIRST_MS", control_prefix)
        self.assertIn(
            "(ROUTE_RETRY_BACKOFF_FIRST_MS / 2u)", control_prefix
        )
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS", control_prefix
        )
        self.assertIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS == 4700u",
            control_prefix,
        )

        # The direct, three-slot CLAIM calculation covers its complete
        # absolute depth-one band plus the shared 850 ms retry margin.
        # TABLE needs 4,700 ms for one legal 2,000 + 1,500 + 750 + 450 ms
        # missed-response recovery cycle, without lengthening CLAIM itself.
        claim_adaptive_ms = 365 + (3 * 450) + 100 + 449 + 850
        table_recovery_ms = 2000 + 1500 + 750 + 450
        self.assertEqual(claim_adaptive_ms, 3114)
        self.assertEqual(table_recovery_ms, 4700)
        self.assertLess(claim_adaptive_ms, table_recovery_ms)

        base_adaptive = adaptive.index(
            "discovery_assignment_adaptive_depth_deadline_offset_ms("
        )
        table_gate = adaptive.index(
            "gateway_discovery_assignment_state.stage ==\n"
            "            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS"
        )
        table_floor = adaptive.index(
            "GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS",
            table_gate,
        )
        self.assertLess(base_adaptive, table_gate)
        self.assertNotIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS",
            adaptive[:table_gate],
        )
        self.assertIn(
            "deadline_offset_ms = MAX(", adaptive[table_gate:table_floor]
        )
        self.assertNotIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS",
            adaptive[table_gate:table_floor + 80],
        )

    def test_final_table_ack_arms_short_settle_before_end(self):
        claim = function_body(
            GATEWAY_CONTROL, "gateway_discovery_assignment_note_claim"
        )
        finalize = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_finalize_work_handler",
        )

        ack_branch = claim[claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)",
            claim.index("mesh_relay_reserve_gateway_ack_candidate(")
        ):]
        ack_mask = ack_branch.index(
            "gateway_discovery_assignment_state.ack_mask |="
        )
        quorum = ack_branch.index(
            "discovery_assignment_ack_quorum_settle_should_arm(", ack_mask
        )
        arm = ack_branch.index(
            "response_ack_settle_armed = true", quorum
        )
        deadline = ack_branch.index(
            "discovery_assignment_response_ack_settle_deadline_ms(", arm
        )
        wake = ack_branch.index('"proof-validated-response"', deadline)
        self.assertLess(ack_mask, quorum)
        self.assertLess(quorum, arm)
        self.assertLess(arm, deadline)
        self.assertLess(deadline, wake)
        self.assertIn(
            "#define DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS 450u",
            DISCOVERY_ASSIGNMENT_HEADER,
        )
        self.assertNotIn(
            "gateway_discovery_assignment_note_ack_confirm",
            GATEWAY_CONTROL,
        )
        self.assertNotIn("POST_CONFIRM_TAIL", GATEWAY_CONTROL)

        quorum_complete = finalize.index("missing_proof_count == 0u")
        settle_pending = finalize.index(
            "discovery_assignment_response_ack_settle_pending(",
            quorum_complete,
        )
        settle_reschedule = finalize.index(
            '"response-ack-settle"', settle_pending
        )
        publish_end = finalize.index(
            "gateway_discovery_assignment_publish_end()", settle_reschedule
        )
        self.assertLess(quorum_complete, settle_pending)
        self.assertLess(settle_pending, settle_reschedule)
        self.assertLess(settle_reschedule, publish_end)

    def test_assignment_result_history_is_confirmed_at_gateway_commit(self):
        commit = function_body(
            RELAY_CUSTODY, "mesh_relay_commit_gateway_delivery"
        )
        store = commit.index("gateway_ack_history_store(")
        parse = commit.index(
            "discovery_assignment_parse_result_tlvs(", store
        )
        claim_phase = commit.index(
            "assignment.phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM", parse
        )
        ack_phase = commit.index(
            "assignment.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK", claim_phase
        )
        confirm = commit.index(
            "gateway_ack_history_confirm_packet(", ack_phase
        )

        self.assertLess(store, parse)
        self.assertLess(parse, claim_phase)
        self.assertLess(claim_phase, ack_phase)
        self.assertLess(ack_phase, confirm)

    def test_gateway_ch9_continuous_rx_traces_each_physical_attempt(self):
        arm = REPORT.index("DBG_GATEWAY_CH9_RX_CONT_ARM")
        receive = REPORT.index(
            "dwm3000_driver_receive_frame_continuous(", arm
        )
        result = REPORT.index("DBG_GATEWAY_CH9_RX_CONT_RESULT", receive)

        self.assertLess(arm, receive)
        self.assertLess(receive, result)
        self.assertIn(
            "IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)",
            REPORT[arm - 200:arm],
        )
        self.assertIn(
            "IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)",
            REPORT[receive:result],
        )

    def test_gateway_ch9_rx_slice_and_rearm_gap_are_bounded(self):
        self.assertIn(
            "#define APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS 500u",
            RX_POLICY_HEADER,
        )
        self.assertIn(
            "#define APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS 1u",
            RX_POLICY_HEADER,
        )
        self.assertIn(
            "APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS",
            function_body(
                RX_POLICY, "app_mesh_rx_policy_gateway_ch9_work_slice_ms"
            ),
        )
        self.assertIn(
            "return APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS;",
            function_body(
                RX_POLICY, "app_mesh_rx_policy_gateway_ch9_rearm_delay_ms"
            ),
        )

    def test_gateway_ch9_rx_parks_by_receive_outcome(self):
        handler = function_body(REPORT, "mesh_uwb_rx_work_handler")
        gateway_start = handler.index("if (mesh_gateway_route_test_role())")
        gateway_end = handler.index("gateway_route_preempt =", gateway_start)
        gateway = handler[gateway_start:gateway_end]

        receive = gateway.index("dwm3000_driver_receive_frame_continuous(")
        command_preempt = gateway.index(
            "} else if (ret == -ECANCELED)", receive
        )
        empty_slice = gateway.index(
            "} else if (ret == -ETIMEDOUT)", command_preempt
        )
        recoverable = gateway.index(
            "} else if (app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(",
            empty_slice,
        )
        hard_failure = gateway.index("} else {", recoverable)
        release = gateway.index(
            "mesh_rx_radio_finish(&radio_lease", hard_failure
        )

        self.assertIn(
            'mesh_radio_idle_with_bounded_recovery(\n'
            '                    "gateway-ch9-command-preempt")',
            gateway[command_preempt:empty_slice],
        )
        self.assertIn(
            'mesh_radio_idle_with_bounded_recovery(\n'
            '                    "gateway-ch9-empty-slice")',
            gateway[empty_slice:recoverable],
        )
        self.assertNotIn(
            "mesh_radio_standby_with_bounded_recovery",
            gateway[empty_slice:recoverable],
        )
        self.assertIn(
            'mesh_radio_standby_with_bounded_recovery(\n'
            '                    "gateway-ch9-hard-rx")',
            gateway[hard_failure:release],
        )
        abort_handoff = gateway.index(
            "app_node_comm_gateway_delivery_safe_boundary()", release
        )
        self.assertLess(command_preempt, release)
        self.assertLess(release, abort_handoff)

    def test_clearing_deferred_rx_rearm_invalidates_already_taken_work(self):
        take = function_body(REPORT, "mesh_take_deferred_uwb_rx_rearm")
        clear = function_body(REPORT, "mesh_clear_deferred_uwb_rx_rearm")
        worker = function_body(REPORT, "mesh_uwb_rx_rearm_work_handler")

        take_generation = take.index("*generation = mesh_uwb_rx_rearm_generation")
        take_pending = take.index("mesh_uwb_rx_rearm_pending = false")
        self.assertLess(take_generation, take_pending)

        clear_lock = clear.index("k_mutex_lock(&mesh_uwb_rx_rearm_lock")
        invalidate = clear.index("mesh_uwb_rx_rearm_generation++", clear_lock)
        clear_pending = clear.index("mesh_uwb_rx_rearm_pending = false", invalidate)
        clear_unlock = clear.index("k_mutex_unlock(&mesh_uwb_rx_rearm_lock")
        cancel = clear.index("k_work_cancel(&mesh_uwb_rx_rearm_work)", clear_unlock)
        self.assertLess(clear_lock, invalidate)
        self.assertLess(invalidate, clear_pending)
        self.assertLess(clear_pending, clear_unlock)
        self.assertLess(clear_unlock, cancel)

        take_work = worker.index("mesh_take_deferred_uwb_rx_rearm(")
        final_lock = worker.index("k_mutex_lock(&mesh_uwb_rx_rearm_lock", take_work)
        stale_check = worker.index(
            "generation != mesh_uwb_rx_rearm_generation", final_lock
        )
        stale_branch = worker.index("if (stale)", stale_check)
        schedule_else = worker.index("} else {", stale_branch)
        schedule = worker.index(
            "mesh_reschedule_owned_work_with_busy_handoff(", schedule_else
        )
        final_unlock = worker.index(
            "k_mutex_unlock(&mesh_uwb_rx_rearm_lock", schedule
        )
        stale_return = worker.index("if (stale)", final_unlock)
        stale_return_end = worker.index("return;", stale_return)
        self.assertLess(take_work, final_lock)
        self.assertLess(final_lock, stale_check)
        self.assertIn(
            "!mesh_rx_handoff_scan_rearm_allowed()",
            worker[stale_check:stale_branch],
        )
        self.assertLess(stale_check, stale_branch)
        self.assertLess(stale_branch, schedule_else)
        self.assertLess(schedule_else, schedule)
        self.assertLess(schedule, final_unlock)
        self.assertLess(final_unlock, stale_return)
        self.assertNotIn(
            "mesh_reschedule_owned_work_with_busy_handoff(",
            worker[stale_return:stale_return_end],
        )

    def test_immediate_channel9_response_owns_scan_until_tx_returns(self):
        send = function_body(REPORT, "mesh_send_causal_channel9_response")

        handoff = send.index("mesh_rx_handoff_begin_control(&abort_scan)")
        stop = send.index("mesh_stop_role_scan()", handoff)
        abort = send.index("dwm3000_driver_request_receive_abort(", stop)
        wait = send.index("mesh_rx_handoff_wait_for_control()", abort)
        tx = send.index("mesh_send_outbound_keep_channel9_awake(", wait)
        tx_return = send.index("out:", tx)
        handoff_end = send.index("mesh_rx_handoff_end_control()", tx_return)
        restart = send.index("mesh_restart_role_scan()", handoff_end)

        self.assertLess(handoff, stop)
        self.assertLess(stop, abort)
        self.assertLess(abort, wait)
        self.assertLess(wait, tx)
        self.assertLess(tx, tx_return)
        self.assertLess(tx_return, handoff_end)
        self.assertLess(handoff_end, restart)
        self.assertNotIn("mesh_restart_role_scan()", send[stop:tx_return])

    def test_table_publication_waits_for_current_responder_ack_quorum(self):
        service = function_body(
            ANCHOR, "gateway_discovery_assignment_service_delivery"
        )
        finalize = function_body(
            ANCHOR, "gateway_discovery_assignment_finalize_work_handler"
        )
        prepare = function_body(
            ANCHOR,
            "gateway_discovery_assignment_prepare_durable_table_locked",
        )

        response_window = service.index(
            "gateway_discovery_assignment_state.round_open = true"
        )
        table_wait = service.index(
            "} else if (kind == "
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE)",
            response_window,
        )
        self.assertIn(
            "gateway_discovery_assignment_adaptive_deadline_offset_ms_locked(\n"
            "                    1u, 1u)",
            service[table_wait:table_wait + 300],
        )

        refresh = function_body(
            ANCHOR,
            "gateway_discovery_assignment_refresh_adaptive_deadline_locked",
        )
        self.assertIn(
            "discovery_assignment_adaptive_depth_deadline_offset_ms(",
            function_body(
                ANCHOR,
                "gateway_discovery_assignment_adaptive_deadline_offset_ms_locked",
            ),
        )
        self.assertIn("response_window_origin_ms", refresh)
        self.assertIn(
            "gateway_discovery_assignment_set_response_deadline_locked(",
            refresh,
        )
        self.assertIn("adaptive-response-deadline", refresh)

        fast_complete = finalize.index(
            "gateway_discovery_assignment_state.table_delivery_succeeded"
        )
        completion = finalize.index(
            "gateway_discovery_assignment_publish_end()",
            fast_complete,
        )
        self.assertIn(
            "missing_proof_count == 0u",
            finalize[fast_complete:completion],
        )
        self.assertNotIn(
            "gateway_discovery_assignment_complete_success_locked()",
            finalize[fast_complete:completion],
        )

        publish_end = function_body(
            ANCHOR, "gateway_discovery_assignment_publish_end"
        )
        self.assertIn(
            "discovery_assignment_append_end_identity", publish_end
        )
        submit = function_body(
            ANCHOR,
            "gateway_discovery_assignment_submit_control_flood_locked",
        )
        self.assertIn(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END",
            submit,
        )
        self.assertIn("NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN", submit)

        self.assertIn("published_slot_mask |=", prepare)
        self.assertIn(
            "publication.acknowledged_mask = responder_slot_mask",
            prepare,
        )
        complete = function_body(
            ANCHOR, "gateway_discovery_assignment_complete_success_locked"
        )
        self.assertIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_END_DELIVERY", complete
        )
        self.assertNotIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS", complete
        )
        self.assertIn("event.success_count = published_count", complete)
        self.assertIn("event.failure_count = missing_count", complete)
        self.assertIn("event.total_count =\n        expected_count", complete)
        self.assertNotIn("gateway_discovery_assignment_state.ack_mask", complete)

    def test_anchor_relay_activation_skips_repeated_wakes_until_end_terminal(self):
        transport = (ROOT / "app" / "src" /
                     "app_mesh_report_transport.inc").read_text()
        send = function_body(transport, "mesh_send_c5_flood_now_until")

        self.assertIn("mesh_c5_flood_enumeration_identity", send)
        needs = send.index("protocol_rx_downstream_activation_needs_wake")
        suppress = send.index("send_wake_train = false", needs)
        mark = send.index("protocol_rx_downstream_activation_mark", suppress)
        clear = send.index("protocol_rx_downstream_activation_clear", mark)
        self.assertLess(needs, suppress)
        self.assertLess(suppress, mark)
        self.assertLess(mark, clear)
        self.assertIn("aggregate_result.sent_count > 0u", send[suppress:mark])
        self.assertIn(
            "enumeration_phase == DISCOVERY_ASSIGNMENT_PHASE_END",
            send[mark:clear],
        )
        self.assertIn(
            "aggregate_result.sent_count == attempt_count", send[mark:clear]
        )
        self.assertIn("protocol_rx_downstream_activation_expire", transport)
        rollover = send.index("discovery_assignment_epoch_strictly_newer")
        stale = send.index("return -ESTALE", rollover)
        self.assertLess(rollover, stale)
        self.assertLess(stale, needs)

    def test_failure_after_claim_rf_sends_one_claim_bound_abort(self):
        publish_abort = function_body(
            ANCHOR, "gateway_discovery_assignment_publish_abort_locked"
        )
        fail = function_body(
            ANCHOR, "gateway_discovery_assignment_fail_locked"
        )
        service = function_body(
            ANCHOR, "gateway_discovery_assignment_service_delivery"
        )
        submit = function_body(
            ANCHOR,
            "gateway_discovery_assignment_submit_control_flood_locked",
        )

        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_ABORT", publish_abort)
        self.assertIn(
            "discovery_assignment_append_abort_identity", publish_abort
        )
        self.assertIn(
            ".claim_session_id =\n"
            "            gateway_discovery_assignment_state.claim_command_seq",
            publish_abort,
        )
        self.assertIn(
            ".claim_command_seq =\n"
            "            gateway_discovery_assignment_state.claim_command_seq",
            publish_abort,
        )
        self.assertIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT", publish_abort
        )
        self.assertIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY", fail
        )
        self.assertIn("claim_rf_started", fail)
        self.assertIn(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT", service
        )
        self.assertIn(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT", submit
        )
        self.assertIn("NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN", submit)

    def test_expected_count_closes_new_claim_admission_immediately(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        closed = claim.index(
            "gateway_discovery_assignment_expected_claims_complete_locked()"
        )
        rejection = claim.index("GATEWAY_ASSIGNMENT_RETURN(-ENOSPC)", closed)
        reserve = claim.index("mesh_relay_reserve_gateway_ack_candidate(")
        self.assertLess(closed, rejection)
        self.assertLess(rejection, reserve)

        duplicate_start = claim.index("if (anchor_index != SIZE_MAX)", reserve)
        insertion_start = claim.index(
            "gateway_discovery_assignment_state.anchor_ids[",
            duplicate_start,
        )
        duplicate = claim[duplicate_start:insertion_start]
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", duplicate)

    def test_ram_only_assignment_uses_the_existing_policy_flag_end_to_end(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        prepare = function_body(
            ANCHOR,
            "gateway_discovery_assignment_prepare_durable_table_locked",
        )
        anchor_save = function_body(
            ANCHOR, "anchor_save_discovery_assignment_semantic"
        )

        self.assertIn(
            "policy_candidate.resolved.assignment.ram_only_iteration",
            start,
        )
        self.assertIn(
            "gateway_clear_registered_membership_roster_ram_only()",
            start,
        )
        self.assertIn(
            "gateway_set_registered_membership_roster_ram_only(",
            prepare,
        )
        self.assertIn(
            "anchor_discovery_assignment_ram_only_iteration", anchor_save
        )
        self.assertLess(
            anchor_save.index(
                "anchor_discovery_assignment_ram_only_iteration"
            ),
            anchor_save.index("app_durable_state_save_anchor_assignment("),
        )

    def test_failed_prepared_ram_only_publication_rolls_back_exact_owner(self):
        finish = function_body(
            ANCHOR, "gateway_discovery_assignment_finish_failure_locked"
        )
        rollback = function_body(
            GATEWAY,
            "gateway_abort_pending_assignment_publication_ram_only",
        )

        abort_then_rollback = re.search(
            r"if\s*\(\s*"
            r"app_gateway_assignment_publisher_abort_prepared_batch\s*"
            r"\(\s*&event\s*\)\s*\)\s*\{[^}]*"
            r"gateway_abort_pending_assignment_publication_ram_only\s*"
            r"\(\s*&event\s*\)",
            finish,
            re.DOTALL,
        )
        self.assertIsNotNone(
            abort_then_rollback,
            "only a successfully aborted prepared publisher may roll back "
            "its matching RAM-only membership publication",
        )
        self.assertEqual(
            finish.count(
                "gateway_abort_pending_assignment_publication_ram_only("
            ),
            1,
        )

        self.assertIn("gateway_membership_ram_only_assignment", rollback)
        self.assertIn(
            "gateway_membership_identity_matches_event(", rollback
        )
        self.assertIn(
            "&gateway_membership_assignment_identity", rollback
        )
        self.assertIn("base_event", rollback)
        self.assertIn(
            "gateway_membership_publication_live_owner = false", rollback
        )
        self.assertIn(
            "atomic_clear(&gateway_assignment_publication_pending_state)",
            rollback,
        )
        self.assertNotIn("app_durable_state_", rollback)
        self.assertNotIn(
            "gateway_clear_registered_membership_roster()", rollback
        )

    def test_late_claim_uses_the_observed_table_size(self):
        late_claim = function_body(
            ANCHOR, "anchor_schedule_late_discovery_claim"
        )

        self.assertIn(
            "&claim_command, epoch, table_entry_count, response_spread_ms",
            late_claim,
        )

    def test_expected_count_uses_only_the_short_ack_settle(self):
        service = function_body(
            ANCHOR,
            "gateway_discovery_assignment_service_delivery",
        )
        quorum = service.index(
            "gateway_discovery_assignment_expected_claims_complete_locked()"
        )
        arm = service.index(
            "gateway_discovery_assignment_arm_claim_ack_settle_locked(",
            quorum,
        )
        remaining = service.index(
            "gateway_discovery_assignment_claim_ack_settle_remaining_locked(",
            arm,
        )
        short_wait = service.index(
            'K_MSEC(wait_ms), "expected-count-settle"', remaining
        )
        self.assertLess(quorum, arm)
        self.assertLess(arm, remaining)
        self.assertLess(remaining, short_wait)

        arm_body = function_body(
            ANCHOR,
            "gateway_discovery_assignment_arm_claim_ack_settle_locked",
        )
        self.assertNotIn("claim_collection_deadline_ms", arm_body)
        self.assertIn("operation_deadline_ms", arm_body)

    def test_late_expected_count_closes_the_frozen_window_before_priority_submit(self):
        finalize = function_body(
            ANCHOR,
            "gateway_discovery_assignment_finalize_work_handler",
        )
        quorum = finalize.index(
            "gateway_discovery_assignment_expected_claims_complete_locked()"
        )
        stage = finalize.rindex(
            "GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS", 0, quorum
        )
        settle = finalize.index(
            "gateway_discovery_assignment_claim_ack_settle_remaining_locked(",
            quorum,
        )
        reschedule = finalize.index(
            'K_MSEC(settle_ms), "expected-claim-settle"', settle
        )
        close = finalize.index(
            "gateway_discovery_assignment_state.round_open = false", quorum
        )
        invalidate = finalize.index(
            "gateway_discovery_assignment_state.response_window_deadline_valid =",
            close,
        )
        response_wait = finalize.index("response-window-deadline", invalidate)
        request = finalize.index(
            "app_discovery_assignment_work_guard_request(", invalidate
        )
        submit = finalize.index("mesh_gateway_command_priority_submit(", request)

        fast_path_end = finalize.index(
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY", invalidate
        )
        self.assertIn("false", finalize[invalidate:response_wait])
        self.assertLess(stage, quorum)
        self.assertLess(quorum, settle)
        self.assertLess(settle, reschedule)
        self.assertLess(reschedule, close)
        self.assertLess(quorum, close)
        self.assertLess(close, invalidate)
        self.assertLess(invalidate, response_wait)
        self.assertLess(response_wait, request)
        self.assertLess(request, submit)
        self.assertIn(
            "GATEWAY_ASSIGNMENT_FINALIZE_RETURN()",
            finalize[settle:close],
            "the expected-count path must wait only for the bounded ACK settle",
        )

    def test_assignment_rf_telemetry_starts_at_the_physical_boundary(self):
        open_round = function_body(
            ANCHOR, "gateway_discovery_assignment_open_claim_round_locked"
        )
        observe = function_body(
            ANCHOR, "gateway_discovery_assignment_observe_rf_start_locked"
        )
        service = function_body(
            ANCHOR, "gateway_discovery_assignment_service_delivery"
        )

        self.assertNotIn(
            "GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT", open_round
        )
        attempts = observe.index("app_node_comm_delivery_attempts_started(")
        nonzero = observe.index("attempts_started == 0u", attempts)
        publish = observe.index(
            "GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT", nonzero
        )
        self.assertLess(attempts, nonzero)
        self.assertLess(nonzero, publish)
        self.assertIn("DBG_ENUM_RF_BEGIN", observe[publish:])

        poll = service.index(
            "gateway_discovery_assignment_observe_rf_start_locked()"
        )
        terminal = service.index(
            "app_node_comm_peek_delivery_event_for(handle, &event)", poll
        )
        self.assertLess(poll, terminal)

    def test_failed_bounded_assignment_rf_is_terminal_radio(self):
        service = function_body(
            ANCHOR, "gateway_discovery_assignment_service_delivery"
        )
        result = service.index("DBG_ENUM_RF_RESULT")
        delivered = service.index(
            "effective_delivered = "
            "event.reason == NODE_COMM_TERMINAL_DELIVERED",
            result,
        )
        failure = service.index(
            "gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR",
            delivered,
        )

        self.assertLess(result, delivered)
        self.assertLess(delivered, failure)
        self.assertNotIn(
            "gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT",
            service,
        )
        self.assertNotIn("delivery-terminal-retry", service)

    def test_assignment_terminal_distinguishes_pre_rf_from_no_anchors(self):
        finish = function_body(
            ANCHOR, "gateway_discovery_assignment_finish_failure_locked"
        )

        pre_rf = finish.index(
            "!gateway_discovery_assignment_state.claim_rf_started"
        )
        radio_status = finish.index("status = COMMAND_RADIO_ERROR", pre_rf)
        radio_reason = finish.index(
            "event_reason = GATEWAY_COMMAND_EVENT_REASON_RADIO", radio_status
        )
        expired = finish.index(
            "app_discovery_assignment_operation_expired(", radio_reason
        )
        no_anchors = finish.index(
            "GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS", expired
        )

        self.assertLess(pre_rf, radio_status)
        self.assertLess(radio_status, radio_reason)
        self.assertLess(radio_reason, expired)
        self.assertIn(
            "gateway_discovery_assignment_state.claim_rf_started",
            finish[expired:no_anchors],
        )
        terminal_attempt = finish.index(
            "event.attempt = gateway_discovery_assignment_state.claim_round"
        )
        terminal_publish = finish.index(
            "gateway_observe_command_event(&event, true)", terminal_attempt
        )
        self.assertLess(no_anchors, terminal_attempt)
        self.assertLess(terminal_attempt, terminal_publish)

    def test_only_owned_acceptance_reaches_the_gateway_ack_gate(self):
        real_gateway = GATEWAY[
            GATEWAY.index(
                "static struct gateway_command_pending gateway_command_pending_state"
            ) :
        ]
        result = function_body(real_gateway, "gateway_note_command_result")
        claim_call = result.index("gateway_discovery_assignment_note_claim")
        ownership = result.index(
            "if (claim_ret != -ENOENT && claim_ret != -ENOTSUP)", claim_call
        )
        collection = result.index("gateway_note_collection_result", ownership)
        self.assertLess(claim_call, ownership)
        self.assertLess(ownership, collection)
        self.assertIn("return claim_ret;", result[ownership:collection])
        self.assertNotIn("claim_ret", result[collection:])

        semantic = function_body(REPORT, "mesh_gateway_accept_semantic_delivery")
        command_case = semantic[
            semantic.index("case MSG_COMMAND_RESULT:") :
            semantic.index("case MSG_RESULT_BUNDLE:")
        ]
        self.assertIn("return gateway_note_command_result", command_case)

        drain = function_body(REPORT, "mesh_drain_rx_queue_locked")
        reserve = drain.index("gateway_ble_reserve_stream_packet")
        classify = drain.index("mesh_gateway_accept_semantic_delivery")
        accepted = drain.index("if (semantic_ret < 0)", classify)
        commit = drain.index("mesh_relay_commit_gateway_delivery", accepted)
        self.assertLess(reserve, classify)
        self.assertLess(classify, accepted)
        self.assertLess(accepted, commit)


if __name__ == "__main__":
    unittest.main()
