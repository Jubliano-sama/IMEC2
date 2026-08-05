#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
BLE = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
ML = (ROOT / "app/src/app_ml.c").read_text(encoding="utf-8")


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


select_frame = function_body(BLE, "gateway_ble_tx_select_frame_locked")
assert "gateway_ble_direct_queue_begin" in select_frame
assert "gateway_ble_direct_queue_complete" not in select_frame

complete = function_body(BLE, "gateway_ble_tx_complete")
assert "gateway_ble_direct_queue_complete" in complete
assert complete.index("gateway_ble_direct_queue_complete") < complete.index(
    "gateway_ble_tx_source = GATEWAY_BLE_TX_NONE"
)

reset = function_body(BLE, "gateway_ble_tx_reset_locked")
assert "gateway_ble_direct_queue_cancel" in reset
assert "gateway_ble_direct_queue_init" not in reset

disconnect = function_body(BLE, "gateway_ble_disconnected")
assert "gateway_ble_tx_reset_locked" in disconnect
assert "gateway_ble_direct_queue_init" not in disconnect

assert "atomic_get(&gateway_host_journal_restore_pending) != 0" in BLE
assert "packet_class == GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC" in BLE
assert "packet_class == GATEWAY_BLE_STREAM_CLASS_STATUS" in BLE

restore_host = function_body(BLE, "gateway_restore_host_journal_runtime")
restore_pressure_gate = restore_host[
    restore_host.index("gateway_ble_stream_state.reservation_active") :
    restore_host.index("gateway_ble_stream_state.restore_staging_active = true")
]
assert "gateway_ble_stream_state.count >=" not in restore_pressure_gate
assert "struct proto_packet staging_packet = {0};" in restore_host
assert "struct proto_packet *packet = &staging_packet;" in restore_host
assert "packet = &gateway_ble_stream_state.items" not in restore_host
assert "gateway_ble_stream_enqueue_staged_packet" in restore_host

queue_frame = function_body(BLE, "gateway_ble_queue_frame")
assert "k_msgq_put" in queue_frame
assert "gateway_ble_resume_rx" in queue_frame
assert "(void)k_work_submit(&gateway_ble_rx_work)" not in queue_frame

for name, work_call, owner in (
    ("gateway_ble_resume_rx", "k_work_submit", '"rx"'),
    ("gateway_ble_schedule_recovery", "k_work_reschedule", '"recovery"'),
    ("gateway_ble_schedule_stream_drain", "k_work_reschedule", '"stream-drain"'),
    ("gateway_ble_schedule_stream_retry", "k_work_reschedule", '"stream-retry"'),
):
    body = function_body(BLE, name)
    assert f"ret = {work_call}" in body
    assert "gateway_ble_work_handoff_requires_reset(ret)" in body
    assert f"gateway_ble_schedule_failed({owner}, ret)" in body

schedule_failure = function_body(BLE, "gateway_ble_schedule_failed")
assert "app_watchdog_stop_feeding" in schedule_failure

ml_flush = function_body(ML, "ml_clicker_flush_buffered_frames")
assert "ml_clicker_runtime.buffered_sample_head++" in ml_flush
assert "ml_clicker_runtime.buffered_frame_head++" in ml_flush
assert "ml_clicker_send_encoded_frame_retained" in ml_flush
assert "continue;" not in ml_flush

ml_terminal = function_body(ML, "ml_clicker_finish_request")
assert "ML terminal result withheld" in ml_terminal
assert "ml_clicker_send_command_result_with_custody" in ml_terminal
assert "app_watchdog_stop_feeding" in ml_terminal

print("gateway BLE and ML custody source invariants passed")
