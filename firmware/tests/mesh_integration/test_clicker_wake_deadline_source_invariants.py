#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CLICKER = (ROOT / "app/src/app_clicker.c").read_text()
RADIO_RECOVERY = (
    ROOT / "app/src/app_radio_recovery.c"
).read_text()
MAIN = (ROOT / "app/src/main.c").read_text()
APP_CONFIG = (ROOT / "app/src/app_config.h").read_text()
DRIVER_IO = (ROOT / "app/src/dwm3000_driver_io.inc").read_text()
DRIVER_RADIO = (ROOT / "app/src/dwm3000_driver_radio.inc").read_text()
MESH_ARBITRATION = (
    ROOT / "app/src/app_mesh_arbitration_zephyr.c"
).read_text()
NODE_COMM = (ROOT / "app/src/app_node_comm.c").read_text()
MESH_COORDINATION = (
    ROOT / "app/src/app_mesh_report_coordination.inc"
).read_text()
MESH_TRANSPORT = (
    ROOT / "app/src/app_mesh_report_transport.inc"
).read_text()
GATEWAY_CONTROL = (
    ROOT / "app/src/app_anchor_gateway_control.inc"
).read_text()
ANCHOR_INIT = (ROOT / "app/src/app_anchor_init.inc").read_text()
POLICY_TEST = (ROOT / "tests/test_app_wake_train_politeness.c").read_text()


def source_function_body(source: str, name: str) -> str:
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


def braced_statement_end(source: str, statement_start: int) -> int:
    brace = source.index("{", statement_start)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return index + 1
    raise AssertionError("unterminated braced statement")


def function_body(name: str) -> str:
    return source_function_body(CLICKER, name)


# Every clicker owner releases the exact generation in two phases.  A stale
# callback must return before it touches the hardware, while a physical park
# failure must reach release_finish so the guard can poison and retain it.
assert "radio_guard_uwb_start" not in CLICKER
assert "radio_guard_uwb_stop" not in CLICKER
for claim in re.finditer(r"radio_guard_uwb_claim\s*\(", CLICKER):
    claim_end = CLICKER.index(");", claim.start())
    assert "RADIO_GUARD_UWB_CLIENT_CLICKER" in CLICKER[
        claim.start():claim_end
    ]

for release_helper, park_call in (
    (
        "clicker_release_radio_to_standby",
        "app_radio_standby_with_bounded_recovery",
    ),
    (
        "clicker_release_radio_to_idle",
        "app_radio_idle_with_bounded_recovery",
    ),
):
    release = function_body(release_helper)
    begin = release.index("radio_guard_uwb_release_begin")
    stale_failure = release.index("if (release_ret < 0)", begin)
    stale_failure_end = braced_statement_end(release, stale_failure)
    parking = release.index(park_call, stale_failure_end)
    finish = release.index(
        "radio_guard_uwb_release_finish(radio_lease, parking_ret)",
        parking,
    )
    assert begin < stale_failure < stale_failure_end < parking < finish
    assert "return release_ret;" in release[stale_failure:stale_failure_end]
    assert park_call not in release[stale_failure:stale_failure_end]


wake = function_body("clicker_send_wake_claim_train_until")
tail = function_body("app_clicker_wake_train_opportunity_tail_ms")
assert "post_wake_claimed_duration_ms" in tail
# The clicker advertises every discovery slot on the wire.  Its absolute
# wake-claim epoch must therefore reserve the full 50-slot reply window, not
# merely the four anchors eventually selected for ranging.
discovery_window = re.search(
    r"#define UWB_MAX_DISCOVERY_WINDOW_MS\s+\\\s*\n\s*"
    r"\(\(\((.*?)\) \+ 999u\) / 1000u\)",
    APP_CONFIG,
    re.DOTALL,
)
assert discovery_window is not None
assert "UWB_DISCOVERY_SLOT_COUNT * UWB_DISCOVERY_SLOT_US" in discovery_window.group(1)
assert "MAX_SCHEDULED_ANCHORS" not in discovery_window.group(1)
admission = wake.index("app_wake_train_deadline_fits")
radio = wake.index("radio_guard_uwb_claim", admission)
pre_sniff = wake.index("clicker_wake_train_sniff_activity", radio)
post_configure_deadline = wake.index(
    "app_wake_train_deadline_fits", pre_sniff
)
post_sniff = wake.index(
    'clicker_wake_train_sniff_activity("post"', post_configure_deadline
)
backoff = wake.index("clicker_wake_train_backoff(", post_sniff)
assert admission < radio < pre_sniff < post_configure_deadline < post_sniff < backoff
wake_release = wake.index("clicker_release_radio_to_standby", post_sniff)
wake_release_failure = wake.index("if (release_ret < 0)", wake_release)
wake_release_failure_end = braced_statement_end(wake, wake_release_failure)
assert "return release_ret;" in wake[
    wake_release_failure:wake_release_failure_end
]
assert "continue;" not in wake[
    wake_release_failure:wake_release_failure_end
]

