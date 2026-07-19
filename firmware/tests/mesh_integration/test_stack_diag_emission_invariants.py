#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "app/src/app_stack_diag.c").read_text(encoding="utf-8")
BOARD_SOURCE = (ROOT / "app/src/app_board.c").read_text(encoding="utf-8")
WORKLOAD_SOURCE = (ROOT / "app/src/app_stack_workload_diag.c").read_text(
    encoding="utf-8"
)
CONFIG = (ROOT / "app/conf/mesh-stack-diagnostics.conf").read_text(
    encoding="utf-8"
)
STRESS_CONFIG = (ROOT / "app/conf/mesh-stack-stress.conf").read_text(
    encoding="utf-8"
)
CMAKE = (ROOT / "app/CMakeLists.txt").read_text(encoding="utf-8")
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
CAPTURE = (ROOT / "scripts/capture_stack_evidence.py").read_text(
    encoding="utf-8"
)


def function_body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", SOURCE, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = SOURCE.index("{", match.start())
    depth = 0
    for index in range(brace, len(SOURCE)):
        depth += SOURCE[index] == "{"
        depth -= SOURCE[index] == "}"
        if depth == 0:
            return SOURCE[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


assert "CONFIG_LOG_BACKEND_RTT_BUFFER=1" in CONFIG
assert "CONFIG_LOG_BACKEND_RTT_BUFFER_SIZE=384" in CONFIG
assert "CONFIG_SEGGER_RTT_CUSTOM_LOCKING=y" in CONFIG
assert "CONFIG_SEGGER_RTT_MODE_NO_BLOCK_SKIP=y" in CONFIG
assert '"--up-channel-id", "0"' in CAPTURE
assert "CONFIG_LOG_BACKEND_RTT_BUFFER=0" not in CONFIG
assert "CONFIG_IMEC_STACK_STRESS_DIAGNOSTICS=y" in STRESS_CONFIG
assert "CONFIG_HW_STACK_PROTECTION=y" in STRESS_CONFIG
assert "CONFIG_STACK_CANARIES=y" in STRESS_CONFIG
assert "CONFIG_THREAD_ANALYZER" not in STRESS_CONFIG
assert "CONFIG_STACK_SENTINEL" not in STRESS_CONFIG
assert "STACK_SIZE" not in STRESS_CONFIG
assert "IMEC_STACK_STRESS_BUILD requires mesh_clicker, mesh_anchor, or mesh_gateway" in CMAKE
assert 'conf/mesh-stack-stress.conf' in CMAKE
assert "defined(CONFIG_IMEC_MESH_ROUTE_TEST)" in MAIN
assert "defined(CONFIG_IMEC_STACK_STRESS_DIAGNOSTICS)" in MAIN
assert "#if IMEC_RETAIN_FATAL_BREADCRUMB" in MAIN
assert "mesh_route_test_fatal_thread" in MAIN
assert "mesh_route_test_fatal_stack_start" in MAIN
assert "mesh_route_test_fatal_stack_size" in MAIN
assert "char line[128]" not in SOURCE
assert "static char stack_diag_record[APP_STACK_DIAG_RECORD_CAPACITY]" in SOURCE
assert "K_MUTEX_DEFINE(stack_diag_emit_mutex)" in SOURCE
assert "DBG_STACK_EMIT_ERROR\\n" in SOURCE
assert "return status_stack_diag_note(stack_diag_record);" in SOURCE
assert "K_MUTEX_DEFINE(status_debug_rtt_mutex)" in BOARD_SOURCE or (
    "#define status_debug_rtt_mutex rtt_term_mutex" in BOARD_SOURCE
)
assert "k_mutex_lock(&status_debug_rtt_mutex, K_NO_WAIT)" in BOARD_SOURCE
assert "stack_diag_transport_write(&status_stack_diag_transport" in BOARD_SOURCE
assert "SEGGER_RTT_Write(0, data" in BOARD_SOURCE
assert "line[len - 1u] = '\\n';" in BOARD_SOURCE
assert "line[len++] = '\\n';" in BOARD_SOURCE

sample = function_body("app_stack_diag_sample")
lock = sample.index("k_mutex_lock(&stack_diag_emit_mutex")
begin = sample.index("DBG_STACK_SAMPLE_BEGIN")
rows = sample.index("k_thread_foreach_unlocked")
end = sample.index("DBG_STACK_SAMPLE_END")
unlock = sample.rindex("k_mutex_unlock(&stack_diag_emit_mutex)")
assert lock < begin < rows < end < unlock
assert sample.count("if (context.emit_error == 0)") >= 4
assert "thread_analyzer_print" not in SOURCE
assert sample.index("run->sample_count++") > end

run_begin = function_body("app_stack_diag_run_begin")
assert "const struct app_stack_diag_state captured" not in run_begin
assert "run->identity = *captured" in run_begin
assert run_begin.index("DBG_STACK_RUN_BEGIN") < run_begin.index("emit_ret < 0")
assert run_begin.index("emit_ret < 0") < run_begin.index("memset(run, 0")
assert run_begin.index("memset(run, 0") < run_begin.index("emitted_run_id = 0u")

run_end = function_body("app_stack_diag_run_end")
assert run_end.index("DBG_STACK_RUN_END") < run_end.index("emit_ret == 0")
assert run_end.index("emit_ret == 0") < run_end.index("memset(run, 0")
assert "app_stack_diag_run_end(run->run_id, outcome, &state) == 0" in WORKLOAD_SOURCE

for name, marker in (
    ("app_stack_diag_start", "DBG_STACK_BOOT"),
    ("app_stack_diag_run_begin", "DBG_STACK_RUN_BEGIN"),
    ("app_stack_diag_sample", "DBG_STACK_SAMPLE_BEGIN"),
    ("app_stack_diag_run_end", "DBG_STACK_RUN_END"),
):
    body = function_body(name)
    assert body.index("k_mutex_lock(&stack_diag_emit_mutex") < body.index(marker)
    assert body.index("status_stack_diag_transaction_begin") < body.index(marker)
    assert body.rindex("status_stack_diag_transaction_end") > body.index(marker)
    assert body.rindex("k_mutex_unlock(&stack_diag_emit_mutex)") > body.index(marker)

print("stack diagnostic emission invariants passed")
