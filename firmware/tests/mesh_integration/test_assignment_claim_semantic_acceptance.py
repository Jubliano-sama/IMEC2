#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
ANCHOR_HEADER = (ROOT / "app/src/app_anchor.h").read_text(encoding="utf-8")
GATEWAY = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")


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
    def test_claim_handler_exposes_owned_tristate_contract(self):
        self.assertRegex(
            ANCHOR_HEADER,
            r"\bint\s+gateway_discovery_assignment_note_claim\s*\(",
        )
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")

        parse = claim.index("gateway_command_extract_id")
        wrong_command = claim.index(
            "command_id != CMD_ASSIGN_DISCOVERY_SLOTS", parse
        )
        decode = claim.index(
            "discovery_assignment_extract_control_tlvs", wrong_command
        )
        status = claim.index("TLV_COMMAND_STATUS", decode)
        claim_hash = claim.index(
            "discovery_assignment_extract_claim_hash", status
        )
        hop = claim.index("TLV_HOP_COUNT", claim_hash)
        lock = claim.index(
            "k_mutex_lock(&gateway_discovery_assignment_mutex", hop
        )
        inactive = claim.index(
            "!gateway_discovery_assignment_state.active", lock
        )

        self.assertLess(parse, wrong_command)
        self.assertLess(wrong_command, decode)
        self.assertLess(decode, status)
        self.assertLess(status, claim_hash)
        self.assertLess(claim_hash, hop)
        self.assertLess(hop, lock)
        self.assertLess(lock, inactive)
        self.assertIn("return -ENOENT;", claim[:parse])
        self.assertIn("return -EBADMSG;", claim[parse:wrong_command])
        self.assertIn("return -ENOENT;", claim[wrong_command:decode])
        self.assertTrue(has_owned_return(
            claim[inactive:],
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
        ))
        self.assertIn("k_mutex_unlock(&gateway_discovery_assignment_mutex)", claim)

    def test_malformed_replies_fail_before_retired_results_are_accepted(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        decode = claim.index("discovery_assignment_extract_control_tlvs")
        lock = claim.index(
            "k_mutex_lock(&gateway_discovery_assignment_mutex", decode
        )
        validation = claim[decode:lock]

        for required in (
            "DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            "DISCOVERY_ASSIGNMENT_PHASE_ACK",
            "TLV_COMMAND_STATUS",
            "COMMAND_OK",
            "discovery_assignment_extract_claim_hash",
            "discovery_assignment_hash(packet->src_id)",
            "packet->src_id == 0u",
            "TLV_HOP_COUNT",
            "hop_len != sizeof(uint8_t)",
        ):
            with self.subTest(required=required):
                self.assertIn(required, validation)
        self.assertTrue(has_owned_return(validation, "-EBADMSG"))
        self.assertTrue(has_owned_return(validation, "-EPROTO"))

        lookup = claim.index("for (size_t i = 0u", lock)
        retired = claim[lock:lookup]
        for required in (
            "!gateway_discovery_assignment_state.active",
            "epoch != gateway_discovery_assignment_state.epoch",
            "app_discovery_assignment_operation_expired(",
        ):
            with self.subTest(retired_gate=required):
                gate = retired.index(required)
                next_gate = retired.find("if (", gate + len(required))
                branch = retired[
                    gate : next_gate if next_gate >= 0 else len(retired)
                ]
                self.assertTrue(has_owned_return(
                    branch,
                    "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
                ))
        self.assertNotIn("APP_GATEWAY_SEMANTIC_ACCEPT", validation)

    def test_every_assignment_state_gate_is_visible_in_hardware_traces(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")

        for reason in (
            "reason=control",
            "reason=phase",
            "reason=rf-start",
        ):
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
        ack_start = claim.index(
            "if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK)"
        )
        duplicate_start = claim.index("if (anchor_index != SIZE_MAX)", ack_start)
        capacity_start = claim.index(
            "if (gateway_discovery_assignment_state.claim_count >=",
            duplicate_start,
        )
        ack = claim[ack_start:duplicate_start]
        duplicate = claim[duplicate_start:capacity_start]
        capacity_and_insert = claim[capacity_start:]

        for required in (
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS",
            "table_command_seq",
            "anchor_index == SIZE_MAX",
            "ack_mask |=",
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW",
        ):
            with self.subTest(ack_required=required):
                self.assertIn(required, ack)
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", ack)
        self.assertIn("ack_mask &", ack)
        self.assertIn("duplicate_count", duplicate)
        self.assertTrue(has_owned_return(
            duplicate, "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE"
        ))
        self.assertTrue(has_owned_return(capacity_and_insert, "-ENOSPC"))
        self.assertIn("claim_count++", capacity_and_insert)
        self.assertTrue(has_owned_return(
            capacity_and_insert, "APP_GATEWAY_SEMANTIC_ACCEPT_NEW"
        ))

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