sniff = function_body("clicker_wake_train_sniff_activity")
sniff_configure = sniff.index("dwm3000_driver_configure_wake_mode")
sniff_deadline = sniff.index("app_wake_train_deadline_fits", sniff_configure)
sniff_rx = sniff.index("dwm3000_driver_sniff_activity", sniff_deadline)
assert sniff_configure < sniff_deadline < sniff_rx

backoff_body = function_body("clicker_wake_train_backoff")
clip = backoff_body.index("app_wake_train_deadline_clip_delay")
sleep = backoff_body.index("k_msleep(delay_ms)", clip)
assert clip < sleep
assert "return -ETIMEDOUT" in backoff_body[:sleep]

politeness = function_body("clicker_politeness_phase")
clip_phase = politeness.index("app_wake_train_deadline_clip_delay")
phase_deadline = politeness.index("deadline_ms = now_ms + phase_budget_ms")
radio = politeness.index("radio_guard_uwb_claim", phase_deadline)
configure = politeness.index("dwm3000_driver_configure_range_mode", radio)
configure_failure = politeness.index("if (ret < 0)", configure)
configure_recovery = politeness.index(
    "clicker_release_radio_to_standby",
    configure_failure,
)
post_configure_deadline = politeness.index(
    "k_uptime_get() >= deadline_ms", configure_recovery
)
receive = politeness.index("clicker_sample_uwb_gate", post_configure_deadline)
hard_receive_failure = politeness.index("if (ret < 0)", receive)
hard_receive_break = politeness.index("break;", hard_receive_failure)
radio_cleanup = politeness.index(
    "clicker_release_radio_to_standby",
    hard_receive_break,
)
hard_receive_return = politeness.index("if (ret < 0)", radio_cleanup)
hard_receive_return_value = politeness.index("return ret;", hard_receive_return)
assert (
    clip_phase
    < phase_deadline
    < radio
    < configure
    < configure_failure
    < configure_recovery
    < post_configure_deadline
    < receive
    < hard_receive_failure
    < hard_receive_break
    < radio_cleanup
    < hard_receive_return
    < hard_receive_return_value
)
assert "click_deadline_ms" in politeness[:clip_phase]

public_wake = function_body("app_clicker_send_wake_claim_train")
assert "clicker_send_wake_claim_train_until" in public_wake
assert "INT64_MAX" in public_wake

collect = function_body("clicker_collect_uwb_attempt_with_options_until")
wake_until = collect.index("clicker_send_wake_claim_train_until")
discover_until = collect.index("clicker_discover_uwb_anchors_until", wake_until)
deadline_before_schedule = collect.index(
    "app_wake_train_deadline_fits", discover_until
)
schedule = collect.index(
    "clicker_send_range_schedule_until", deadline_before_schedule
)
assert wake_until < discover_until < deadline_before_schedule < schedule
insufficient_release = collect.index(
    "app_clicker_send_range_release", discover_until
)
insufficient_release_failure = collect.index(
    "if (release_ret < 0)", insufficient_release
)
insufficient_release_failure_end = braced_statement_end(
    collect, insufficient_release_failure
)
assert "return release_ret;" in collect[
    insufficient_release_failure:insufficient_release_failure_end
]

schedule_send = function_body("clicker_send_range_schedule_until")
configure = schedule_send.index("dwm3000_driver_configure_wake_mode")
deadline_after_configure = schedule_send.index(
    "app_wake_train_deadline_fits", configure
)
send = schedule_send.index("dwm3000_driver_send_frame", deadline_after_configure)
tx_complete = schedule_send.index("tx_complete_ms = k_uptime_get()", send)
publish_epoch = schedule_send.index("*schedule_tx_ms = tx_complete_ms", tx_complete)
schedule_log = schedule_send.index(
    "clicker_log_range_schedule_entries", publish_epoch
)
assert (
    configure
    < deadline_after_configure
    < send
    < tx_complete
    < publish_epoch
    < schedule_log
)
assert "clicker_range_schedule_deadline_budget_ms" in schedule_send

