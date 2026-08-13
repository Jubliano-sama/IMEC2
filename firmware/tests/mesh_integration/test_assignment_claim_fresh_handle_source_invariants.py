#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text(
    encoding="utf-8"
)
PAIR_RESULT = (ROOT / "app/src/app_anchor_survey_result_delivery.c").read_text(
    encoding="utf-8"
)
POLICY_HEADER = (ROOT / "include/discovery_assignment.h").read_text(
    encoding="utf-8"
)
OPERATION_HEADER = (ROOT / "include/operation_policy.h").read_text(
    encoding="utf-8"
)
GATEWAY_COMMAND_HEADER = (ROOT / "include/gateway_command.h").read_text(
    encoding="utf-8"
)
GUI_POLICY = (ROOT.parent / "tools/gateway_gui/operation_policy.py").read_text(
    encoding="utf-8"
)
GUI_PROTOCOL = (ROOT.parent / "tools/gateway_gui/protocol.py").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{re.escape(name)}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth:
                continue
            cursor = index + 1
            while cursor < len(source) and source[cursor].isspace():
                cursor += 1
            if cursor >= len(source) or source[cursor] != "{":
                break
            depth = 0
            for end in range(cursor, len(source)):
                depth += source[end] == "{"
                depth -= source[end] == "}"
                if depth == 0:
                    return source[cursor : end + 1]
    raise AssertionError(f"function not found: {name}")


def macro_value(source: str, name: str) -> int:
    match = re.search(rf"#define\s+{re.escape(name)}\s+(\d+)u?\b", source)
    if match is None:
        raise AssertionError(f"macro not found: {name}")
    return int(match.group(1))


