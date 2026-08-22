#!/usr/bin/env python3
"""Source invariants for the boost-active single-shot mesh route wake train.

When the sender's own operation decode latched the high-duty boost, the route
responder decoded the same flood and sits in ~100% Channel-5 RX, so the wake
train must collapse to one WAKE_CLAIM opportunity instead of cycling
MESH_RADIO_WAKE_OPPORTUNITIES for the full 400 ms window.  Without the boost
the full train must remain untouched.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
ROUTE_CONTROL = (ROOT / "app/src/app_mesh_report_route_control.inc").read_text(
    encoding="utf-8"
)
ANCHOR_RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text(encoding="utf-8")
CONFIG = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")


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


class MeshRouteWakeBoostSourceInvariants(unittest.TestCase):
    def test_boost_constant_is_two_control_tx_budgets(self):
        # One claim TX at the worst-case control-frame timeout budget plus an
        # equal turnaround margin; it must never exceed the full train.
        constant = re.search(
            r"#define\s+MESH_ROUTE_WAKE_ADV_BOOST_ACTIVE_MS\s*\\\n\s*(.+)",
            CONFIG,
        )
        self.assertIsNotNone(constant)
        self.assertEqual(
            constant.group(1).strip(),
            "(UWB_CONTROL_TX_TIMEOUT_MS + UWB_CONTROL_TX_TIMEOUT_MS)",
        )
        self.assertRegex(
            CONFIG,
            r"#define\s+UWB_CONTROL_TX_TIMEOUT_MS\s+20u",
        )
        self.assertRegex(CONFIG, r"#define\s+WAKE_ADV_MS\s+MESH_RADIO_WAKE_TRAIN_MS")

    def test_anchor_exports_boost_active_query(self):
        # The route wake-train owner lives in app_mesh_report.c and can only
        # reach the anchor radio owner through a non-static definition.
        definition = re.search(
            r"^bool anchor_relay_control_followup_boost_active\(void\)$",
            ANCHOR_RADIO,
            re.MULTILINE,
        )
        self.assertIsNotNone(definition)
        body = function_body(ANCHOR_RADIO, "anchor_relay_control_followup_boost_active")
        self.assertIn("anchor_relay_control_followup_boost_active_state", body)
        self.assertIn("uptime_deadline_reached", body)

    def test_wake_train_selects_short_window_only_when_boost_active(self):
        wake = function_body(ROUTE_CONTROL, "mesh_send_route_wake_train")
        attempt = wake.index("wake_train_attempt:")
        selection = wake.index("boost_single_shot =", attempt)
        query = wake.index("anchor_relay_control_followup_boost_active()", selection)
        branch = wake.index("wake_train_config.wake_adv_ms = boost_single_shot ?", query)
        short = wake.index("MESH_ROUTE_WAKE_ADV_BOOST_ACTIVE_MS", branch)
        fallback = wake.index("WAKE_ADV_MS", short)
        debug = wake.index("DBG_WAKE_TRAIN_BOOST_SINGLE", fallback)
        contact_open = wake.index("mesh_c5_contact_open(", debug)

        # The selection happens per polite-retry round, before the train
        # window (close_ms) and the contact deadline are computed from it.
        self.assertLess(attempt, selection)
        self.assertLess(query, branch)
        self.assertLess(branch, short)
        self.assertLess(short, fallback)
        self.assertLess(fallback, debug)
        self.assertLess(debug, contact_open)

    def test_boost_train_sends_exactly_one_claim_opportunity(self):
        wake = function_body(ROUTE_CONTROL, "mesh_send_route_wake_train")
        loop_at = wake.index("while (k_uptime_get() < close_ms)")
        single_break = wake.index(
            "if (boost_single_shot && sent_count > 0u)", loop_at
        )
        embedded_guard = wake.index("boost_single_shot && embedded_count > 0u", single_break)

        # The one-opportunity break fires before the next claim build and the
        # embedded claim suppresses the duplicate plain claim, so a boosted
        # train emits exactly one WAKE_CLAIM frame per attempt.
        build = wake.index("uwb_clicker_build_wake_claim(", single_break)
        self.assertLess(single_break, build)
        self.assertLess(single_break, embedded_guard)

    def test_fallback_keeps_full_train(self):
        # The non-boost initializer still starts from the full train window
        # and the politeness sniff/backoff machinery stays untouched.
        self.assertIn(
            ".wake_adv_ms = WAKE_ADV_MS,",
            ROUTE_CONTROL,
        )
        wake = function_body(ROUTE_CONTROL, "mesh_send_route_wake_train")
        sniff_pre = wake.index('mesh_route_wake_sniff_activity("pre"')
        sniff_post = wake.index('mesh_route_wake_sniff_activity("post"', sniff_pre)
        backoff = wake.index("mesh_route_wake_backoff(", sniff_post)
        retry = wake.index("goto wake_train_attempt;", backoff)
        self.assertLess(sniff_pre, sniff_post)
        self.assertLess(sniff_post, backoff)
        self.assertLess(backoff, retry)


if __name__ == "__main__":
    unittest.main()
