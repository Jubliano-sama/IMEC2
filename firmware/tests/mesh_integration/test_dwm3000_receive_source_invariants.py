#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
DRIVER = (ROOT / "app/src/dwm3000_driver.c").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", DRIVER, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = DRIVER.index("{", match.start())
    depth = 0
    for index in range(brace, len(DRIVER)):
        depth += DRIVER[index] == "{"
        depth -= DRIVER[index] == "}"
        if depth == 0:
            return DRIVER[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


read_frame = function_body("read_rx_frame")
receive = function_body("receive_frame_with_preamble_timeout")

assert read_frame.count("dwt_read32bitreg(RX_FINFO_ID)") == 1
assert "last_rx_finfo_register = rx_finfo" in read_frame
assert "dwt_read32bitreg(RX_FINFO_ID)" not in receive
assert "ret == 0 ? last_rx_finfo_register : 0u" in receive
assert "unbounded diagnostic SPI transaction" in receive

print("DWM3000 receive source invariants passed")
