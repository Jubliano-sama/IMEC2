#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text(
    encoding="utf-8"
)
INIT = (ROOT / "app/src/app_anchor_init.inc").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth:
                continue
            brace = index + 1
            while brace < len(source) and source[brace].isspace():
                brace += 1
            if brace >= len(source) or source[brace] != "{":
                break
            line_start = source.rfind("\n", 0, candidate.start()) + 1
            brace_depth = 0
            for end in range(brace, len(source)):
                brace_depth += source[end] == "{"
                brace_depth -= source[end] == "}"
                if brace_depth == 0:
                    return source[line_start : end + 1]
            raise AssertionError(f"unterminated function {name}")
    raise AssertionError(f"missing function {name}")


def assert_ordered(source: str, *needles: str) -> None:
    offset = 0
    for needle in needles:
        index = source.find(needle, offset)
        assert index >= 0, f"missing ordered source invariant: {needle}"
        offset = index + len(needle)


assert "SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY" in function_body(
    RUNTIME, "schedule_result_delivery_ms"
)
assert "SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY" in function_body(
    RUNTIME, "app_anchor_survey_runtime_schedule_discovery_custody_ms"
)
assert (
    ".schedule_work_ms =\n"
    "            app_anchor_survey_runtime_schedule_discovery_custody_ms"
) in INIT

worker = function_body(RUNTIME, "survey_work_handler")
assert_ordered(
    worker,
    "deadline_events = deadline_take_due()",
    "if (deadline_events.due_mask == 0u)",
    "if (result_due || pair_admission_due)",
    "if (discovery_custody_due)",
    "SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY",
    "SURVEY_ANCHOR_DEADLINE_OPERATION",
    "SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY",
    "SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION",
    "if (!pair_due)",
    "if (!pair_start_delivery_ready())",
)
assert "result_due || discovery_custody_due" not in worker
assert "general_due" not in worker

physical = function_body(RUNTIME, "schedule_physical")
cleanup_schedule = function_body(RUNTIME, "pair_cleanup_schedule_exact")
cleanup_handler = function_body(RUNTIME, "pair_lease_work_handler")
assert "k_work_reschedule_for_queue(runtime_ops.work_queue" in physical
assert "k_work_reschedule(&pair_lease_work" in cleanup_schedule
assert "pair_cleanup_generation = generation" in cleanup_schedule
assert_ordered(
    cleanup_handler,
    "operation_generation = pair_cleanup_generation",
    "pair_lease.pair.operation_generation == operation_generation",
    "survey_pair_lease_expire(&pair_lease",
)

print("anchor survey deadline owner source invariants passed")