class AssignmentClaimFreshHandleSourceTests(unittest.TestCase):
    def test_claim_terminal_gets_two_fresh_handles_and_deadlines(self):
        self.assertEqual(
            2,
            macro_value(
                POLICY_HEADER,
                "DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES",
            ),
        )
        self.assertEqual(
            598,
            macro_value(
                POLICY_HEADER,
                "DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS",
            ),
        )
        worker = function_body(ANCHOR, "anchor_discovery_claim_work_handler")
        terminal = worker.index("pending.delivery_handle != 0u")
        claim_retry = worker.index(
            "pending.phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM", terminal
        )
        retry_cap = worker.index(
            "DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES", claim_retry
        )
        cancel = worker.index(
            "event.reason != NODE_COMM_TERMINAL_CANCELLED", terminal
        )
        delivered = worker.index(
            "event.reason != NODE_COMM_TERMINAL_DELIVERED", terminal
        )
        clear = worker.index(
            "anchor_discovery_claim_pending.delivery_handle = 0u", retry_cap
        )
        increment = worker.index(
            "anchor_discovery_claim_pending.terminal_retry_count++", clear
        )
        refresh = worker.index(
            "anchor_discovery_terminal_retry_refresh_deadline_locked(",
            increment,
        )
        reschedule = worker.index(
            "anchor_discovery_claim_reschedule_locked(", refresh
        )
        retire = worker.index(
            "anchor_discovery_claim_pending.active = false", reschedule
        )

        self.assertLess(delivered, claim_retry)
        self.assertLess(cancel, claim_retry)
        self.assertLess(claim_retry, retry_cap)
        self.assertLess(clear, increment)
        self.assertLess(increment, refresh)
        self.assertLess(refresh, reschedule)
        self.assertLess(reschedule, retire)

        refresher = function_body(
            ANCHOR, "anchor_discovery_terminal_retry_refresh_deadline_locked"
        )
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_CLAIM", refresher)
        self.assertIn("DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES", refresher)
        self.assertIn("discovery_assignment_response_deadline_ms(", refresher)

        scheduler = function_body(ANCHOR, "anchor_schedule_discovery_response")
        self.assertIn("generation = anchor_discovery_claim_next_generation(", scheduler)
        self.assertIn("app_node_comm_abandon_delivery(replaced_delivery_handle)", scheduler)
        self.assertIn("anchor_discovery_claim_pending = replaced_pending", scheduler)

        supersession = function_body(
            ANCHOR, "anchor_settle_ack_before_newer_assignment"
        )
        inactive = supersession[
            supersession.index("if (!anchor_discovery_claim_pending.active") :
            supersession.index("pending = anchor_discovery_claim_pending")
        ]
        self.assertEqual(
            1,
            inactive.count("k_mutex_unlock(&anchor_discovery_claim_mutex)"),
            "inactive supersession must release the claim mutex exactly once",
        )

    def test_claim_budget_matches_firmware_gui_and_stays_deployable(self):
        required = re.search(
            r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS"
            r"\(response_spread_ms\)\s*\\(?P<body>.*?)"
            r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS",
            POLICY_HEADER,
            re.DOTALL,
        )
        self.assertIsNotNone(required)
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES *",
            required.group("body"),
        )
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS",
            required.group("body"),
        )

        firmware_default = macro_value(
            OPERATION_HEADER,
            "OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS",
        )
        gui_default = re.search(
            r"^ASSIGNMENT_DEFAULT_BUDGET_MS\s*=\s*([\d_]+)\s*$",
            GUI_POLICY,
            re.MULTILINE,
        )
        discovery_policy_max = macro_value(
            OPERATION_HEADER, "OPERATION_POLICY_COMMAND_BUDGET_MAX_MS"
        )
        command_max = macro_value(
            GATEWAY_COMMAND_HEADER, "GATEWAY_COMMAND_BUDGET_MAX_MS"
        )
        gui_discovery_policy_max = re.search(
            r"^COMMAND_BUDGET_MAX_MS\s*=\s*([\d_]+)\s*$",
            GUI_POLICY,
            re.MULTILINE,
        )
        gui_command_max = re.search(
            r"^GATEWAY_COMMAND_BUDGET_MAX_MS\s*=\s*([\d_]+)\s*$",
            GUI_PROTOCOL,
            re.MULTILINE,
        )
        self.assertIsNotNone(gui_default)
        self.assertIsNotNone(gui_discovery_policy_max)
        self.assertIsNotNone(gui_command_max)
        self.assertEqual(751_204, firmware_default)
        self.assertEqual(firmware_default, int(gui_default.group(1).replace("_", "")))
        self.assertEqual(900_000, discovery_policy_max)
        self.assertEqual(
            discovery_policy_max,
            int(gui_discovery_policy_max.group(1).replace("_", "")),
        )
        self.assertEqual(3_600_000, command_max)
        self.assertEqual(
            command_max, int(gui_command_max.group(1).replace("_", ""))
        )
        self.assertLessEqual(895_204, discovery_policy_max)

    def test_analogous_local_result_owners_rearm_same_identity(self):
        discovery_retry = function_body(
            DISCOVERY, "app_anchor_survey_discovery_retry_report"
        )
        discovery_terminal = function_body(
            DISCOVERY, "survey_delivery_poll_comm_result"
        )
        self.assertIn("SURVEY_DISCOVERY_REPORT_SOURCE_DEADLINE_MS", discovery_retry)
        self.assertRegex(
            DISCOVERY,
            r"#define\s+SURVEY_DISCOVERY_REPORT_SOURCE_DEADLINE_MS\s+UINT64_MAX",
        )
        self.assertIn("survey_delivery_retain_for_retry_locked", discovery_terminal)
        self.assertIn("survey_delivery_identity_equal", discovery_terminal)
        self.assertIn("app_mesh_local_delivery_identity_matches", discovery_terminal)

        pair_terminal = function_body(
            PAIR_RESULT, "result_delivery_service_terminal"
        )
        pair_slot = function_body(PAIR_RESULT, "result_delivery_service_slot")
        self.assertRegex(
            PAIR_RESULT,
            r"#define\s+SURVEY_PAIR_RESULT_SOURCE_DEADLINE_MS\s+UINT64_MAX",
        )
        self.assertIn("result_delivery_retain_for_retry_locked", pair_terminal)
        self.assertIn("result_delivery_same_packet", pair_terminal)
        self.assertIn("SURVEY_PAIR_RESULT_SOURCE_DEADLINE_MS", pair_slot)

        command_result = function_body(
            ANCHOR, "anchor_collection_result_work_handler_locked"
        )
        self.assertIn("anchor_collection_result_pending.delivery_handle = 0u", command_result)
        self.assertIn('"command-result-terminal-resubmit"', command_result)
        self.assertIn("anchor_submit_command_result(", command_result)


if __name__ == "__main__":
    unittest.main()
