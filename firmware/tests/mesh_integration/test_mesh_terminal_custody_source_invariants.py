#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
ANCHOR = read_composed_source(ROOT / "app" / "src" / "app_anchor.c")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


class MeshTerminalCustodySourceInvariantTests(unittest.TestCase):
    def test_transit_terminal_commits_without_source_owner_cleanup(self) -> None:
        release = function_body(REPORT, "mesh_complete_terminal_release")
        branch_at = release.index("terminal->packet.src_id != DEVICE_ID")
        local_range_at = release.index(
            "mesh_anchor_range_report_note_terminal_release(", branch_at
        )
        transit = release[branch_at:local_range_at]
        self.assertIn("mesh_relay_commit_terminal_release(", transit)
        self.assertNotIn("mesh_save_outbox_durable", transit)
        self.assertNotIn("mesh_deferred_outbox_pending", transit)
        for source_owner_cleanup in (
            "mesh_anchor_range_report_note_terminal_release(",
            "anchor_survey_delivery_transport_released(",
            "app_node_comm_note_gateway_failed_digest(",
        ):
            with self.subTest(cleanup=source_owner_cleanup):
                self.assertNotIn(source_owner_cleanup, transit)

        local_node_comm_at = release.index(
            "app_node_comm_note_gateway_failed_digest(", local_range_at
        )
        self.assertLess(branch_at, local_range_at)
        self.assertLess(local_range_at, local_node_comm_at)

    def test_terminal_cleanup_retry_uses_bounded_cadence(self) -> None:
        schedule = function_body(REPORT, "mesh_schedule_tx_timeout")
        terminal_match = re.search(
            r"pending\.state\s*==\s*MESH_RELAY_TX_WAIT_TERMINAL_COMMIT\)\s*\{"
            r"(?P<body>.*?)\n\s*\}\s*else",
            schedule,
            re.DOTALL,
        )

        self.assertIsNotNone(terminal_match)
        terminal = terminal_match.group("body")
        self.assertIn("deadline = now + REPORT_TX_RETRY_DELAY_MS", terminal)
        terminal_without_comments = re.sub(
            r"/\*.*?\*/", "", terminal, flags=re.DOTALL
        )
        self.assertNotIn("gateway_ack_deadline_ms", terminal_without_comments)
        self.assertNotRegex(terminal, r"(?:deadline|delay_ms)\s*=\s*(?:0u|1u)\b")

        tick = function_body(REPORT, "mesh_tx_timeout_handler")
        release_action = tick.index("mesh_handle_result_actions(")
        reschedule = tick.index("mesh_schedule_tx_timeout()", release_action)
        self.assertLess(release_action, reschedule)

    def test_newer_claim_settles_any_older_ack_before_admission(self) -> None:
        apply_assignment = function_body(
            ANCHOR, "anchor_apply_discovery_assignment_command"
        )
        same_epoch = apply_assignment.index("epoch == snapshot.pending_epoch")
        same_epoch_resume = apply_assignment.index(
            "anchor_resume_pending_discovery_assignment_ack(false)", same_epoch
        )
        same_epoch_return = apply_assignment.index("return 0;", same_epoch_resume)
        newer = apply_assignment.index(
            "discovery_assignment_epoch_strictly_newer(", same_epoch_return
        )
        settle = apply_assignment.index(
            "anchor_settle_ack_before_newer_assignment(epoch)", newer
        )
        note_claim = apply_assignment.index(
            "local_anchor_discovery_assignment_note_claim(epoch)", settle
        )
        schedule_claim = apply_assignment.index(
            "anchor_schedule_discovery_claim(", note_claim
        )

        supersession_gate = apply_assignment[same_epoch_return:newer]
        self.assertNotIn("snapshot.provisioned", supersession_gate)
        self.assertLess(same_epoch, same_epoch_resume)
        self.assertLess(same_epoch_resume, same_epoch_return)
        self.assertLess(newer, settle)
        self.assertLess(settle, note_claim)
        self.assertLess(note_claim, schedule_claim)

        settle_body = function_body(
            ANCHOR, "anchor_settle_ack_before_newer_assignment"
        )
        cancel = settle_body.index("app_node_comm_cancel_delivery(")
        take_terminal = settle_body.index(
            "app_node_comm_take_delivery_event_for(", cancel
        )
        retire = settle_body.index(
            "anchor_retire_superseded_discovery_ack(&pending)", take_terminal
        )
        self.assertLess(cancel, take_terminal)
        self.assertLess(take_terminal, retire)


if __name__ == "__main__":
    unittest.main()
