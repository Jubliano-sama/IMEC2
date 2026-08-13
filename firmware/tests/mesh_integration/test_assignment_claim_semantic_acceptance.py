#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
ANCHOR_HEADER = (ROOT / "app/src/app_anchor.h").read_text(encoding="utf-8")
GATEWAY = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
RELAY = (ROOT / "src/mesh_relay.c").read_text(encoding="utf-8")
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
            "discovery_assignment_table_collection_window_ms",
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
            "discovery_assignment_table_collection_window_ms(", stage
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
            "gateway_discovery_assignment_state.operation_deadline_ms",
            deadline_branch,
        )
        self.assertNotIn("APP_GATEWAY_SEMANTIC_ACCEPT", validation)

    def test_every_assignment_state_gate_is_visible_in_hardware_traces(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")

        for reason in ("reason=envelope", "reason=rf-start"):
            with self.subTest(reason=reason):
                self.assertIn(
                    f"DBG_DISCOVERY_SLOT_CLAIM_STATE_REJECT {reason}",
                    claim,
                )
        for reason in (
            "reason=inactive",
            "reason=epoch",
            "reason=deadline",
            "reason=ack-state",
        ):
            with self.subTest(retired_reason=reason):
                self.assertIn(
                    f"DBG_DISCOVERY_SLOT_RESULT_RETIRED {reason}",
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
        self.assertNotIn("GATEWAY_ASSIGNMENT_RETURN(-EAGAIN)", ack)
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

    def test_retained_roster_table_acks_refresh_reliable_hop_projection(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        reset = start.index(
            "memset(&gateway_discovery_assignment_state"
        )
        retained = start.index("if (prior_anchor_count != 0u)", reset)
        activate = start.index(
            "gateway_discovery_assignment_state.active = true", retained
        )
        retained_roster = start[reset:activate]

        self.assertIn(
            "memcpy(gateway_discovery_assignment_state.anchor_ids",
            retained_roster,
        )
        self.assertIn(
            "memcpy(gateway_discovery_assignment_state.anchor_slots",
            retained_roster,
        )
        self.assertNotIn(
            "memcpy(gateway_discovery_assignment_state.anchor_hop_counts",
            retained_roster,
            "a new assignment must not borrow stale route evidence",
        )

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

        complete = function_body(
            ANCHOR,
            "gateway_discovery_assignment_complete_success_locked",
        )
        prepare = complete.index(
            "app_gateway_assignment_publisher_prepare_table("
        )
        persist = complete.index(
            "gateway_set_registered_membership_roster(", prepare
        )
        publisher_call = complete[prepare:persist]
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
        semantic_start = service.index(
            "app_discovery_assignment_semantic_terminal_success("
        )
        semantic_end = service.index(");", semantic_start)
        self.assertIn(
            "gateway_discovery_assignment_current_claim_count_locked()",
            service[semantic_start:semantic_end],
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

    def test_table_correlated_late_claim_redrives_without_releasing_custody(self):
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
            "gateway_discovery_assignment_missing_ack_count_locked() == 0u"
        )
        pending = publish.index("late_table_redrive_pending", missing)
        table = publish.index(
            "gateway_discovery_assignment_publish_table()", pending
        )
        normal_limit = publish.index(
            "gateway_discovery_assignment_state.table_round >=", table
        )
        self.assertLess(missing, pending)
        self.assertLess(pending, table)
        self.assertLess(table, normal_limit)

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
            "gateway_discovery_assignment_state.late_table_redrive_pending ||"
        )
        retry_schedule = table_publish.index("table-admission-retry", retry_branch)
        self.assertLess(retry_branch, retry_schedule)

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
            "gateway_discovery_assignment_live_proof("
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
            "gateway_discovery_assignment_state.prior_anchor_count",
            admission,
        )
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

    def test_append_only_roster_has_no_production_clear_caller(self):
        start = function_body(ANCHOR, "gateway_start_discovery_assignment")
        complete = function_body(
            ANCHOR,
            "gateway_discovery_assignment_complete_success_locked",
        )
        self.assertIn(
            "gateway_discovery_assignment_state.claim_count =\n"
            "        prior_anchor_count",
            start,
        )
        self.assertIn("published_slot_mask |=", complete)
        self.assertNotIn("gateway_discovery_assignment_state.ack_mask", complete)
        self.assertNotIn("!acknowledged", complete)

        for path in (ROOT / "app/src").glob("*"):
            if path.suffix not in {".c", ".inc"}:
                continue
            source = path.read_text(encoding="utf-8")
            for match in re.finditer(
                r"\bgateway_clear_registered_membership_roster\s*\(",
                source,
            ):
                prefix = source[max(0, match.start() - 32):match.start()]
                self.assertRegex(
                    prefix,
                    r"\bvoid\s+$",
                    f"production clear caller remains in {path.name}",
                )

    def test_table_publication_does_not_wait_for_optional_anchor_acks(self):
        service = function_body(
            ANCHOR, "gateway_discovery_assignment_service_delivery"
        )
        finalize = function_body(
            ANCHOR, "gateway_discovery_assignment_finalize_work_handler"
        )
        complete = function_body(
            ANCHOR, "gateway_discovery_assignment_complete_success_locked"
        )

        response_window = service.index(
            "gateway_discovery_assignment_state.round_open = true"
        )
        table_wait = service.index(
            "} else if (kind == "
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE)",
            response_window,
        )
        self.assertIn("wait_ms = 0u", service[table_wait:table_wait + 300])

        fast_complete = finalize.index(
            "gateway_discovery_assignment_state.table_delivery_succeeded"
        )
        completion = finalize.index(
            "gateway_discovery_assignment_complete_success_locked()",
            fast_complete,
        )
        self.assertNotIn(
            "missing_ack_count == 0u",
            finalize[fast_complete:completion],
        )

        self.assertIn("published_slot_mask |=", complete)
        self.assertIn(
            "publication.acknowledged_mask = published_slot_mask",
            complete,
        )
        self.assertIn("event.success_count = published_count", complete)
        self.assertIn("event.failure_count = 0u", complete)
        self.assertNotIn("gateway_discovery_assignment_state.ack_mask", complete)

    def test_duplicate_claim_restarts_settle_to_immutable_horizon(self):
        self.assertIsNotNone(
            re.search(
                r"\buint64_t\s+claim_collection_deadline_ms\s*;",
                ANCHOR,
            ),
            "assignment state is missing its immutable collection horizon",
        )
        settle = function_body(
            ANCHOR,
            "gateway_discovery_assignment_arm_claim_ack_settle_locked",
        )
        self.assertIn(
            "discovery_assignment_claim_ack_settle_deadline_ms(",
            settle,
        )
        self.assertIn("claim_collection_deadline_ms", settle)
        self.assertIn("operation_deadline_ms", settle)
        latest_deadline_sources = re.findall(
            r"latest_deadline\s*=\s*([^;]+);",
            settle,
        )
        self.assertGreaterEqual(len(latest_deadline_sources), 2)
        for source in latest_deadline_sources:
            with self.subTest(latest_deadline_source=source.strip()):
                self.assertNotIn("response_window_deadline", source)
        self.assertNotIn(
            "candidate_deadline = response_deadline",
            settle,
        )
        self.assertIn(
            "response_window_deadline_ms =\n"
            "                (uint32_t)candidate_deadline",
            settle,
            "extended settle does not keep semantic RX/finalization open",
        )

        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        duplicate_start = claim.index("if (anchor_index != SIZE_MAX)")
        insertion_start = claim.index(
            "gateway_discovery_assignment_state.anchor_ids[",
            duplicate_start,
        )
        duplicate = claim[duplicate_start:insertion_start]
        self.assertIn(
            "gateway_discovery_assignment_arm_claim_ack_settle_locked(",
            duplicate,
        )
        self.assertIn("(uint32_t)received_at_ms", duplicate)
        self.assertIn(
            "gateway_discovery_assignment_reschedule(",
            duplicate,
        )
        self.assertIn("K_MSEC(settle_ms)", duplicate)
        self.assertIn('"duplicate-claim-settle"', duplicate)
        self.assertNotIn("k_work_reschedule(", duplicate)

        service = function_body(
            ANCHOR,
            "gateway_discovery_assignment_service_delivery",
        )
        self.assertEqual(
            service.count("claim_collection_deadline_ms ="),
            1,
            "claim collection horizon must be frozen once per delivered phase",
        )

    def test_expected_claim_settle_preempts_the_long_response_window(self):
        finalize = function_body(
            ANCHOR,
            "gateway_discovery_assignment_finalize_work_handler",
        )
        round_open = finalize.index(
            "if (gateway_discovery_assignment_state.round_open)"
        )
        quorum = finalize.index(
            "gateway_discovery_assignment_expected_claims_complete_locked()",
            round_open,
        )
        remaining = finalize.index(
            "gateway_discovery_assignment_claim_ack_settle_remaining_locked(",
            quorum,
        )
        settle_schedule = finalize.index('"expected-claim-settle"', remaining)
        validation_boundary = finalize.index(
            "gateway_discovery_assignment_boundary_ready_locked(",
            settle_schedule,
        )
        response_window = finalize.index(
            ".response_window_deadline_valid", validation_boundary
        )

        self.assertLess(round_open, quorum)
        self.assertLess(quorum, remaining)
        self.assertLess(remaining, settle_schedule)
        self.assertLess(settle_schedule, validation_boundary)
        self.assertLess(validation_boundary, response_window)
        self.assertIn(
            ".claim_ack_settle_deadline_ms",
            finalize[validation_boundary:response_window],
        )
        self.assertIn(
            "else if (gateway_discovery_assignment_state",
            finalize[settle_schedule:response_window],
            "the generic response window can still mask completed quorum",
        )

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