# A normal click must leave the DWM3000 awake and in range mode across the
# RANGE_SCHEDULE -> first DS-TWR slot handoff. Re-entering from standby uses
# most of the advertised 50 ms first-poll delay on hardware, so the default
# cannot inherit the ML clicker's conditional standby behavior.
range_tx_initializer = re.search(
    r"static const struct app_clicker_range_tx_config\s+"
    r"clicker_range_tx_config\s*=\s*\{(?P<body>.*?)\};",
    CLICKER,
    re.DOTALL,
)
assert range_tx_initializer is not None
assert re.search(
    r"\.prepare_range_mode_after_schedule\s*=\s*true\s*,",
    range_tx_initializer.group("body"),
), "normal click must prepare range mode immediately after RANGE_SCHEDULE"

prepare_branch = schedule_send.index(
    "if (ret == 0 && config->prepare_range_mode_after_schedule)"
)
prepare_range_mode = schedule_send.index(
    "dwm3000_driver_configure_range_mode()", prepare_branch
)
prepared_idle_release = schedule_send.index(
    "clicker_release_radio_to_idle", prepare_range_mode
)
fallback_standby_release = schedule_send.index(
    "clicker_release_radio_to_standby", prepared_idle_release
)
assert (
    prepare_branch
    < prepare_range_mode
    < prepared_idle_release
    < fallback_standby_release
)

# ML builds retain their runtime override: each path starts from the safe
# normal default, then may explicitly choose the ML runtime's handoff policy.
for ml_schedule_builder_name in (
    "clicker_collect_uwb_attempt_with_options_until",
    "clicker_build_and_send_schedule_for_event",
):
    ml_schedule_builder = function_body(ml_schedule_builder_name)
    inherited_config = ml_schedule_builder.index(
        "range_tx_config = clicker_range_tx_config"
    )
    ml_override_guard = ml_schedule_builder.index(
        "if (clicker_callbacks.ml_runtime_active != NULL)", inherited_config
    )
    ml_override = ml_schedule_builder.index(
        "range_tx_config.prepare_range_mode_after_schedule =",
        ml_override_guard,
    )
    ml_runtime_value = ml_schedule_builder.index(
        "clicker_callbacks.ml_runtime_active()", ml_override
    )
    configured_schedule_send = ml_schedule_builder.index(
        "clicker_send_range_schedule_until", ml_runtime_value
    )
    configured_schedule_argument = ml_schedule_builder.index(
        "&range_tx_config", configured_schedule_send
    )
    assert (
        inherited_config
        < ml_override_guard
        < ml_override
        < ml_runtime_value
        < configured_schedule_send
        < configured_schedule_argument
    )

range_burst = function_body("app_clicker_range_scheduled_anchors")
assert "int64_t schedule_tx_ms" in range_burst
assert "schedule_tx_ms = k_uptime_get()" not in range_burst
burst_radio = range_burst.index(
    "radio_guard_uwb_claim"
)
burst_radio_failure = range_burst.index("if (ret < 0)", burst_radio)
burst_radio_failure_end = braced_statement_end(
    range_burst, burst_radio_failure
)
failed_burst_acquire = range_burst[
    burst_radio_failure:burst_radio_failure_end
]
assert "clicker_release_radio_to_standby" not in failed_burst_acquire
assert "dwm3000_driver_standby" not in failed_burst_acquire

burst_loop = range_burst.index(
    "while (session->state == UWB_CLICKER_RANGING)",
    burst_radio_failure_end,
)
burst_loop_end = braced_statement_end(range_burst, burst_loop)
burst_loop_body = range_burst[burst_loop:burst_loop_end]
next_step = burst_loop_body.index("uwb_clicker_next_range_step")
schedule_complete = burst_loop_body.index(
    "if (ret == PROTO_ERR_NOT_FOUND)", next_step
)
schedule_complete_end = braced_statement_end(
    burst_loop_body, schedule_complete
)
schedule_complete_body = burst_loop_body[
    schedule_complete:schedule_complete_end
]
success_at_schedule_end = schedule_complete_body.index(
    "session->state == UWB_CLICKER_SUCCEEDED"
)
clear_stale_sample_error = schedule_complete_body.index(
    "last_ret = 0", success_at_schedule_end
)
schedule_end_break = schedule_complete_body.index(
    "break", clear_stale_sample_error
)
assert success_at_schedule_end < clear_stale_sample_error < schedule_end_break
target = range_burst.index("target_us =", burst_loop)
target_budget = range_burst.index("latest_start_ms", target)
sleep_to_target = range_burst.index("sleep_until_us(target_us)", target_budget)
setup_budget = range_burst.index("remaining_ms =", sleep_to_target)
exchange = range_burst.index("dwm3000_driver_range_initiator", setup_budget)
sample_idle = range_burst.index(
    "idle_ret = clicker_idle_scheduled_range_radio()", exchange
)
idle_failure = range_burst.index("if (idle_ret < 0)", sample_idle)
idle_failure_end = braced_statement_end(range_burst, idle_failure)
cancel_failure = range_burst.index("if (ret == -ECANCELED)", idle_failure_end)
cancel_failure_end = braced_statement_end(range_burst, cancel_failure)
start_failure = range_burst.index(
    "if (!range_result.exchange_started)", cancel_failure_end
)
ml_start_failure_continue = range_burst.index(
    "clicker_callbacks.ml_continue_after_range_start_failure",
    start_failure,
)
assert (
    burst_radio
    < burst_loop
    < target
    < target_budget
    < sleep_to_target
    < setup_budget
    < exchange
    < sample_idle
    < idle_failure
    < cancel_failure
    < start_failure
    < ml_start_failure_continue
    < burst_loop_end
)
assert "slot_deadline_budget_ms" in range_burst[target:exchange]
assert range_burst.count("radio_guard_uwb_claim") == 1
assert "radio_guard_uwb_claim" not in burst_loop_body
assert "clicker_release_radio_to_standby" not in burst_loop_body
assert "last_ret = idle_ret;" in range_burst[idle_failure:idle_failure_end]
assert "break;" in range_burst[idle_failure:idle_failure_end]
assert "uwb_clicker_abort_attempt(session)" in range_burst[
    cancel_failure:cancel_failure_end
]
assert "last_ret = ret;" in range_burst[cancel_failure:cancel_failure_end]
assert "break;" in range_burst[cancel_failure:cancel_failure_end]

