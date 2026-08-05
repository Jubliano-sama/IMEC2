#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CLICKER = (ROOT / "app/src/app_clicker.c").read_text(encoding="utf-8")
BOARD = (ROOT / "app/src/app_board.c").read_text(encoding="utf-8")


def function_body(name: str, source: str = CLICKER) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


def braced_statement(source: str, statement_start: int) -> tuple[str, int]:
    brace = source.index("{", statement_start)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[statement_start : index + 1], index + 1
    raise AssertionError("unterminated braced statement")


def top_level_index(source: str, needle: str, body_depth: int = 1) -> int:
    depth = 0
    cursor = 0
    while True:
        index = source.find(needle, cursor)
        if index < 0:
            return -1
        for char in source[cursor:index]:
            depth += char == "{"
            depth -= char == "}"
        if depth == body_depth:
            return index
        cursor = index + len(needle)


idle = function_body("clicker_enter_systemon_retained_idle")
standby = idle.index("ret = clicker_enter_radio_retained_standby()")
wake_arm = idle.index("clicker_arm_retained_idle_wake()", standby)
idle_telemetry = idle.index('"CLICKER_IDLE"', wake_arm)
led_disconnect = idle.index("status_leds_disconnect()", idle_telemetry)

failure_guard_match = re.search(
    r"\bif\s*\(\s*ret\s*<\s*0\s*\)\s*\{",
    idle[standby:wake_arm],
)
assert failure_guard_match is not None, (
    "retained-standby failure must be handled before the click IRQ is armed"
)
failure_guard = standby + failure_guard_match.start()
failure_branch, failure_branch_end = braced_statement(idle, failure_guard)

recovery_reset = top_level_index(
    failure_branch, "click_button_recovery_reset("
)
failure_return = top_level_index(failure_branch, "return;")
assert 0 <= recovery_reset < failure_return, (
    "retained-standby failure must take the cold-reset recovery path and "
    "return before idle can be reported as successful"
)
assert re.search(
    r"click_button_recovery_reset\s*\([^;]*,\s*ret\s*\)\s*;",
    failure_branch,
    re.DOTALL,
), "retained-idle recovery must preserve the standby failure status"

for forbidden in (
    "click_button_arm_idle_interrupt(",
    '"CLICKER_IDLE"',
    "status_leds_disconnect(",
):
    assert forbidden not in failure_branch, (
        f"retained-standby failure branch must not reach idle success: "
        f"{forbidden}"
    )

pins_float = idle.index("dwm3000_port_float_pins()", failure_branch_end)
assert (
    standby
    < failure_guard
    < failure_branch_end
    < pins_float
    < wake_arm
    < idle_telemetry
    < led_disconnect
), (
    "radio retention must be proven before IRQ arm, CLICKER_IDLE telemetry, "
    "and LED disconnect"
)

wake_helper = function_body("clicker_arm_retained_idle_wake")
assert "click_button_arm_idle_interrupt()" in wake_helper, (
    "the retained-idle wake helper must remain the physical IRQ-arm boundary"
)

reset_helper = function_body("click_button_recovery_reset")
feed_stop = reset_helper.index("app_watchdog_stop_feeding()")
reboot_delay = reset_helper.index(
    "k_msleep(CLICK_BUTTON_RECOVERY_REBOOT_DELAY_MS)"
)
cold_reboot = reset_helper.index("sys_reboot(SYS_REBOOT_COLD)")
assert feed_stop < reboot_delay < cold_reboot, (
    "fatal clicker recovery must stop watchdog feeds and wait before reboot "
    "so a persistent low-power fault cannot create a tight reboot loop"
)

connect_helper = function_body("clicker_connect_status_leds_for_action")
assert re.search(
    r"#define\s+CLICKER_STATUS_LED_CONNECT_ATTEMPTS\s+2u\b", CLICKER
), "LED reconnect retries must remain explicitly bounded"
retained_gate = connect_helper.index(
    "if (!clicker_systemon_retained_idle_enabled())"
)
connect_loop = connect_helper.index(
    "attempt < CLICKER_STATUS_LED_CONNECT_ATTEMPTS", retained_gate
)
connect_call = connect_helper.index("status_leds_connect()", connect_loop)
retry_wait = connect_helper.index(
    "k_busy_wait(CLICKER_STATUS_LED_CONNECT_RETRY_US)", connect_call
)
assert retained_gate < connect_loop < connect_call < retry_wait, (
    "retained-idle LED restore must use the bounded action-only reconnect "
    "path"
)

action_handler = function_body("app_clicker_handle_button_action")
action_reconnect = action_handler.index(
    "clicker_connect_status_leds_for_action()"
)
action_switch = action_handler.index("switch (action)", action_reconnect)
assert action_reconnect < action_switch, (
    "every retained-idle action must reconnect LED GPIO before any status "
    "pattern can be applied"
)

terminal_hold = function_body("clicker_hold_terminal_status")
product_hold = terminal_hold.index(
    "k_msleep(STATUS_PASS_DURATION_MS)"
)
assert product_hold >= 0, (
    "the production hold must retain its terminal result"
)

normal_case = action_handler[
    action_handler.index("case BUTTON_ACTION_NORMAL_CLICK:") :
    action_handler.index("case BUTTON_ACTION_SELF_TEST_ARMED:")
]
assert normal_case.count("clicker_hold_terminal_status(ret)") >= 1, (
    "the production normal-click exit must visibly retain its terminal result"
)
self_test_case = action_handler[
    action_handler.index("case BUTTON_ACTION_SELF_TEST_START:") :
    action_handler.index("case BUTTON_ACTION_SELF_TEST_CANCELLED:")
]
self_test_status = self_test_case.index("status_apply(&status)")
self_test_hold = self_test_case.index(
    "clicker_hold_terminal_status(", self_test_status
)
assert self_test_status < self_test_hold, (
    "self-test terminal status must be held before retained idle disconnects "
    "the LEDs"
)

action_worker = function_body("clicker_action_work_handler")
action_run = action_worker.index("app_clicker_handle_button_action(action)")
idle_transition = action_worker.index("app_clicker_enter_idle()", action_run)
assert action_run < idle_transition, (
    "the complete action and terminal LED hold must precede low-power "
    "disconnect"
)

board_connect = function_body("status_leds_connect", BOARD)
assert "configure_output(" in board_connect
assert "k_work_init" not in board_connect, (
    "waking a clicker must reconnect GPIO without reinitializing live work "
    "objects"
)
board_init = function_body("status_leds_init", BOARD)
assert "return status_leds_connect();" in board_init, (
    "cold initialization and retained-idle reconnection must share the same "
    "GPIO configuration boundary"
)

print("clicker retained-idle failure source invariants passed")
