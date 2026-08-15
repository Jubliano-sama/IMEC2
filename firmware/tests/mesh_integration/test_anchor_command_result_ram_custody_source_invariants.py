#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
COMPLETION = (ROOT / "app/src/app_anchor_command_completion.c").read_text(
    encoding="utf-8"
)
COMPLETION_HEADER = (
    ROOT / "app/src/app_anchor_command_completion.h"
).read_text(encoding="utf-8")
NODE_COMM_HEADER = (ROOT / "include/node_comm.h").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{re.escape(name)}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth:
                continue
            cursor = index + 1
            while cursor < len(source) and source[cursor].isspace():
                cursor += 1
            if cursor >= len(source) or source[cursor] != "{":
                break
            depth = 0
            for end in range(cursor, len(source)):
                depth += source[end] == "{"
                depth -= source[end] == "}"
                if depth == 0:
                    return source[cursor : end + 1]
    raise AssertionError(f"function not found: {name}")


def braced_block_after(source: str, marker: str, start: int = 0) -> str:
    marker_at = source.index(marker, start)
    brace_at = source.index("{", marker_at + len(marker))
    depth = 0
    for end in range(brace_at, len(source)):
        depth += source[end] == "{"
        depth -= source[end] == "}"
        if depth == 0:
            return source[brace_at : end + 1]
    raise AssertionError(f"unterminated block after: {marker}")


def without_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def struct_fields(source: str, name: str) -> list[str]:
    match = re.search(
        rf"\bstruct\s+{re.escape(name)}\s*\{{(?P<body>.*?)\}}\s*;",
        without_comments(source),
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"struct not found: {name}")

    fields: list[str] = []
    for declaration in match.group("body").split(";"):
        field = re.search(
            r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*$",
            declaration.strip(),
        )
        if field is not None:
            fields.append(field.group(1))
    return fields


