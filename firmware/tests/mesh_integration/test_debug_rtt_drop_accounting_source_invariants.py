#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
BOARD = (ROOT / "app/src/app_board.c").read_text()
BOARD_HEADER = (ROOT / "app/src/app_board.h").read_text()


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


def braced_statement(source: str, start: int) -> str:
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[start : index + 1]
    raise AssertionError("unterminated braced statement")


# Optional RTT traces must stay nonblocking, but every skipped record must be
# visible later.  Mutex contention and a short SEGGER write are separate loss
# mechanisms, so they must update different cumulative counters.
writer = function_body(BOARD, "debug_rtt_write_bytes")
lock = writer.index("k_mutex_lock(&status_debug_rtt_mutex, K_NO_WAIT)")
busy_branch = braced_statement(writer, writer.rfind("if", 0, lock + 1))
assert "K_FOREVER" not in writer
assert "return;" in busy_branch

busy_increment = re.search(
    r"atomic_inc\s*\(\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\)",
    busy_branch,
)
assert busy_increment is not None, "mutex-busy RTT drops must be counted"

write = re.search(
    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*SEGGER_RTT_Write\s*\(",
    writer,
)
assert write is not None, "the SEGGER write result must not be discarded"
written_name = write.group(1)
write_end = writer.index(");", write.end())
write_arguments = writer[write.end() : write_end]
assert "text" in write_arguments and "length" in write_arguments
short_start = writer.index("if", write_end)
short_header = writer[short_start : writer.index("{", short_start)]
assert written_name in short_header and "length" in short_header
assert "!=" in short_header, "short RTT writes must be detected"
short_branch = braced_statement(writer, short_start)
short_increment = re.search(
    r"atomic_inc\s*\(\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\)",
    short_branch,
)
assert short_increment is not None, "short RTT writes must be counted"
assert busy_increment.group(1) != short_increment.group(1)
assert "(void)SEGGER_RTT_Write" not in writer

snapshot = function_body(BOARD, "status_debug_rtt_drop_counts")
assert busy_increment.group(1) in snapshot
assert short_increment.group(1) in snapshot
assert snapshot.count("atomic_get") >= 2
assert re.search(
    r"void\s+status_debug_rtt_drop_counts\s*\(\s*"
    r"uint32_t\s*\*\s*mutex_busy_drops\s*,\s*"
    r"uint32_t\s*\*\s*short_write_drops\s*\)\s*;",
    BOARD_HEADER,
    re.DOTALL,
)
