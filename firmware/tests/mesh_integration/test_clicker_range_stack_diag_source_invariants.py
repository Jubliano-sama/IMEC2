#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CLICKER = (ROOT / "app/src/app_clicker.c").read_text()


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


def braced_statement_end(source: str, statement_start: int) -> int:
    brace = source.index("{", statement_start)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return index + 1
    raise AssertionError("unterminated braced statement")


# Stack-workload diagnostics are deliberately burst-level bookkeeping.  Their
# synchronous stack scans take milliseconds on hardware, so running them once
# per scheduled sample consumes the following anchor's fixed RF slot.
range_burst = function_body(CLICKER, "app_clicker_range_scheduled_anchors")
radio_claim = range_burst.index("radio_guard_uwb_claim")
sample_loop = range_burst.index(
    "while (session->state == UWB_CLICKER_RANGING)", radio_claim
)
sample_loop_end = braced_statement_end(range_burst, sample_loop)
finish_burst = range_burst.index(
    "finish_ret = clicker_finish_scheduled_range_radio_burst(&radio_lease)",
    sample_loop_end,
)

admit_call = "app_stack_workload_diag_click_activity_admit"
sample_call = "app_stack_workload_diag_click_activity_sample"
release_call = "app_stack_workload_diag_click_activity_release"

assert range_burst.count(admit_call) == 1
assert range_burst.count(sample_call) == 1
assert range_burst.count(release_call) == 1

admit = range_burst.index(admit_call)
sample = range_burst.index(sample_call)
release = range_burst.index(release_call)

assert radio_claim < admit < sample_loop
assert sample_loop_end < finish_burst < sample < release

timing_critical_samples = range_burst[sample_loop:sample_loop_end]
for diag_call in (admit_call, sample_call, release_call):
    assert diag_call not in timing_critical_samples, (
        f"{diag_call} must stay outside the scheduled DS-TWR sample loop"
    )
