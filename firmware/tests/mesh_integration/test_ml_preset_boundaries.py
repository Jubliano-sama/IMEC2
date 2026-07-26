#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app"


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


class MlPresetBoundaryTests(unittest.TestCase):
    def test_ml_clicker_retains_bounded_ble_host_transport(self):
        config = (APP / "conf" / "ml-clicker.conf").read_text()

        self.assertIn("CONFIG_IMEC_GATEWAY_BLE=y", config)
        self.assertIn("CONFIG_BT_BUF_ACL_TX_COUNT=4", config)
        self.assertIn("CONFIG_LOG_BUFFER_SIZE=2048", config)

    def test_ml_anchor_does_not_restore_removed_ble_debug_transport(self):
        config = (APP / "conf" / "ml-anchor.conf").read_text()
        state = (APP / "src" / "app_state.c").read_text()
        ml = (APP / "src" / "app_ml.c").read_text()
        transport = function_body(state, "gateway_ble_transport_enabled")
        init = function_body(ml, "app_ml_init")
        anchor_start = init.index("#if defined(CONFIG_IMEC_ML_ANCHOR)")
        anchor_end = init.index("#endif", anchor_start)
        anchor_init = init[anchor_start:anchor_end]

        self.assertNotIn("CONFIG_BT=y", config)
        self.assertNotIn("CONFIG_BT_PERIPHERAL=y", config)
        self.assertNotIn("CONFIG_IMEC_GATEWAY_BLE=y", config)
        self.assertNotIn("CONFIG_IMEC_ML_ANCHOR", transport)
        self.assertNotIn("gateway_ble_init", anchor_init)
        self.assertNotIn("BLE debug", anchor_init)
        self.assertIn("UWB collection enabled", anchor_init)


if __name__ == "__main__":
    unittest.main()
