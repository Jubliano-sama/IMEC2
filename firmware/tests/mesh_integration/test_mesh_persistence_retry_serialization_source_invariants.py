#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app" / "src"


def function_body(source: str, name: str) -> str:
    definition = re.search(
        rf"(?m)^[A-Za-z_][^;\n]*\b{re.escape(name)}\s*\([^;]*?\)\s*\n?\{{",
        source,
    )
    if definition is None:
        raise AssertionError(f"missing function {name}")
    brace = source.index("{", definition.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


coordination = (
    APP / "app_mesh_report_coordination.inc"
).read_text(encoding="utf-8")
kconfig = (ROOT / "app" / "Kconfig").read_text(encoding="utf-8")

# The persistence retry mutates the same relay runtime as ACK, cancellation,
# replacement, and child-custody processing. Production mesh presets select
# the dedicated route queue, so the retry must use that owner rather than the
# system workqueue.
schedule = function_body(coordination, "mesh_schedule_persistence_retry")
assert "mesh_route_owner_work_reschedule(&mesh_persistence_retry_work" in schedule
assert "k_work_reschedule(" not in schedule
assert "k_work_reschedule_for_queue(" not in schedule

route_owner = function_body(
    coordination, "mesh_route_owner_work_reschedule_timeout"
)
assert "k_work_reschedule_for_queue(&mesh_route_work_q" in route_owner
assert re.search(
    r"config IMEC_DEDICATED_COMM_WORKQUEUE.*?"
    r"default y if IMEC_MESH_ROUTE_TEST",
    kconfig,
    re.DOTALL,
)

# Every outbox attempt becomes dirty before it observes the current relay
# state. An inactive ACK/cancel generation must issue a checked durable clear;
# dropping the dirty bit would resurrect the prior outbox after reset.
outbox_save = function_body(coordination, "mesh_save_outbox_durable")
generation_begin = outbox_save.index(
    "mesh_persistence_generation_begin("
)
mark_dirty = outbox_save.index("mesh_outbox_persistence_dirty = true;")
active_check = outbox_save.index("mesh_relay_tx_active(&mesh_runtime)")
save_current = outbox_save.index("app_mesh_persistence_save_outbox(")
initialize_clear = outbox_save.index("app_mesh_persistence_init()")
clear_current = outbox_save.index("app_mesh_persistence_clear_outbox()")
failure = outbox_save.index("if (ret < 0)", clear_current)
retry = outbox_save.index(
    "mesh_schedule_persistence_retry(reason)", failure
)
failure_return = outbox_save.index("return ret;", retry)
assert (
    generation_begin < mark_dirty < active_check <
    save_current < initialize_clear < clear_current <
    failure < retry < failure_return
)

retry_handler = function_body(
    coordination, "mesh_persistence_retry_work_handler"
)
retry_save = retry_handler.index(
    'mesh_save_outbox_durable("outbox-dirty")'
)
# A successfully persisted ACK_CONFIRM must be rescheduled for RF; the active
# check is allowed only after the unconditional durable retry has completed.
ack_confirm_reschedule = retry_handler.index(
    "mesh_relay_tx_active(&mesh_runtime)", retry_save
)
assert retry_save < ack_confirm_reschedule
assert "mesh_outbox_persistence_dirty = false" not in retry_handler

# A save/clear completion can retire only the generation it began. This
# ensures a replacement admitted before an older completion keeps its own
# dirty obligation until that replacement is persisted.
generation_next = function_body(
    coordination, "mesh_persistence_generation_begin"
)
assert "next_generation = *generation + 1u;" in generation_next
assert "if (next_generation == 0u)" in generation_next
assert "*generation = next_generation;" in generation_next

outbox_clear = outbox_save.rfind(
    "mesh_outbox_persistence_dirty = false;"
)
outbox_generation_guard = outbox_save.rfind(
    "mesh_persistence_generation_is_current(",
    0,
    outbox_clear,
)
assert outbox_generation_guard >= 0
assert outbox_save.count("mesh_outbox_persistence_dirty = false;") == 1

child_save = function_body(
    coordination, "mesh_save_child_custody_durable"
)
child_begin = child_save.index("mesh_persistence_generation_begin(")
child_dirty = child_save.index(
    "mesh_child_custody_persistence_dirty = true;"
)
child_persist = child_save.index(
    "app_mesh_persistence_save_child_custody("
)
child_clear = child_save.index(
    "mesh_child_custody_persistence_dirty = false;"
)
child_guard = child_save.rfind(
    "mesh_persistence_generation_is_current(",
    0,
    child_clear,
)
assert child_begin < child_dirty < child_persist < child_guard < child_clear
assert child_save.count(
    "mesh_child_custody_persistence_dirty = false;"
) == 1


def next_generation(current: int) -> int:
    current = (current + 1) & 0xFFFFFFFF
    return 1 if current == 0 else current


def complete_if_current(
    current: int, completed: int, dirty: bool
) -> bool:
    return False if completed != 0 and current == completed else dirty


# Adversarial order: an inactive clear fails, its route-owned retry starts,
# and a replacement generation is admitted before that retry completion is
# published. The old completion cannot clear the replacement's dirty state.
current_generation = 0
failed_clear_generation = next_generation(current_generation)
current_generation = failed_clear_generation
dirty = True
retry_generation = next_generation(current_generation)
current_generation = retry_generation
replacement_generation = next_generation(current_generation)
current_generation = replacement_generation
dirty = complete_if_current(
    current_generation, retry_generation, dirty
)
assert dirty
dirty = complete_if_current(
    current_generation, replacement_generation, dirty
)
assert not dirty
