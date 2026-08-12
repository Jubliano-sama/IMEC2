#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
ML = (ROOT / "app/src/app_ml.c").read_text(encoding="utf-8")
ANCHOR_INIT = (ROOT / "app/src/app_anchor_init.inc").read_text(encoding="utf-8")
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text(encoding="utf-8")
ANCHOR_RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text(encoding="utf-8")
CONFIG = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")
MESH_REPORT = (ROOT / "app/src/app_mesh_report.c").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
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


callback_guard = (
    "#if !defined(CONFIG_IMEC_ML_CLICKER) && \\\n"
    "    !defined(CONFIG_IMEC_ML_ANCHOR)\n"
    "        .send_mesh_outbound = app_node_comm_send,\n"
    "#endif"
)
assert callback_guard in MAIN, (
    "ML roles must not retain connected-mesh node communication through "
    "the generic clicker callback table"
)

runtime_guard_start = MAIN.index(
    "#if !defined(CONFIG_IMEC_ML_CLICKER) && \\\n"
    "    !defined(CONFIG_IMEC_ML_ANCHOR)\n"
    "    ret = app_node_comm_init(app_anchor_mesh_report_callbacks());"
)
runtime_guard_end = MAIN.index(
    "\n#endif\n#if !defined(CONFIG_IMEC_ML_CLICKER)\n"
    "    ret = app_anchor_init();",
    runtime_guard_start,
)
runtime_guard = MAIN[runtime_guard_start:runtime_guard_end]
for connected_mesh_init in (
    "app_node_comm_init",
    "app_mesh_test_init",
    "gateway_command_result_tracking_init",
):
    assert connected_mesh_init in runtime_guard, (
        f"{connected_mesh_init} escaped the ML role-isolation guard"
    )
assert "app_anchor_init" not in runtime_guard, (
    "ML anchors still require their role-specific UWB runtime initialization"
)

ml_init_start = ML.index("int app_ml_init(void)")
ml_init = ML[ml_init_start:]
for required_ml_path in (
    "app_clicker_start_work_queue",
    "gateway_ble_init",
    "ml_clicker_collect_work_handler",
):
    assert required_ml_path in ml_init, (
        f"ML role isolation removed required runtime path {required_ml_path}"
    )

ml_anchor_start = function_body(ANCHOR_INIT, "app_ml_anchor_start_role")
for required_anchor_path in (
    "uwb_anchor_session_init",
    "anchor_uwb_scan_work_handler",
    "anchor_click_handoff_work_handler",
    "app_ml_init",
    "k_work_queue_start",
    "anchor_start_uwb_scan",
):
    assert required_anchor_path in ml_anchor_start, (
        f"ML anchor isolation removed required path {required_anchor_path}"
    )
for connected_mesh_owner in (
    "mesh_relay_init",
    "app_mesh_persistence",
    "app_mesh_report_attach",
    "app_anchor_survey_runtime",
    "app_anchor_survey_discovery",
    "mesh_report_wake_active_outbox",
):
    assert connected_mesh_owner not in ml_anchor_start, (
        f"ML anchor retained unused connected-mesh owner {connected_mesh_owner}"
    )

assert re.search(
    r"#if IS_ENABLED\(CONFIG_IMEC_ML_ANCHOR\).*?"
    r"#define MESH_RX_QUEUE_DEPTH 1",
    CONFIG,
    re.DOTALL,
)
assert (
    "#if DEVICE_ROLE == ROLE_ANCHOR && IS_ENABLED(CONFIG_IMEC_ML_ANCHOR)\n"
    "#define REPORT_TX_QUEUE_DEPTH 1"
) in CONFIG
assert (
    "#if defined(CONFIG_IMEC_ML_ANCHOR)\n"
    "#define MESH_CH9_TX_BATCH_MAX 1u"
) in MESH_REPORT
assert (
    "#if DEVICE_ROLE == ROLE_ANCHOR && !defined(CONFIG_IMEC_ML_ANCHOR)"
) in ANCHOR

for required_anchor_output in (
    "gateway_ble_enter_uwb_quiet",
    "gateway_ble_exit_uwb_quiet",
    "ml_anchor_run_post_burst_diagnostics",
    "anchor_run_clicker_pair_survey",
):
    assert required_anchor_output in ANCHOR_RADIO, (
        f"ML anchor isolation removed UWB/BLE output {required_anchor_output}"
    )

