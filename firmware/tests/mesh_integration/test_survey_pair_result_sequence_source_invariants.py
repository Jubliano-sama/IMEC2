#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
ANCHOR_RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text(
    encoding="utf-8"
)
GATEWAY_SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text(
    encoding="utf-8"
)
PAIR_DELIVERY = (
    ROOT / "app/src/app_anchor_survey_result_delivery.c"
).read_text(encoding="utf-8")
PERSISTENCE_HEADER = (
    ROOT / "app/src/app_mesh_persistence.h"
).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth != 0:
                continue
            next_index = index + 1
            while next_index < len(source) and source[next_index].isspace():
                next_index += 1
            if next_index >= len(source) or source[next_index] != "{":
                break
            line_start = source.rfind("\n", 0, candidate.start()) + 1
            brace_depth = 0
            for end in range(next_index, len(source)):
                brace_depth += source[end] == "{"
                brace_depth -= source[end] == "}"
                if brace_depth == 0:
                    return source[line_start : end + 1]
            raise AssertionError(f"unterminated function {name}")
    raise AssertionError(f"missing function {name}")


queue_result = function_body(
    ANCHOR_RADIO, "anchor_queue_survey_sample_result"
)
derive = queue_result.index("ret = survey_pair_result_transport_sequence(")
reject = queue_result.index("if (ret != PROTO_OK)", derive)
reject_return = queue_result.index(
    "return mesh_errno_from_proto(ret);", reject
)
serialize = queue_result.index("survey_append_sample_tlvs(", reject_return)
packet_init = queue_result.index(
    "survey_init_result_packet_from_reporter(", serialize
)
durable_stage = queue_result.index(
    "app_anchor_survey_result_delivery_stage_reserved(", packet_init
)

assert derive < reject < reject_return < serialize < packet_init < durable_stage
assert re.search(
    r"survey_pair_result_transport_sequence\s*\(\s*round_id\s*,"
    r"\s*sample_index\s*,\s*&transport_sequence\s*\)",
    queue_result,
), "the synchronized round/sample identity must derive the transport sequence"
assert "app_anchor_survey_runtime_next_sequence(" not in queue_result, (
    "pair results cannot fall back to reset-volatile transport sequence state"
)
assert re.search(
    r"survey_init_result_packet_from_reporter\s*\([^;]*"
    r"\btransport_sequence\b",
    queue_result,
    re.DOTALL,
), "the checked deterministic sequence must reach the immutable packet header"
assert "delivery_reservation_token == 0u" in queue_result
assert "app_node_comm_submit_reliable_uplink(" not in queue_result, (
    "the radio producer must hand its reserved result to the durable four-slot "
    "owner instead of creating a reset-volatile communication record directly"
)

assert "#define APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS 4u" in (
    PERSISTENCE_HEADER
)
assert re.search(
    r"result_delivery_slots\s*\[\s*"
    r"APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS\s*\]",
    PAIR_DELIVERY,
), "one persistent local-delivery owner is required for every legal sample"
assert re.search(
    r"BUILD_ASSERT\s*\(\s*APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS\s*==\s*"
    r"SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT",
    PAIR_DELIVERY,
), "the durable journal count must cover the complete four-sample burst"

stage_result = function_body(
    PAIR_DELIVERY, "app_anchor_survey_result_delivery_stage_reserved"
)
persist = stage_result.index("app_mesh_local_delivery_stage(")
admit = stage_result.index(
    "app_node_comm_commit_durable_reliable_uplink_reservation(", persist
)
publish_handle = stage_result.index("slot->handle = handle", admit)
assert persist < admit < publish_handle, (
    "the exact result must be durable before its reserved node-communication "
    "handle becomes visible"
)
assert "slot->admission_in_progress = true" in stage_result
assert "result_delivery_abandon_handle(handle" in stage_result, (
    "a raced successful admission must not leave a second live transport owner"
)

service_slot = function_body(PAIR_DELIVERY, "result_delivery_service_slot")
owner_match = service_slot.index(
    "result_delivery_ops.active_owner_matches_outbound(&outbound)"
)
resume_confirm = service_slot.index(
    'result_delivery_ops.resume_restored_outbox(\n'
    '                "survey-pair-result-owner")',
    owner_match,
)
reserve = service_slot.index(
    "app_node_comm_reserve_durable_reliable_uplinks(", resume_confirm
)
commit_reserved = service_slot.index(
    "app_node_comm_commit_durable_reliable_uplink_reservation(", reserve
)
assert owner_match < resume_confirm < reserve < commit_reserved, (
    "boot service must resume an exact restored raw/ACK-confirm owner before "
    "allocating a replacement delivery"
)

digest_match = function_body(
    PAIR_DELIVERY, "result_delivery_semantic_digest_matches"
)
assert "mesh_packet_semantic_digest(" in digest_match
assert "semantic_digest_equal(" in digest_match
for callback in (
    "app_anchor_survey_result_delivery_gateway_confirmed",
    "app_anchor_survey_result_delivery_transport_released",
):
    callback_body = function_body(PAIR_DELIVERY, callback)
    assert "result_delivery_semantic_digest_matches(" in callback_body
    assert "-EBADMSG" in callback_body, (
        "same-header callbacks must not mutate a slot with different bytes"
    )

round_owner = function_body(GATEWAY_SURVEY, "gateway_survey_action_round_id")
allocate = round_owner.index("ret = survey_pair_result_next_round_id(")
commit = round_owner.index(
    "gateway_survey_sequential_run = "
    "(struct gateway_survey_sequential_run)",
    allocate,
)
publish = round_owner.index(
    "*round_id = gateway_survey_sequential_run.round_id", commit
)
assert allocate < commit < publish
assert "gateway_survey_sequential_run.generation_cursor" in round_owner[
    allocate:commit
]
assert ".round_id = next_round_id" in round_owner[commit:publish]
assert ".generation_cursor = next_round_id" in round_owner[commit:publish]
assert "gateway_next_command_seq(" not in round_owner, (
    "survey round generations cannot inherit the unrelated command sequence"
)

begin_survey = function_body(
    GATEWAY_SURVEY, "gateway_route_survey_reachability"
)
finish_survey = function_body(
    GATEWAY_SURVEY, "gateway_survey_auto_finish_status"
)
finish_cleanup = function_body(
    GATEWAY_SURVEY, "gateway_survey_finish_cleanup_if_complete"
)
reset_statement = (
    "memset(&gateway_survey_sequential_run, 0,"
)
assert reset_statement in begin_survey, (
    "a newly admitted survey must start its bounded generation at zero"
)
assert reset_statement not in finish_survey, (
    "survey finish must retain its sequential generation cursor while remote "
    "cleanup lanes still own the terminating round"
)
assert reset_statement in finish_cleanup, (
    "survey terminal cleanup must retire its sequential generation cursor "
    "only after every remote cleanup lane is complete"
)
