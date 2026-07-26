#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
BLE = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
SURVEY = (ROOT / "app/src/app_gateway_survey_observability.c").read_text(
    encoding="utf-8"
)


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


admit = function_body(ANCHOR, "gateway_host_command_admit")
assert "k_msgq_get" not in admit
assert admit.index("gateway_observe_host_acceptance") < admit.index("k_msgq_put")

survey_work = function_body(ANCHOR, "gateway_survey_work_handler")
boundary_gate = survey_work.index(
    "if (!gateway_survey_flush_boundary_event())"
)
boundary_flush = survey_work.index(
    "gateway_survey_flush_boundary_event", boundary_gate
)
active_gate = survey_work.index("if (!gateway_survey_operation_active())")
deadline_gate = survey_work.index("if ((uint64_t)k_uptime_get() >=")
deadline_api = survey_work.index(
    "gateway_survey_machine_operation_deadline_ms(", deadline_gate
)
deadline_exit = survey_work.index("goto out;", deadline_gate)
deadline_close = survey_work.index("}", deadline_exit) + 1
pair_finalize = survey_work.index("gateway_survey_finalize_pair_observation")
assert deadline_gate < deadline_api < deadline_exit
assert active_gate < deadline_gate < boundary_flush, (
    "pending boundaries must flush at the first safe point after the active "
    "survey and operation-deadline checks"
)
assert not survey_work[deadline_close:boundary_gate].strip(), (
    "the boundary flush must immediately follow the operation-deadline gate"
)
for external_wait_gate in (
    "gateway_survey_wait_for_discovery_collection()",
    "gateway_survey_cleanup_pending()",
    "survey_gateway_response_ack_settle_pending(",
    "if (gateway_survey_auto.waiting)",
    "gateway_survey_round_drive()",
):
    assert boundary_flush < survey_work.index(external_wait_gate), (
        "a backpressured pair-start boundary must be flushed before external "
        f"wait gate {external_wait_gate}"
    )
assert boundary_flush < pair_finalize, (
    "a pair result must not be finalized before its pending pair-start "
    "boundary has custody"
)
assert boundary_flush < survey_work.index("survey_gateway_auto_next_action")
assert pair_finalize < survey_work.index(
    "survey_gateway_auto_next_action"
)

report_ingress = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
assert "GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED" not in report_ingress
assert "reports[state->report_cursor]" in SURVEY
assert "GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED" in SURVEY
assert "GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY" in SURVEY

ble_work = function_body(BLE, "gateway_ble_stream_work_handler")
assert "if (gateway_ble_tx_in_flight)" in ble_work
assert "credit_available = 0u" in ble_work
assert "ret = bt_gatt_notify_cb" in ble_work
assert "else if (source == GATEWAY_BLE_TX_STREAM)" in ble_work
assert "successful async submit consumes the controller credit" in ble_work
assert "app_stack_workload_diag_ble_sample_with_pressure" in ble_work
assert "gateway_ble_schedule_stream_retry();" in ble_work
assert "bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN)" not in ble_work

ble_retry = function_body(BLE, "gateway_ble_stream_retry_delay_ms")
assert "GATEWAY_BLE_TX_RETRY_MS << shift" in ble_retry
assert "GATEWAY_BLE_TX_RETRY_MAX_MS" in ble_retry

ble_init = function_body(BLE, "gateway_ble_init")
assert ble_init.count("return ret;") >= 2
assert "recovery remains active" in ble_init
for marker in (
    "DBG_GATEWAY_BLE stage=entry",
    "DBG_GATEWAY_BLE stage=bt_enable",
    "DBG_GATEWAY_BLE stage=adv_start",
    "DBG_GATEWAY_BLE event=recovery",
    "DBG_GATEWAY_BLE event=connected",
    "DBG_GATEWAY_BLE event=disconnected",
):
    assert marker in BLE
for marker in (
    "DBG_NODE_COMM_BOOT stage=init",
    "DBG_GATEWAY_BOOT stage=ble_begin",
    "DBG_GATEWAY_BOOT stage=ble_done",
    "DBG_GATEWAY_BOOT stage=ch9_begin",
    "DBG_GATEWAY_BOOT stage=ch9_done",
):
    assert marker in MAIN

print("gateway observability source invariants passed")