# The scheduled owner covers every sample and parks the radio before the sole
# release. Every post-acquisition loop exit converges on that cleanup.
burst_cleanup = function_body("clicker_finish_scheduled_range_radio_burst")
assert "radio_guard_uwb_claim" not in burst_cleanup
cleanup_release = burst_cleanup.index("clicker_release_radio_to_standby")
cleanup_failure = burst_cleanup.index("if (ret < 0)", cleanup_release)
cleanup_failure_end = braced_statement_end(burst_cleanup, cleanup_failure)
assert (
    cleanup_release
    < cleanup_failure
    < cleanup_failure_end
)
assert burst_cleanup.count("clicker_release_radio_to_standby") == 1
assert "return ret;" in burst_cleanup[cleanup_failure_end:]

finish_call = range_burst.index(
    "finish_ret = clicker_finish_scheduled_range_radio_burst(&radio_lease)",
    burst_loop_end,
)
state_abort = range_burst.index(
    "if (last_ret < 0 && session->state == UWB_CLICKER_RANGING)",
    burst_loop_end,
)
finish_failure = range_burst.index("if (finish_ret < 0)", finish_call)
finish_failure_end = braced_statement_end(range_burst, finish_failure)
assert range_burst.count(
    "clicker_finish_scheduled_range_radio_burst(&radio_lease)"
) == 1
assert "return " not in range_burst[
    burst_radio_failure_end:finish_call
], "every post-acquisition burst exit must pass through standby and guard release"
assert burst_loop_end < state_abort < finish_call < finish_failure
assert "uwb_clicker_abort_attempt(session)" in range_burst[state_abort:finish_call]
assert "return finish_ret;" in range_burst[
    finish_failure:finish_failure_end
]
last_error = range_burst.index("if (last_ret < 0)", finish_failure_end)
assert finish_failure < last_error

# Receive-abort ownership is split by lifecycle. Node-communication and mesh
# control are boundary edges consumed by the active receive; gateway priority
# remains level-triggered until its exact arbiter lifecycle releases it.
wait_status = source_function_body(DRIVER_RADIO, "wait_status_internal")
request_abort = source_function_body(
    DRIVER_IO, "dwm3000_driver_request_receive_abort"
)
clear_abort = source_function_body(
    DRIVER_IO, "dwm3000_driver_clear_receive_abort"
)
transport_quiesced = source_function_body(
    NODE_COMM, "app_node_comm_transport_quiesced"
)
assert "receive_abort_owners" in wait_status
assert "DWM3000_RECEIVE_ABORT_LEVEL_MASK" in wait_status
assert "atomic_and(&receive_abort_owners, ~edge_owners)" in wait_status
assert "atomic_or(&receive_abort_owners, requested)" in request_abort
assert "atomic_and(&receive_abort_owners, ~cleared)" in clear_abort
assert "dwm3000_driver_receive_abort_pending()" in transport_quiesced
assert "DWM3000_RECEIVE_ABORT_NODE_COMM" in NODE_COMM
assert "dwm3000_driver_clear_receive_abort" not in NODE_COMM
assert "DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY" in MESH_ARBITRATION
assert "dwm3000_driver_request_receive_abort(" in MESH_ARBITRATION
assert "dwm3000_driver_clear_receive_abort(" in MESH_ARBITRATION
assert "DWM3000_RECEIVE_ABORT_MESH_CONTROL" in MESH_COORDINATION
assert MESH_TRANSPORT.count("DWM3000_RECEIVE_ABORT_MESH_CONTROL") >= 2
assert "DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY" in GATEWAY_CONTROL
assert "DWM3000_RECEIVE_ABORT_NODE_COMM" not in MESH_ARBITRATION
assert "DWM3000_RECEIVE_ABORT_MESH_CONTROL" not in MESH_ARBITRATION

