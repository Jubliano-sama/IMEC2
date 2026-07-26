#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
BLE = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")
OWNER = (ROOT / "app/src/app_mesh_route_owner_queue.c").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    match = None
    brace = None
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth == 0:
                next_index = index + 1
                while source[next_index].isspace():
                    next_index += 1
                if source[next_index] == "{":
                    match = candidate
                    brace = next_index
                break
        if match is not None:
            break
    assert match is not None and brace is not None, f"missing function {name}"
    line_start = source.rfind("\n", 0, match.start()) + 1
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[line_start : index + 1]
    raise AssertionError(f"unterminated function {name}")


def braced_block(source: str, open_brace: int) -> str:
    depth = 0
    for index in range(open_brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[open_brace : index + 1]
    raise AssertionError("unterminated braced block")


queue_owner = function_body(OWNER, "mesh_route_owner_work_queue")
assert "CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE" in queue_owner
assert "return &mesh_route_work_q;" in queue_owner
assert "return NULL;" in queue_owner, (
    "the queue accessor must preserve the legacy system-workqueue fallback"
)

reschedule_owner = function_body(OWNER, "mesh_route_owner_work_reschedule")
assert "mesh_transport_paused" not in reschedule_owner, (
    "state-owner work must remain schedulable while ordinary mesh transport is "
    "paused, so survey cleanup and abort can still make progress"
)
assert re.search(
    r"struct k_work_q \*queue\s*=\s*mesh_route_owner_work_queue\(\).*?"
    r"return queue == NULL \? k_work_reschedule\s*\(\s*work\s*,\s*delay\s*\)"
    r"\s*:\s*k_work_reschedule_for_queue\s*\(\s*queue\s*,\s*work\s*,\s*"
    r"delay\s*\)",
    reschedule_owner,
    re.DOTALL,
), "delayable state-owner work must use mesh_route with a legacy queue fallback"

submit_owner = function_body(OWNER, "mesh_route_owner_work_submit")
assert "mesh_transport_paused" not in submit_owner, (
    "preemptive state-owner work must remain submit-capable while ordinary mesh "
    "transport is paused"
)
assert re.search(
    r"struct k_work_q \*queue\s*=\s*mesh_route_owner_work_queue\(\).*?"
    r"return queue == NULL \? k_work_submit\s*\(\s*work\s*\)"
    r"\s*:\s*k_work_submit_to_queue\s*\(\s*queue\s*,\s*work\s*\)",
    submit_owner,
    re.DOTALL,
), "immediate state-owner work must use mesh_route with a legacy queue fallback"

owned_work = {
    "gateway_survey_work": (ANCHOR, "mesh_route_owner_work_reschedule"),
    "gateway_command_result_timeout_work": (
        BLE,
        "mesh_route_owner_work_reschedule",
    ),
    "gateway_host_command_retry_work": (
        ANCHOR,
        "mesh_route_owner_work_reschedule",
    ),
    "gateway_host_abort_work": (ANCHOR, "mesh_route_owner_work_submit"),
}
for work_name, (source, owner_api) in owned_work.items():
    assert re.search(
        rf"\b{owner_api}\s*\(\s*&{work_name}\b",
        source,
    ), f"{work_name} must have an explicit mesh-route owner schedule edge"
    for plain_api in ("k_work_reschedule", "k_work_submit"):
        assert not re.search(
            rf"\b{plain_api}\s*\(\s*&{work_name}\b",
            source,
        ), f"{work_name} must never fall back to the system workqueue"

preemptive_submit = function_body(
    ANCHOR, "gateway_host_command_submit_preemptive"
)
abort_request = preemptive_submit.index(
    "dwm3000_driver_request_receive_abort()"
)
owner_submit = preemptive_submit.index(
    "mesh_route_owner_work_submit(&gateway_host_abort_work)"
)
assert abort_request < owner_submit, (
    "the preemptive abort must interrupt active RX before queueing its mesh-route "
    "state transition"
)
submit_failure = preemptive_submit.index("if (ret < 0)", owner_submit)
failure_open = preemptive_submit.index("{", submit_failure)
failure_body = braced_block(preemptive_submit, failure_open)
clear_failure = failure_body.index("dwm3000_driver_clear_receive_abort()")
release_failure = failure_body.index("k_msgq_get(")
assert clear_failure < release_failure < failure_body.index("return ret;"), (
    "a failed owner submit must clear the RX-abort request before releasing "
    "preemptive command custody"
)

abort_handler = function_body(ANCHOR, "gateway_host_abort_work_handler")
assert abort_handler.index("dwm3000_driver_clear_receive_abort()") < (
    abort_handler.index("while (k_msgq_get(")
), "the mesh-route abort handler must consume the RX-abort request on entry"

print("gateway survey owner-queue source invariants passed")
