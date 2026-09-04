#!/usr/bin/env python3
"""Keep application retry ownership connected to relay refusal actions."""

from pathlib import Path
import re
import unittest

from source_text import read_composed_source

ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")


class C5DeferredAckSourceInvariants(unittest.TestCase):
    def test_answered_retry_actions_rearm_the_shared_timeout_owner(self):
        for action in ("TX_HOP_PROGRESS", "TX_RELAY_BUSY", "TX_HOP_DEFERRED"):
            with self.subTest(action=action):
                branch = re.search(
                    rf"if \(result->actions & MESH_RELAY_ACTION_{action}\)"
                    r"\s*\{([^{}]*)\}", REPORT)
                self.assertIsNotNone(branch)
                self.assertIn("mesh_schedule_tx_timeout()", branch.group(1))
                self.assertNotIn("mesh_relay_note_tx_failed", branch.group(1))

    def test_leaf_allows_refusal_without_admitting_relay_work(self):
        allowed = re.search(r"const uint32_t allowed_actions\s*=([^;]+);", REPORT)
        self.assertIsNotNone(allowed)
        for action in ("TX_HOP_PROGRESS", "TX_RELAY_BUSY", "TX_HOP_DEFERRED"):
            self.assertIn(f"MESH_RELAY_ACTION_{action}", allowed.group(1))
        self.assertNotIn("MESH_RELAY_ACTION_FORWARD", allowed.group(1))

    def test_backoff_uses_retained_deadline_and_owned_scheduler(self):
        start = REPORT.index("static int mesh_schedule_tx_timeout(void)\n{")
        end = REPORT.index("\n}\n", start)
        scheduler = REPORT[start:end]
        self.assertRegex(scheduler,
                         r"MESH_RELAY_TX_WAIT_RETRY_BACKOFF\s*\?\s*"
                         r"mesh_runtime.pending.retry_after_ms")
        self.assertIn("mesh_reschedule_owned_work(&mesh_tx_timeout_work", scheduler)


if __name__ == "__main__":
    unittest.main()