# The gateway's continuous Channel-9 coordinator owner must stand down as
# soon as an explicit RX control handoff exists, including the interval while
# the old scan is still aborting. Otherwise the coordinator immediately
# reclaims gateway RX and prevents the priority Channel-5 command from running.
control_active = source_function_body(
    MESH_COORDINATION, "mesh_rx_handoff_control_active"
)
assert "k_spin_lock(&mesh_rx_handoff_lock)" in control_active
assert (
    "app_mesh_rx_handoff_control_active(&mesh_rx_handoff)" in control_active
)
assert "k_spin_unlock(&mesh_rx_handoff_lock" in control_active

coordinator = source_function_body(
    MESH_COORDINATION, "mesh_coordinator_decide_for_c5_intent"
)
assert "app_mesh_ch9_core_pending_allows_rx(" in coordinator, (
    "the coordinator must keep Channel-9 RX visible while retained custody "
    "is between gateway-ACK attempts"
)
assert re.search(
    r"\.ch9_ack_wait_active\s*=.*?"
    r"app_mesh_ch9_core_ack_wait_active\s*\(",
    coordinator,
    re.DOTALL,
), (
    "the live ACK-owner predicate must block competing Channel-5 work only "
    "while an ACK deadline or send owner is active"
)
assert re.search(
    r"\.ch9_ack_receive_eligible\s*=.*?"
    r"app_mesh_ch9_core_pending_allows_rx\s*\(",
    coordinator,
    re.DOTALL,
), (
    "retry-backoff custody must remain RX eligible without becoming a live "
    "Channel-5 veto"
)
assert re.search(
    r"\.gateway_continuous_ch9\s*=\s*mesh_gateway_route_test_role\s*\(\s*\)"
    r"\s*&&\s*!mesh_rx_handoff_control_active\s*\(\s*\)",
    coordinator,
), (
    "an explicit RX control handoff must suppress the gateway's continuous "
    "Channel-9 coordinator capture"
)

# Every work item admitted through the gateway-priority arbiter owns its
# level-triggered receive-abort bit until the scheduled handler reaches the
# safe boundary. Derive this list from the submit sites so a future internal
# priority handler cannot silently omit the matching release.
gateway_priority_work_items = set(re.findall(
    r"\bmesh_gateway_command_priority_submit\s*\(\s*&([A-Za-z0-9_]+)\s*\)",
    GATEWAY_CONTROL,
))
assert gateway_priority_work_items, "no gateway-priority work submitters found"
for work_item in gateway_priority_work_items:
    init_matches = re.findall(
        rf"\bk_work_init_delayable\s*\(\s*&{re.escape(work_item)}\s*,\s*"
        r"([A-Za-z0-9_]+)\s*\)",
        ANCHOR_INIT,
    )
    assert len(init_matches) == 1, (
        f"gateway-priority work {work_item} must have one handler initializer"
    )
    handler_name = init_matches[0]
    handler = source_function_body(GATEWAY_CONTROL, handler_name)
    release = handler.find(
        "dwm3000_driver_clear_receive_abort(\n"
        "        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY)"
    )
    assert release >= 0, (
        f"{handler_name} retains the gateway-priority receive-abort owner"
    )

    entry = handler.index("ARG_UNUSED(work);") + len("ARG_UNUSED(work);")
    first_stateful_call = re.search(
        r"\b(?:gateway_[A-Za-z0-9_]+|k_(?:mutex|msgq)_[A-Za-z0-9_]+)\s*\(",
        handler[entry:],
    )
    assert first_stateful_call is not None
    assert release < entry + first_stateful_call.start(), (
        f"{handler_name} must release the gateway-priority abort owner at "
        "its safe-boundary entry"
    )

main = source_function_body(MAIN, "main")
port_init = main.index("ret = dwm3000_port_init()")
port_failure = main.index("if (ret < 0)", port_init)
port_failure_end = braced_statement_end(main, port_failure)
assert "runtime_start_fail_closed(" in main[port_failure:port_failure_end]
role_start = min(
    position
    for token in (
        "app_anchor_start_anchor_role()",
        "app_anchor_start_gateway_role()",
        "app_clicker_button_init()",
    )
    if (position := main.find(token)) >= 0
)
assert port_init < port_failure < port_failure_end < role_start

