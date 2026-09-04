#!/usr/bin/env python3
"""Guard collision-free click discovery against hash-slot fallback."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
STATE = (ROOT / "app/src/app_state.c").read_text(encoding="utf-8")
RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class AnchorClickDiscoverySlotSourceInvariants(unittest.TestCase):
    def test_production_anchor_requires_exact_committed_enumeration_slot(self):
        slot = function_body(STATE, "int local_anchor_discovery_slot(")
        production = slot.split("#else", maxsplit=1)[1].split("#endif", maxsplit=1)[0]

        self.assertIn("local_anchor_discovery_assignment_get(", production)
        self.assertIn("assigned_slot >= slot_count", production)
        self.assertIn("return PROTO_ERR_NOT_FOUND;", production)
        self.assertNotIn("uwb_discovery_slot_for_anchor", production)
        self.assertIn("*anchor_slot = assigned_slot;", production)

    def test_invalid_assignment_suppresses_reply_instead_of_hashing(self):
        self.assertIn(
            'DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED '
            'event=%u attempt=%u reply=disabled',
            RADIO,
        )
        self.assertIn('anchor_click_event_abort_if_needed("discovery-slot")', RADIO)
        self.assertNotIn("reply=fallback", RADIO)
        self.assertNotIn("using deterministic fallback", RADIO)

    def test_reply_delay_is_derived_from_the_selected_table_slot(self):
        self.assertIn(
            "local_anchor_discovery_slot(discover.discovery_slot_count,",
            RADIO,
        )
        self.assertIn(
            "sleep_precise_us((uint32_t)reply.anchor_slot * "
            "UWB_DISCOVERY_SLOT_US);",
            RADIO,
        )


if __name__ == "__main__":
    unittest.main()
