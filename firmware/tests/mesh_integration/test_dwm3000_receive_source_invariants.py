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
responder = function_body("responder_poll_once")

assert read_frame.count("dwt_read32bitreg(RX_FINFO_ID)") == 1
assert "last_rx_finfo_register = rx_finfo" in read_frame
assert "dwt_read32bitreg(RX_FINFO_ID)" not in receive
assert "ret == 0 ? last_rx_finfo_register : 0u" in receive
assert "unbounded diagnostic SPI transaction" in receive

poll_receive = responder.index("ret = receive_frame(")
poll_failure = responder.index("if (ret < 0)", poll_receive)
poll_timeout_status = responder.index(
    "result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT", poll_failure
)
poll_timeout_return = responder.index("if (ret == -ETIMEDOUT)", poll_failure)
exchange_started = responder.index("result->exchange_started = true")
assert poll_failure < poll_timeout_status < poll_timeout_return < exchange_started, (
    "a pre-POLL receive timeout must be classified before returning and before "
    "the exchange is marked started"
)
assert "ret == -EMSGSIZE ? RANGE_BAD_FRAME" in responder[
    poll_failure:poll_timeout_return
], "a malformed pre-POLL frame must remain distinguishable from an RX error"
assert "RANGE_RX_ERROR" in responder[poll_failure:poll_timeout_return], (
    "a non-timeout pre-POLL receive failure must be reported as an RX error"
)

print("DWM3000 receive source invariants passed")