public_collect = function_body("app_clicker_collect_uwb_attempt_with_options")
assert "INT64_MAX" in public_collect

normal = function_body("app_clicker_run_normal_click")
deadline = normal.index("click_deadline_ms =")
normal_wake = normal.index("app_clicker_send_wake_claim_train_until", deadline)
wake_deadline_argument = normal.index("click_deadline_ms", normal_wake)
normal_discover = normal.index(
    "app_clicker_discover_uwb_anchors_until", wake_deadline_argument
)
discover_deadline_argument = normal.index("click_deadline_ms", normal_discover)
normal_schedule = normal.index(
    "clicker_build_and_send_schedule_for_event", discover_deadline_argument
)
schedule_deadline_argument = normal.index("click_deadline_ms", normal_schedule)
normal_epoch_output = normal.index("&schedule_tx_ms", schedule_deadline_argument)
normal_range = normal.index(
    "app_clicker_range_scheduled_anchors", normal_epoch_output
)
normal_epoch_input = normal.index("schedule_tx_ms", normal_range)
range_deadline_argument = normal.index("click_deadline_ms", normal_epoch_input)
assert (
    deadline
    < normal_wake
    < wake_deadline_argument
    < normal_discover
    < discover_deadline_argument
    < normal_schedule
    < schedule_deadline_argument
    < normal_epoch_output
    < normal_range
    < normal_epoch_input
    < range_deadline_argument
)
normal_cancel = normal.index("if (range_ret == -ECANCELED)", normal_range)
normal_cancel_end = braced_statement_end(normal, normal_cancel)
retry = function_body("clicker_runtime_prepare_retry")
retry_rearm_gate = retry.index("if (!radio_guard_uwb_rearm_allowed())")
retry_prepare = retry.index("uwb_clicker_prepare_retry")
retry_tail = retry.index("app_clicker_wake_train_opportunity_tail_ms")
retry_delay = retry.index("app_clicker_apply_retry_delay", retry_prepare)
contention_delay = normal.index(
    "clicker_runtime_prepare_retry", normal_cancel_end
)
retry_contention_delay = retry.index(
    "app_clicker_apply_contention_delay", retry_delay
)
assert normal_range < normal_cancel < normal_cancel_end < contention_delay
assert "return range_ret;" in normal[normal_cancel:normal_cancel_end]
range_error = normal.index("if (range_ret < 0)", normal_cancel_end)
success_with_error = normal.index(
    "if (session.state == UWB_CLICKER_SUCCEEDED)",
    range_error,
)
success_with_error_end = braced_statement_end(normal, success_with_error)
plain_success = normal.index(
    "if (payload.count >= session.config.min_anchor_count",
    success_with_error_end,
)
assert (
    normal_cancel_end
    < range_error
    < success_with_error
    < success_with_error_end
    < plain_success
    < contention_delay
)
assert "clicker_runtime_abort_click" in normal[
    success_with_error:success_with_error_end
]
assert normal[
    success_with_error:success_with_error_end
].count("clicker_runtime_abort_click") == 1
assert "clicker_runtime_expect_effect(FW_EFFECT_CLICK_CLEANUP)" not in normal[
    success_with_error:success_with_error_end
], "the abort helper already consumes the one cleanup effect"
assert "return range_ret;" in normal[
    success_with_error:success_with_error_end
]
assert retry_rearm_gate < retry_prepare < retry_tail < retry_delay < retry_contention_delay
assert "required_retry_tail_ms" in retry[retry_tail:retry_contention_delay]

retry_after_failure = function_body("clicker_runtime_retry_after_failure")
failure_rearm_gate = retry_after_failure.index(
    "if (!radio_guard_uwb_rearm_allowed())"
)
failure_event = retry_after_failure.index(
    "clicker_runtime_click_event_payload", failure_rearm_gate
)
failure_timer = retry_after_failure.index(
    "clicker_runtime_expect_effect(FW_EFFECT_START_TIMER)", failure_event
)
assert failure_rearm_gate < failure_event < failure_timer
assert "clicker_runtime_abort_click" in retry_after_failure[
    failure_rearm_gate:failure_event
]

discover_release_gate = normal.index(
    "if (ret < 0 && !radio_guard_uwb_rearm_allowed())", normal_discover
)
discover_completion = normal.index(
    "FW_EVENT_DISCOVERY_COMPLETED", discover_release_gate
)
range_release_gate = normal.index(
    "if (range_ret < 0 && !radio_guard_uwb_rearm_allowed())", normal_range
)
range_completion = normal.index("FW_EVENT_RANGE_COMPLETED", range_release_gate)
assert normal_discover < discover_release_gate < discover_completion
assert normal_range < range_release_gate < range_completion

