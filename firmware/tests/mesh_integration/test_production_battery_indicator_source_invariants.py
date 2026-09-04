#!/usr/bin/env python3
"""Keep production battery LEDs separate from bench RF activity lights."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP_CMAKE = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
BOARD = (ROOT / "app" / "src" / "app_board.c").read_text(encoding="utf-8")
CLICKER = (ROOT / "app" / "src" / "app_clicker.c").read_text(encoding="utf-8")
INDICATOR = (ROOT / "app" / "src" / "app_battery_indicator.c").read_text(
    encoding="utf-8"
)
WATCHDOG = (ROOT / "app" / "src" / "app_watchdog.c").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    marker = f"{name}("
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


assert (
    'if(IMEC_DEPLOYABLE_MESH_PRESET)\n'
    '        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"\n'
    '            "CONFIG_IMEC_PRODUCTION_STATUS_LEDS=y\\n")'
) in APP_CMAKE, "only deployable mesh presets may select production LED policy"

assert (
    'if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor" OR\n'
    '       IMEC_BUILD_PRESET STREQUAL "mesh_clicker")'
) in APP_CMAKE, "only exact production battery roles may select periodic pulses"
battery_selector_start = APP_CMAKE.index(
    'if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor" OR\n'
    '       IMEC_BUILD_PRESET STREQUAL "mesh_clicker")'
)
battery_selector_end = APP_CMAKE.index(
    'if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")',
    battery_selector_start,
)
battery_selector = APP_CMAKE[battery_selector_start:battery_selector_end]
assert "mesh_anchor_forcedhop" not in battery_selector
assert "CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR=y" in battery_selector
assert (
    "target_sources_ifdef(CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR app PRIVATE"
    in APP_CMAKE
), "battery worker state must not link into gateway or test presets"

activity_gate = function_body(BOARD, "mesh_route_activity_leds_enabled")
assert "!IS_ENABLED(CONFIG_IMEC_PRODUCTION_STATUS_LEDS)" in activity_gate

power_indicator = function_body(BOARD, "status_power_indicator_set")
assert "production_anchor_battery_indicator_enabled()" in power_indicator
assert "enabled = false;" in power_indicator

assert "#define BATTERY_INDICATOR_LED_ON_MS 50u" in INDICATOR
assert "#define BATTERY_INDICATOR_ANCHOR_PERIOD_MS 5000u" in INDICATOR
assert "#define BATTERY_INDICATOR_CLICKER_PERIOD_MS 10000u" in INDICATOR
assert "band == BATTERY_STATUS_LOW" in INDICATOR
assert "band == BATTERY_STATUS_HIGH" in INDICATOR
assert "band == BATTERY_STATUS_MIDDLE" in INDICATOR
assert "battery_status_clicker_band(battery_mv)" in INDICATOR

indicator_handler = function_body(INDICATOR, "battery_indicator_work_handler")
assert indicator_handler.index("if (battery_indicator_led_on)") < \
    indicator_handler.index("app_watchdog_clicker_idle_checkpoint()") < \
    indicator_handler.index("battery_sample_lithium_mv"), \
    "the clicker watchdog must share the existing LED-on wake, even if ADC sampling fails"
assert (
    "BATTERY_INDICATOR_CLICKER_PERIOD_MS <\n"
    "                 APP_WATCHDOG_HARDWARE_TIMEOUT_MS"
) in INDICATOR, "the coalesced battery/watchdog interval needs a build-time bound"

coalesced = function_body(WATCHDOG, "clicker_idle_watchdog_coalesced")
assert "DEVICE_ROLE == ROLE_CLICKER" in coalesced
assert "CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR" in coalesced
monitor_start = function_body(WATCHDOG, "start_watchdog_health_monitor")
assert monitor_start.index("clicker_idle_watchdog_coalesced()") < \
    monitor_start.index("k_work_init_delayable"), \
    "production clicker must skip the one-second work and timer owners"
checkpoint = function_body(WATCHDOG, "app_watchdog_clicker_idle_checkpoint")
assert checkpoint.index("atomic_set(&system_progress_ms") < \
    checkpoint.index("watchdog_timer_handler(NULL)"), \
    "the shared battery wake must refresh progress before feeding"

action = function_body(CLICKER, "app_clicker_handle_button_action")
assert action.index("app_battery_indicator_suspend()") < action.index(
    "clicker_connect_status_leds_for_action()"
), "click/self-test feedback must take LED ownership before reconnecting pins"

retained_idle = function_body(CLICKER, "clicker_enter_systemon_retained_idle")
assert retained_idle.index("status_leds_disconnect()") < retained_idle.index(
    "app_battery_indicator_resume()"
), "the low-battery timer starts only after retained-idle pin parking"

systemoff_idle = function_body(CLICKER, "app_clicker_enter_systemoff_idle")
assert "app_battery_indicator_suspend()" in systemoff_idle

print("production battery indicator source invariants passed")
