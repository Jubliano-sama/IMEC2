#!/usr/bin/env python3
"""Source guards for RF-scope framing at the shared DWM3000 boundary."""

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
DRIVER = read_composed_source(ROOT / "app/src/dwm3000_driver.c")
UWB_HEADER = (ROOT / "include/uwb.h").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing function {name}")

    start = match.start()
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


class UwbRfScopeSourceInvariantTests(unittest.TestCase):
    def test_local_scope_comes_from_physical_role_and_forced_depth(self) -> None:
        local_scope = function_body(DRIVER, "dwm3000_local_rf_scope")

        self.assertIn("UWB_RF_SCOPE_ROLE_GATEWAY", local_scope)
        self.assertIn("UWB_RF_SCOPE_ROLE_ANCHOR", local_scope)
        self.assertIn("UWB_RF_SCOPE_ROLE_CLICKER", local_scope)
        self.assertIn(
            "CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS",
            local_scope,
        )
        self.assertIn(
            "uwb_rf_scope_build(role, forced_relay_hops, scope)",
            local_scope,
        )

        anchor_branch = local_scope.index("#elif DEVICE_ROLE == ROLE_ANCHOR")
        forced_depth = local_scope.index(
            "CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS",
            anchor_branch,
        )
        clicker_branch = local_scope.index("#else", forced_depth)
        self.assertLess(anchor_branch, forced_depth)
        self.assertLess(forced_depth, clicker_branch)

    def test_every_transmit_prefixes_the_current_physical_scope(self) -> None:
        write_frame = function_body(DRIVER, "write_tx_frame")

        build = write_frame.index("dwm3000_local_rf_scope(&local_scope)")
        encode = write_frame.index(
            "uwb_rf_scope_encode(&local_scope, &scope_wire)", build
        )
        scope_write = write_frame.index("dwt_writetxdata(", encode)
        payload_write = write_frame.index("dwt_writetxdata(", scope_write + 1)
        frame_control = write_frame.index("dwt_writetxfctrl(", payload_write)

        self.assertLess(build, encode)
        self.assertLess(encode, scope_write)
        self.assertLess(scope_write, payload_write)
        self.assertLess(payload_write, frame_control)
        self.assertIn(
            "UWB_RF_SCOPE_WIRE_LEN, &scope_wire, 0u",
            write_frame[scope_write:payload_write],
        )
        self.assertIn(
            "(uint8_t *)frame,\n                        UWB_RF_SCOPE_WIRE_LEN",
            write_frame[payload_write:frame_control],
        )
        self.assertIn(
            "UWB_RF_SCOPE_WIRE_LEN + frame_len + FCS_LEN",
            write_frame[frame_control:],
        )
        self.assertIn(
            "max_frame_len - FCS_LEN - UWB_RF_SCOPE_WIRE_LEN",
            write_frame[:scope_write],
        )

    def test_receive_filters_scope_before_exposing_protocol_bytes(self) -> None:
        read_frame = function_body(DRIVER, "read_rx_frame")

        scope_read = read_frame.index("dwt_readrxdata(")
        decode = read_frame.index("uwb_rf_scope_decode(", scope_read)
        visible = read_frame.index("uwb_rf_scope_visible(", decode)
        scope_drop = read_frame.index("driver_stats.rx_scope_drops++", visible)
        unreachable = read_frame.index("return -EHOSTUNREACH", scope_drop)
        payload_read = read_frame.index("dwt_readrxdata(", scope_read + 1)

        self.assertLess(scope_read, decode)
        self.assertLess(decode, visible)
        self.assertLess(visible, scope_drop)
        self.assertLess(scope_drop, unreachable)
        self.assertLess(unreachable, payload_read)
        self.assertIn(
            "&scope_wire, UWB_RF_SCOPE_WIRE_LEN, 0u",
            read_frame[scope_read:decode],
        )
        self.assertIn(
            "raw_frame_len - UWB_RF_SCOPE_WIRE_LEN",
            read_frame[unreachable:payload_read],
        )
        self.assertIn(
            "(uint16_t)frame_len,\n                   UWB_RF_SCOPE_WIRE_LEN",
            read_frame[payload_read:],
        )

    def test_delayed_transmit_patches_stay_after_the_scope_prefix(self) -> None:
        patch_frame = function_body(DRIVER, "patch_tx_frame")

        self.assertIn("offset + UWB_RF_SCOPE_WIRE_LEN", patch_frame)
        self.assertIn(
            "(size_t)offset + length + UWB_RF_SCOPE_WIRE_LEN",
            patch_frame,
        )

    def test_no_raw_dwm_frame_io_bypasses_the_scope_boundary(self) -> None:
        read_frame = function_body(DRIVER, "read_rx_frame")
        write_frame = function_body(DRIVER, "write_tx_frame")
        patch_frame = function_body(DRIVER, "patch_tx_frame")
        outside_boundary = DRIVER

        for body in (read_frame, write_frame, patch_frame):
            outside_boundary = outside_boundary.replace(body, "", 1)

        self.assertNotIn("dwt_readrxdata(", outside_boundary)
        self.assertNotIn("dwt_writetxdata(", outside_boundary)
        self.assertNotIn("dwt_writetxfctrl(", outside_boundary)

    def test_physical_capacity_reserves_the_scope_prefix_byte(self) -> None:
        physical_budget = re.search(
            r"#define UWB_PHY_EXTENDED_PAYLOAD_MAX_LEN\s+\\\n"
            r"(?P<body>.*?)(?=\n#define)",
            UWB_HEADER,
            re.DOTALL,
        )
        self.assertIsNotNone(physical_budget)
        self.assertIn("UWB_RF_SCOPE_WIRE_LEN", physical_budget.group("body"))
        self.assertIn(
            "UWB_PHY_EXTENDED_PAYLOAD_MAX_LEN - UWB_MESH_FRAME_HEADER_LEN",
            UWB_HEADER,
        )


if __name__ == "__main__":
    unittest.main()