# One logical click owns one generation across bounded retries, while RF
# attempt accounting is emitted only after the wake sender reports real TX.
rf_evidence = normal.index(
    "session.diagnostics.wake_claim_tx_count != wake_claim_tx_count",
    normal_wake,
)
rf_started = normal.index("FW_EVENT_RF_STARTED", rf_evidence)
wake_error = normal.index("if (wake_ret < 0)", rf_started)
retry_event = normal.index(
    "clicker_runtime_retry_after_failure",
    wake_error,
)
assert normal_wake < rf_evidence < rf_started < wake_error < retry_event

diagnostic = function_body("app_clicker_run_uwb_diagnostic_click")
diagnostic_deadline = diagnostic.index("click_deadline_ms =")
diagnostic_collect = diagnostic.index(
    "clicker_collect_uwb_attempt_with_options_until", diagnostic_deadline
)
diagnostic_deadline_argument = diagnostic.index(
    "click_deadline_ms", diagnostic_collect
)
assert diagnostic_deadline < diagnostic_collect < diagnostic_deadline_argument
assert "app_clicker_collect_uwb_attempt(&session" not in diagnostic

self_test = function_body("app_clicker_run_self_test")
self_test_acquire = self_test.index("radio_guard_uwb_claim")
self_test_acquire_failure = self_test.index(
    "if (ret < 0)", self_test_acquire
)
self_test_acquire_failure_end = braced_statement_end(
    self_test, self_test_acquire_failure
)
self_test_first_dwm = min(
    self_test.index(name)
    for name in (
        "dwm3000_port_init",
        "dwm3000_port_wakeup",
        "dwm3000_port_hw_reset",
        "dwm3000_driver_probe",
        "dwm3000_port_set_fast_spi",
        "clicker_release_radio_to_standby",
    )
)
self_test_release = self_test.index(
    "clicker_release_radio_to_standby", self_test_first_dwm
)
self_test_diagnostic = self_test.index(
    "app_clicker_run_uwb_diagnostic_click", self_test_release
)
assert (
    self_test_acquire
    < self_test_acquire_failure
    < self_test_acquire_failure_end
    < self_test_first_dwm
    < self_test_release
    < self_test_diagnostic
)
assert "clicker_release_radio_to_standby" not in self_test[
    self_test_acquire_failure:self_test_acquire_failure_end
]
assert "return " not in self_test[
    self_test_acquire_failure_end:self_test_release
], "every post-acquisition self-test exit must pass through guard release"
assert self_test.count("clicker_release_radio_to_standby") == 1

radio_recovery = source_function_body(
    RADIO_RECOVERY,
    "radio_transition_with_bounded_recovery",
)
first_transition = radio_recovery.index("radio_recovery_transition(target)")
force_recovery = radio_recovery.index(
    "dwm3000_driver_force_recovery", first_transition
)
retry_transition = radio_recovery.index(
    "radio_recovery_transition(target)", force_recovery
)
stop_watchdog = radio_recovery.index(
    "app_watchdog_stop_feeding()", retry_transition
)
assert first_transition < force_recovery < retry_transition < stop_watchdog
assert "return 0;" in radio_recovery[first_transition:force_recovery]
assert "return 0;" in radio_recovery[retry_transition:stop_watchdog]

assert "dwm3000_driver_idle()" not in MAIN
assert "dwm3000_driver_standby()" not in MAIN

self_test_report = function_body("app_clicker_emit_self_test_report")
assert "outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;" in self_test_report
assert "app_node_comm_submit_reliable_uplink" in self_test_report
assert "app_node_comm_take_delivery_event_for" in self_test_report
assert "NODE_COMM_TERMINAL_DELIVERED" in self_test_report
assert "SELF_TEST_REPORT_DELIVERY_TIMEOUT_MS" in self_test_report
assert "k_uptime_get" in self_test_report
assert (
    "app_node_comm_cancel_delivery" in self_test_report
    or "app_node_comm_abandon_delivery" in self_test_report
), "bounded timeout must retire the live delivery owner before low power"
assert "clicker_callbacks.send_mesh_outbound" not in self_test_report

# The action worker reaches low power only after the synchronous self-test
# handler has observed delivery terminal state or its bounded timeout.
button_actions = function_body("app_clicker_handle_button_action")
self_test_case = button_actions.index("case BUTTON_ACTION_SELF_TEST_START:")
self_test_emit = button_actions.index(
    "app_clicker_emit_self_test_report", self_test_case
)
self_test_case_end = button_actions.index(
    "case BUTTON_ACTION_SELF_TEST_CANCELLED:", self_test_emit
)
assert self_test_case < self_test_emit < self_test_case_end
action_worker = function_body("clicker_action_work_handler")
handle_action = action_worker.index("app_clicker_handle_button_action(action)")
enter_idle = action_worker.index("app_clicker_enter_idle()", handle_action)
assert handle_action < enter_idle