class AnchorCommandResultRamCustodySourceTests(unittest.TestCase):
    def test_exact_take_compares_every_terminal_field_without_durable_hook(self):
        exact_take = function_body(
            COMPLETION, "app_anchor_command_completion_take_terminal_exact"
        )
        fields = struct_fields(NODE_COMM_HEADER, "node_comm_terminal_event")

        self.assertEqual(
            {
                "handle",
                "delivery_generation",
                "client_token",
                "terminal_at_ms",
                "reason",
                "attempts_started",
                "proof",
            },
            set(fields),
        )
        for field in fields:
            with self.subTest(field=field):
                self.assertIn(f"taken_event.{field}", exact_take)
                self.assertIn(f"peeked_event->{field}", exact_take)

        self.assertNotIn("clear_durable", COMPLETION)
        self.assertNotIn("clear_durable", COMPLETION_HEADER)
        self.assertNotIn("commit_terminal", COMPLETION_HEADER)

    def test_ram_owner_scheduler_has_two_queues_and_fail_closed_terminal(self):
        scheduler = function_body(ANCHOR, "anchor_ram_owner_work_reschedule")
        system = scheduler.index("k_work_reschedule(work, delay)")
        system_success = scheduler.index("if (system_ret >= 0)", system)
        owner = scheduler.index(
            "mesh_route_owner_work_reschedule_timeout(work, delay)",
            system_success,
        )
        both_failed = scheduler.index("if (owner_ret < 0)", owner)
        watchdog = scheduler.index("app_watchdog_stop_feeding()", both_failed)

        self.assertLess(system, system_success)
        self.assertLess(system_success, owner)
        self.assertLess(owner, both_failed)
        self.assertLess(both_failed, watchdog)
        self.assertIn("return owner_ret;", scheduler[owner:])

        for owner_function in (
            "anchor_collection_result_retry",
            "anchor_schedule_collection_command_result",
            "anchor_admit_direct_action_result",
            "anchor_reconcile_pending_command_result",
            "anchor_schedule_reboot_after_command_result",
        ):
            with self.subTest(owner_function=owner_function):
                body = function_body(ANCHOR, owner_function)
                self.assertIn("anchor_ram_owner_work_reschedule(", body)
                self.assertNotIn("anchor_checked_work_reschedule(", body)

        worker = function_body(
            ANCHOR, "anchor_collection_result_work_handler_locked"
        )
        self.assertGreaterEqual(
            worker.count("anchor_ram_owner_work_reschedule("), 2
        )

    def test_initial_submit_or_schedule_failure_is_not_accepted(self):
        collection = function_body(
            ANCHOR, "anchor_schedule_collection_command_result"
        )
        published = collection.index(
            "anchor_collection_result_pending.active = true"
        )
        scheduled = collection.index(
            "schedule_ret = anchor_ram_owner_work_reschedule(", published
        )
        schedule_failure = braced_block_after(
            collection, "if (schedule_ret < 0)", scheduled
        )
        self.assertIn("return schedule_ret;", schedule_failure)
        self.assertNotIn("return 0", without_comments(schedule_failure))
        self.assertNotIn("anchor_collection_result_clear", schedule_failure)

        direct = function_body(ANCHOR, "anchor_admit_direct_action_result")
        submit = direct.index("ret = anchor_submit_command_result(")
        submit_failure = braced_block_after(direct, "if (ret < 0)", submit)
        self.assertIn(
            "app_node_comm_cancel_protocol_response_reservation(",
            submit_failure,
        )
        self.assertIn("anchor_collection_result_retry(", submit_failure)
        self.assertIn("return ret;", submit_failure)
        self.assertNotIn("return 0", without_comments(submit_failure))

        handle_publish = direct.index(
            "anchor_collection_result_pending.delivery_handle = delivery_handle",
            submit,
        )
        terminal_owner = direct.index(
            "schedule_ret = anchor_ram_owner_work_reschedule(", handle_publish
        )
        self.assertIn(
            "return schedule_ret < 0 ? schedule_ret : 0;",
            direct[terminal_owner:],
        )

        handler = function_body(ANCHOR, "anchor_handle_local_command_locked")
        admission = handler.index("ret = anchor_admit_direct_action_result(")
        rejected = braced_block_after(handler, "if (ret < 0)", admission)
        accepted_log = handler.index(
            "anchor action-bearing command accepted with RAM result custody",
            admission,
        )
        accepted_return = handler.index("return 1;", accepted_log)
        self.assertIn("return 0;", rejected)
        self.assertLess(admission, accepted_log)
        self.assertLess(accepted_log, accepted_return)

        finish = function_body(ANCHOR, "anchor_finish_broadcast_command")
        collection_schedule = finish.index(
            "ret = anchor_schedule_collection_command_result("
        )
        self.assertIn("return ret;", finish[collection_schedule:])

    def test_transient_failures_retry_the_same_immutable_ram_owner(self):
        worker = function_body(
            ANCHOR, "anchor_collection_result_work_handler_locked"
        )
        submit = worker.index("ret = anchor_submit_command_result(")
        submit_call_end = worker.index(");", submit)
        submit_call = worker[submit:submit_call_end]
        for exact_argument in (
            "&pending.command",
            "pending.command_id",
            "pending.status",
            "pending.reason",
            "collection_result_id",
            "pending.collection_epoch_id",
        ):
            with self.subTest(argument=exact_argument):
                self.assertIn(exact_argument, submit_call)

        submit_failure = braced_block_after(worker, "if (ret < 0)", submit)
        self.assertIn(
            'anchor_collection_result_retry("command-result-submit-retry")',
            submit_failure,
        )
        self.assertNotIn("anchor_collection_result_clear", submit_failure)
        self.assertNotIn("memset", submit_failure)
        self.assertNotIn("anchor_next_collection_result_seq", submit_failure)

        terminal_failure_at = worker.index("if (ret == 0)")
        terminal_failure = braced_block_after(
            worker, "if (ret == 0)", terminal_failure_at
        )
        handle_clear = terminal_failure.index(
            "anchor_collection_result_pending.delivery_handle = 0u"
        )
        retry = terminal_failure.index(
            'anchor_collection_result_retry(\n                '
            '"command-result-terminal-resubmit")',
            handle_clear,
        )
        self.assertLess(handle_clear, retry)
        self.assertNotIn("anchor_collection_result_clear", terminal_failure)

        retry_owner = function_body(ANCHOR, "anchor_collection_result_retry")
        self.assertIn("anchor_collection_result_pending.active", retry_owner)
        self.assertIn("anchor_ram_owner_work_reschedule(", retry_owner)
        for immutable_assignment in (
            ".command =",
            ".command_id =",
            ".status =",
            ".reason =",
            ".result_id =",
            ".collection_epoch_id =",
        ):
            with self.subTest(immutable_assignment=immutable_assignment):
                self.assertNotIn(immutable_assignment, retry_owner)

    def test_confirmed_actions_transfer_before_ram_owner_clear(self):
        apply_actions = function_body(
            ANCHOR, "anchor_collection_result_apply_actions"
        )
        rediscovery = apply_actions.index(
            "ret = anchor_force_rediscovery_from_command()"
        )
        rediscovery_failure = braced_block_after(
            apply_actions, "if (ret < 0)", rediscovery
        )
        reboot = apply_actions.index(
            "ret = anchor_schedule_reboot_after_command_result()",
            rediscovery,
        )
        reboot_failure = braced_block_after(
            apply_actions, "if (ret < 0)", reboot
        )
        self.assertIn("return ret;", rediscovery_failure)
        self.assertIn("return ret;", reboot_failure)

        worker = function_body(
            ANCHOR, "anchor_collection_result_work_handler_locked"
        )
        confirmed_branch = worker.index(
            "if (anchor_collection_result_pending.result_confirmed)"
        )
        first_apply = worker.index(
            "ret = anchor_collection_result_apply_actions(&pending)",
            confirmed_branch,
        )
        first_clear = worker.index(
            "anchor_collection_result_clear()", first_apply
        )
        terminal_take = worker.index(
            "app_anchor_command_completion_take_terminal_exact("
        )
        mark_confirmed = worker.index(
            "anchor_collection_result_pending.result_confirmed = true",
            terminal_take,
        )
        second_apply = worker.index(
            "ret = anchor_collection_result_apply_actions(&pending)",
            mark_confirmed,
        )
        action_failure = braced_block_after(
            worker, "if (ret < 0)", second_apply
        )
        second_clear = worker.index(
            "anchor_collection_result_clear()", second_apply
        )

        self.assertLess(first_apply, first_clear)
        self.assertLess(mark_confirmed, second_apply)
        self.assertIn("anchor_collection_result_retry(", action_failure)
        self.assertNotIn("anchor_collection_result_clear", action_failure)
        self.assertLess(second_apply, second_clear)

        reboot_owner = function_body(
            ANCHOR, "anchor_schedule_reboot_after_command_result"
        )
        reboot_pending = reboot_owner.index("anchor_reboot_pending = true")
        reboot_schedule = reboot_owner.index(
            "return anchor_ram_owner_work_reschedule(", reboot_pending
        )
        self.assertLess(reboot_pending, reboot_schedule)

        rediscovery_owner = function_body(
            ANCHOR, "anchor_force_rediscovery_from_command"
        )
        path_refresh = rediscovery_owner.index(
            "ret = app_node_comm_schedule_path_refresh("
        )
        path_refresh_failure = braced_block_after(
            rediscovery_owner, "if (ret < 0)", path_refresh
        )
        self.assertIn("return ret;", path_refresh_failure)
        self.assertNotIn("EAGAIN", rediscovery_owner)

    def test_role_start_explicitly_aborts_ram_owner_without_false_restore(self):
        reset = function_body(ANCHOR, "anchor_collection_result_start_ram_owner")
        lock = reset.index("k_mutex_lock(&anchor_command_result_mutex")
        clear = reset.index("anchor_collection_result_clear()", lock)
        sequence = reset.index("anchor_collection_result_seq = 0u", clear)
        unlock = reset.index("k_mutex_unlock(&anchor_command_result_mutex", sequence)
        self.assertLess(lock, clear)
        self.assertLess(clear, sequence)
        self.assertLess(sequence, unlock)

        start = function_body(ANCHOR, "app_anchor_start_anchor_role")
        work_init = start.index(
            "k_work_init_delayable(&anchor_collection_result_work"
        )
        reset_call = start.index("anchor_collection_result_start_ram_owner()")
        heartbeat = start.index("anchor_heartbeat_request_startup()", reset_call)
        self.assertLess(work_init, reset_call)
        self.assertLess(reset_call, heartbeat)

        for false_durable_owner in (
            "anchor_collection_result_restore",
            "anchor_collection_result_persist",
            "anchor_collection_result_clear_durable",
            "anchor_collection_result_restore_complete",
        ):
            with self.subTest(false_durable_owner=false_durable_owner):
                self.assertNotIn(false_durable_owner, ANCHOR)

    def test_heartbeat_is_operator_requested_and_never_boot_traffic(self):
        startup = function_body(ANCHOR, "anchor_heartbeat_request_startup")
        self.assertNotIn("anchor_heartbeat_enabled", startup)
        self.assertNotIn("anchor_heartbeat_schedule", startup)
        self.assertNotIn("anchor_send_heartbeat", startup)

        worker = function_body(ANCHOR, "anchor_heartbeat_work_handler")
        admitted = worker.index("ret = anchor_send_heartbeat()")
        busy = worker[:admitted]
        self.assertIn(
            "anchor_heartbeat_schedule(anchor_heartbeat_interval_ms)", busy
        )
        self.assertNotIn("REPORT_TX_RETRY_DELAY_MS", busy)
        periodic_gate = worker.index("if (anchor_heartbeat_enabled)", admitted)
        periodic_schedule = worker.index(
            "anchor_heartbeat_schedule(anchor_heartbeat_interval_ms)",
            periodic_gate,
        )

        self.assertLess(admitted, periodic_gate)
        self.assertLess(periodic_gate, periodic_schedule)

        send = function_body(ANCHOR, "anchor_send_heartbeat")
        self.assertIn("app_node_comm_submit_best_effort_uplink(", send)
        self.assertNotIn("app_node_comm_submit_reliable_uplink(", send)

        stop = function_body(ANCHOR, "anchor_stop_heartbeat")
        disable = stop.index("anchor_heartbeat_enabled = false")
        cancel = stop.index("k_work_cancel_delayable(&anchor_heartbeat_work)")
        self.assertLess(disable, cancel)
        self.assertNotIn("anchor_startup_heartbeat_pending", ANCHOR)


if __name__ == "__main__":
    unittest.main()
