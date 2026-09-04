#!/usr/bin/env python3
from pathlib import Path
import sys


MESH_INTEGRATION = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(MESH_INTEGRATION))

from source_text import read_composed_source  # noqa: E402


FIRMWARE = Path(__file__).resolve().parents[4]
SOURCE = FIRMWARE / "app/src/app_mesh_report.c"
APP_CMAKE = (FIRMWARE / "app/CMakeLists.txt").read_text(encoding="utf-8")
RELAY_SOURCE = (FIRMWARE / "src/mesh_relay.c").read_text(encoding="utf-8")


def anchor_guard_intervals(text: str) -> list[tuple[int, int]]:
    intervals: list[tuple[int, int]] = []
    stack: list[tuple[int, bool]] = []
    offset = 0

    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith(("#if ", "#ifdef ", "#ifndef ")):
            stack.append((offset, "DEVICE_ROLE == ROLE_ANCHOR" in stripped))
        elif stripped.startswith("#endif"):
            start, is_anchor_guard = stack.pop()
            if is_anchor_guard:
                intervals.append((start, offset + len(line)))
        offset += len(line)
    assert not stack, "unbalanced preprocessor conditionals"
    return intervals


def assert_gateway_reachable(text: str,
                             needle: str,
                             start: int = 0,
                             end: int | None = None) -> None:
    position = text.rindex(needle, start, len(text) if end is None else end)
    for start, end in anchor_guard_intervals(text):
        assert not start <= position < end, (
            f"gateway-required action is hidden by ROLE_ANCHOR: {needle}"
        )


def main() -> None:
    source = read_composed_source(SOURCE)
    result_actions_start = source.rindex(
        "static void mesh_handle_result_actions("
    )
    result_actions_end = source.index(
        "static uint32_t mesh_drain_rx_queue_locked(", result_actions_start
    )

    for needle in (
        "app_mesh_result_handoff_after_forward(result,",
        "MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u",
        "if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_BUSY)",
        "if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT)",
        "if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK)",
    ):
        assert_gateway_reachable(
            source, needle, result_actions_start, result_actions_end
        )

    assert_gateway_reachable(
        source,
        "static int mesh_handoff_send_result_grant(",
    )
    assert_gateway_reachable(
        source,
        "static void mesh_handoff_note_tx_sent(",
    )

    assert APP_CMAKE.count("IMEC_MESH_RELAY_GATEWAY_ONLY=1") == 1
    assert "#ifndef IMEC_MESH_RELAY_GATEWAY_ONLY" in RELAY_SOURCE
    assert "#define IMEC_MESH_RELAY_GATEWAY_ONLY 0" in RELAY_SOURCE
    assert (
        "#if IMEC_MESH_RELAY_GATEWAY_ONLY != 0 && "
        "IMEC_MESH_RELAY_GATEWAY_ONLY != 1"
    ) in RELAY_SOURCE


if __name__ == "__main__":
    main()