ml = (ROOT / "app/src/app_ml.c").read_text()
assert "dwm3000_driver_idle()" not in ml
assert "dwm3000_driver_standby()" not in ml
assert ml.count("app_clicker_collect_uwb_attempt_with_options_until(") >= 2
assert "app_clicker_send_wake_claim_train_until(" in ml
assert "app_clicker_discover_uwb_anchors_until(" in ml
assert "ml_clicker_anchor_pair_schedule_budget_ms" in ml
pair_send = ml.index("static int ml_clicker_send_anchor_pair_schedule")
pair_configure = ml.index("dwm3000_driver_configure_wake_mode", pair_send)
pair_deadline = ml.index("app_wake_train_deadline_fits", pair_configure)
pair_tx = ml.index("dwm3000_driver_send_frame", pair_deadline)
pair_cleanup = ml.index(
    "app_radio_idle_with_bounded_recovery", pair_tx
)
pair_release = ml.index("radio_guard_uwb_stop", pair_cleanup)
pair_cleanup_failure = ml.index("if (cleanup_ret < 0)", pair_release)
assert (
    pair_configure
    < pair_deadline
    < pair_tx
    < pair_cleanup
    < pair_release
    < pair_cleanup_failure
)

pair_receive = source_function_body(ml, "ml_clicker_receive_anchor_pair_results")
pair_receive_configure = pair_receive.index(
    "dwm3000_driver_configure_range_mode"
)
pair_receive_label = pair_receive.index("cleanup:", pair_receive_configure)
pair_receive_cleanup = pair_receive.index(
    "app_radio_standby_with_bounded_recovery", pair_receive_label
)
pair_receive_release = pair_receive.index(
    "radio_guard_uwb_stop", pair_receive_cleanup
)
pair_receive_cleanup_failure = pair_receive.index(
    "if (cleanup_ret < 0)", pair_receive_release
)
assert (
    pair_receive_configure
    < pair_receive_label
    < pair_receive_cleanup
    < pair_receive_release
    < pair_receive_cleanup_failure
)
assert pair_receive.count("goto cleanup;") >= 2

pair_survey = source_function_body(ml, "ml_clicker_run_anchor_pair_survey")
pair_attempt_budget = pair_survey.index(
    "ml_clicker_anchor_pair_attempt_budget_ms(session)"
)
pair_wake = pair_survey.index(
    "app_clicker_send_wake_claim_train_until", pair_attempt_budget
)
pair_discovery = pair_survey.index(
    "app_clicker_discover_uwb_anchors_until", pair_wake
)
pair_schedule = pair_survey.index(
    "ml_clicker_send_anchor_pair_schedule", pair_discovery
)
assert pair_attempt_budget < pair_wake < pair_discovery < pair_schedule

ml_post_burst = source_function_body(ml, "ml_clicker_run_post_burst_diagnostics")
diag_target = ml_post_burst.index("attempt_target_us =")
diag_latest_start = ml_post_burst.index("latest_start_ms =", diag_target)
diag_target_admission = ml_post_burst.index(
    "attempt_target_us > latest_start_ms * 1000", diag_latest_start
)
diag_sleep = ml_post_burst.index("sleep_until_us(attempt_target_us)", diag_target_admission)
diag_radio = ml_post_burst.index("radio_guard_uwb_start", diag_sleep)
diag_post_radio_budget = ml_post_burst.index(
    "remaining_ms = click_deadline_ms - k_uptime_get();", diag_radio
)
diag_timeout = ml_post_burst.index("range_request.timeout_ms =", diag_post_radio_budget)
diag_exchange = ml_post_burst.index("dwm3000_driver_range_initiator", diag_timeout)
assert (
    diag_target
    < diag_latest_start
    < diag_target_admission
    < diag_sleep
    < diag_radio
    < diag_post_radio_budget
    < diag_timeout
    < diag_exchange
)
assert "diagnostic_deadline_budget_ms" in ml_post_burst[diag_target:diag_sleep]
assert ml_post_burst.count("app_radio_idle_with_bounded_recovery") == 2
assert "radio_cleanup_failed = true;" in ml_post_burst

assert "deadline-401" in POLICY_TEST
assert "Forced post-sniff activity" in POLICY_TEST
assert "app_wake_train_deadline_clip_delay" in POLICY_TEST

print("clicker wake deadline source invariants passed")
