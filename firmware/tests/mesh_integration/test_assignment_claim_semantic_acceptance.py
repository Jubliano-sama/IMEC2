#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text(encoding="utf-8")
ANCHOR_HEADER = (ROOT / "app/src/app_anchor.h").read_text(encoding="utf-8")
GATEWAY = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")
REPORT = (ROOT / "app/src/app_mesh_report.c").read_text(encoding="utf-8")


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
        inactive = claim.index(
            "!gateway_discovery_assignment_state.active", wrong_command
        )
        decode = claim.index("discovery_assignment_extract_control_tlvs", inactive)

        self.assertLess(parse, wrong_command)
        self.assertLess(wrong_command, inactive)
        self.assertLess(inactive, decode)
        self.assertIn("return -ENOENT;", claim[:parse])
        self.assertIn("return -EBADMSG;", claim[parse:wrong_command])
        self.assertIn("return -ENOENT;", claim[wrong_command:inactive])
        self.assertIn("return -ESTALE;", claim[inactive:decode])

    def test_malformed_and_stale_claims_are_never_accepted(self):
        claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
        decode = claim.index("discovery_assignment_extract_control_tlvs")
        lookup = claim.index("for (size_t i = 0u", decode)
        validation = claim[decode:lookup]

        for required in (
            "DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            "DISCOVERY_ASSIGNMENT_PHASE_ACK",
            "gateway_discovery_assignment_state.epoch",
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
        self.assertIn("return -EBADMSG;", validation)
        self.assertIn("return -EPROTO;", validation)
        self.assertIn("return -ESTALE;", validation)

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
            "return -ESTALE;",
            "ack_mask |=",
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW",
        ):
            with self.subTest(ack_required=required):
                self.assertIn(required, ack)
        self.assertIn("ack_mask &", ack)
        self.assertIn("duplicate_count", duplicate)
        self.assertIn("return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;", duplicate)
        self.assertIn("return -ENOSPC;", capacity_and_insert)
        self.assertIn("claim_count++", capacity_and_insert)
        self.assertTrue(
            capacity_and_insert.rstrip().endswith(
                "return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;\n}"
            )
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