pair_runner = function_body(ANCHOR_RADIO, "anchor_run_clicker_pair_survey")
pair_range = pair_runner.index("dwm3000_driver_range_initiator")
pair_result = pair_runner.index("anchor_send_pair_survey_result", pair_range)
pair_low_power = pair_runner.index(
    'anchor_enter_low_power(APP_RADIO_LOW_POWER_IDLE,\n'
    '                                                   "pair-survey-initiator")',
    pair_result,
)
assert pair_range < pair_result < pair_low_power

ml_collect = function_body(ML, "ml_clicker_collect_work_handler")
assert "atomic_clear(&ml_clicker_busy)" not in ml_collect, (
    "ML request ownership must outlive the running collection work item"
)
assert ml_collect.count("ml_clicker_finish_request(") == 3, (
    "every ML collection terminal path must defer request-owner release"
)
pair_survey = ml_collect.index("ml_clicker_run_anchor_pair_survey")
normal_collection = ml_collect.index(
    "app_clicker_collect_uwb_attempt_with_options_until", pair_survey
)
normal_range_guard = ml_collect.index(
    "if (ret == 0 && !request.anchor_pair_survey)", normal_collection
)
normal_range = ml_collect.index(
    "app_clicker_range_scheduled_anchors", normal_range_guard
)
pair_results = ml_collect.index(
    "ml_clicker_emit_stored_anchor_pair_results", normal_range
)
assert pair_survey < normal_collection < normal_range_guard < normal_range < pair_results

live_match = function_body(ML, "ml_clicker_live_control_matches_active")
assert "request->command.src_id" in live_match
assert "ml_clicker_live_control_owner.src_id" in live_match
assert "request->command.session_id" in live_match
assert "ml_clicker_live_control_owner.session_id" in live_match
assert "k_spin_lock(&ml_clicker_live_control_lock)" in live_match
assert "k_spin_unlock(&ml_clicker_live_control_lock" in live_match

live_run = function_body(ML, "ml_clicker_run_live_tracking")
live_publish = live_run.index("ml_clicker_live_control_publish(request)")
live_loop = live_run.index("while (true)", live_publish)
live_clear = live_run.index("ml_clicker_live_control_clear()", live_loop)
assert live_publish < live_loop < live_clear

ml_finish = function_body(ML, "ml_clicker_finish_request")
terminal_result = ml_finish.index("ml_clicker_send_command_result")
deferred_release = ml_finish.index(
    "app_clicker_submit_work(&ml_clicker_release_work)", terminal_result
)
fail_closed = ml_finish.index("k_panic()", deferred_release)
assert terminal_result < deferred_release < fail_closed

ml_release = function_body(ML, "ml_clicker_release_work_handler")
assert "atomic_clear(&ml_clicker_busy)" in ml_release

cache_admission = function_body(ML, "ml_clicker_cache_entry_for_discovery")
for required_cache_replacement in (
    "MAX(entry->last_found_ms, entry->last_ranged_ms)",
    "activity_ms < oldest_activity_ms",
    "memset(oldest, 0, sizeof(*oldest))",
):
    assert required_cache_replacement in cache_admission, (
        "a full ML cache must replace its least-recently-active entry"
    )

cache_range_note = function_body(ML, "ml_clicker_cache_note_range_result")
assert "ml_clicker_cache_find(anchor_id)" in cache_range_note
assert "ml_clicker_cache_entry_for_discovery" not in cache_range_note, (
    "a range result must not allocate or evict without discovery metadata"
)

release_init = ml_init.index(
    "k_work_init(&ml_clicker_release_work, ml_clicker_release_work_handler)"
)
ble_init = ml_init.index("gateway_ble_init()", release_init)
assert release_init < ble_init, (
    "the owner-release item must exist before BLE can dispatch a request"
)
assert "return ret;" in ml_init[ble_init:], (
    "ML clicker command ingress failure must fail role initialization"
)

ble_handler = function_body(ML, "ml_clicker_handle_ble_frame")
live_control_branch = ble_handler.index(
    "command_id == CMD_ML_LIVE_TRACKING_HEARTBEAT"
)
live_owner_check = ble_handler.index(
    "ml_clicker_live_control_matches_active(&request)", live_control_branch
)
live_touch = ble_handler.index("ml_clicker_live_tracking_touch()", live_owner_check)
live_stop = ble_handler.index(
    "atomic_set(&ml_clicker_live_stop_requested, 1)", live_touch
)
live_denied = ble_handler.index("live_status = COMMAND_DENIED", live_stop)
assert live_control_branch < live_owner_check < live_touch < live_stop < live_denied

print("ML role isolation source invariants passed")
