#!/usr/bin/env python3
"""Source-boundary guards for durable post-command action custody."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMMANDS = ROOT / "app" / "src" / "app_anchor_commands.inc"
INIT = ROOT / "app" / "src" / "app_anchor_init.inc"


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


def main() -> None:
    commands = COMMANDS.read_text(encoding="utf-8")
    init = INIT.read_text(encoding="utf-8")

    worker = function_body(
        commands, "anchor_collection_result_work_handler_locked"
    )
    peek = worker.index("app_node_comm_peek_delivery_event_for(")
    commit = worker.index(
        "app_anchor_command_completion_commit_terminal(", peek
    )
    actions = worker.index("anchor_collection_result_apply_actions(", commit)
    assert peek < commit < actions
    assert "app_mesh_persistence_clear_collection_result();" not in worker
    assert "command-result-terminal-resubmit" in worker
    assert "command-result-terminal-commit-retry" in worker
    assert "command-result-terminal-owner" in worker
    consumed_mismatch = worker.index("if (ret == -EPROTO)")
    dead_handle_clear = worker.index(
        "anchor_collection_result_pending.delivery_handle = 0u;",
        consumed_mismatch,
    )
    retry = worker.index(
        'anchor_collection_result_retry(\n'
        '                "command-result-terminal-commit-retry")',
        dead_handle_clear,
    )
    assert consumed_mismatch < dead_handle_clear < retry

    admission = function_body(commands, "anchor_admit_direct_action_result")
    persist = admission.index("anchor_collection_result_persist(")
    submit = admission.index("anchor_submit_command_result(", persist)
    assert persist < submit
    assert "anchor_collection_result_pending.active = true;" in admission
    assert "direct-command-result-persistence-retry" in admission
    assert "direct-command-result-submit-retry" in admission
    assert "return 0;" in admission[submit:]

    handler = function_body(commands, "anchor_handle_local_command_locked")
    duplicate = handler.index(
        "anchor_direct_action_result_matches(packet, command_id)"
    )
    execute = handler.index("anchor_execute_command_side_effects(", duplicate)
    durable = handler.index("anchor_admit_direct_action_result(", execute)
    assert duplicate < execute < durable
    assert "app_anchor_command_completion_watch(" not in handler
    assert (
        "app_anchor_survey_runtime_abandon_pair_start_delivery("
        in handler
    )
    assert "app_anchor_survey_runtime_cancel_pair_start(packet)" in handler

    direct_match = function_body(
        commands, "anchor_direct_action_result_matches"
    )
    assert (
        "anchor_collection_result_pending.command_id == command_id"
        in direct_match
    )
    collection_match = function_body(
        commands, "anchor_collection_result_matches_broadcast_command"
    )
    assert (
        "anchor_collection_result_pending.command_id == command_id"
        in collection_match
    )
    assert (
        "anchor_collection_result_pending.collection_epoch_id ==\n"
        "               options->collection_epoch_id"
        in collection_match
    )

    for wrapper_name in (
        "anchor_collection_result_work_handler",
        "anchor_command_execute_work_handler",
        "anchor_handle_local_command",
    ):
        wrapper = function_body(commands, wrapper_name)
        assert "k_mutex_lock(&anchor_command_result_mutex" in wrapper
        assert "k_mutex_unlock(&anchor_command_result_mutex)" in wrapper

    restore = function_body(commands, "anchor_collection_result_restore")
    restore_locked = function_body(
        commands, "anchor_collection_result_restore_locked"
    )
    admission = function_body(
        commands, "anchor_schedule_collection_command_result"
    )
    assert "DEVICE_ROLE != ROLE_ANCHOR" in restore
    assert "return -ENOTSUP;" in restore
    assert "k_mutex_lock(&anchor_command_result_mutex" in restore
    assert "k_mutex_unlock(&anchor_command_result_mutex)" in restore
    assert "anchor_collection_result_restore_complete = true;" in restore
    assert "anchor_collection_result_restore_complete = true;" in restore_locked
    assert "anchor_collection_result_seq <" not in restore_locked
    assert (
        "anchor_collection_result_seq = snapshot.result_id.result_seq"
        not in restore_locked
    )
    assert (
        "anchor_collection_node_boot_counter = "
        "snapshot.result_id.node_boot_counter"
        not in restore_locked
    )
    gate = admission.index("!anchor_collection_result_restore_complete")
    allocate_boot = admission.index("anchor_collection_node_boot_id()", gate)
    allocate_seq = admission.index("anchor_next_collection_result_seq()", gate)
    assert gate < allocate_boot
    assert gate < allocate_seq
    direct_admission = function_body(
        commands, "anchor_admit_direct_action_result"
    )
    direct_gate = direct_admission.index(
        "!anchor_collection_result_restore_complete"
    )
    direct_mutation = direct_admission.index(
        "memset(&anchor_collection_result_pending", direct_gate
    )
    assert direct_gate < direct_mutation

    start = function_body(init, "app_anchor_start_anchor_role")
    assert "anchor_collection_result_restore();" in start
    assert "app_mesh_persistence_clear_collection_result();" not in start
    outbox = start.index("app_mesh_persistence_restore_outbox(")
    deferred = start.index(
        "app_mesh_persistence_restore_deferred_outbox(", outbox
    )
    child = start.index(
        "app_mesh_persistence_restore_child_custody(", deferred
    )
    radio = start.index("uwb_anchor_session_init(", child)
    assert outbox < deferred < child < radio
    assert "return ret;" in start[outbox:deferred]
    assert "return ret;" in start[deferred:child]
    assert "return ret;" in start[child:radio]
    assert "failed closed" in start[outbox:deferred]
    assert "failed closed" in start[deferred:child]
    assert "failed closed" in start[child:radio]

    scheduler = function_body(commands, "anchor_checked_work_reschedule")
    assert "app_watchdog_stop_feeding();" in scheduler


if __name__ == "__main__":
    main()
