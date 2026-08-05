#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
COMMAND_INGRESS = (ROOT / "app/src/app_gateway_command_ingress.c").read_text()
SURVEY_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
SURVEY_CORE = (ROOT / "src/survey.c").read_text()


def function_body(source: str, name: str) -> str:
    match = None
    brace = None
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth == 0:
                next_index = index + 1
                while source[next_index].isspace():
                    next_index += 1
                if source[next_index] == "{":
                    match = candidate
                    brace = next_index
                break
        if match is not None:
            break
    assert match is not None and brace is not None, f"missing function {name}"
    line_start = source.rfind("\n", 0, match.start()) + 1
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[line_start : index + 1]
    raise AssertionError(f"unterminated function {name}")


reject = function_body(ANCHOR, "gateway_reject_survey_request")
assert reject.count("gateway_emit_host_command_result(") == 1
assert reject.count("gateway_observe_host_terminal(") == 1

reachability = function_body(ANCHOR, "gateway_route_survey_reachability")
assert "gateway_command_survey_sample_admission(" in reachability
assert (
    "uint32_t command_budget_ms = "
    "SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS;"
) in reachability, "survey must retain its dedicated 600-second default"
assert re.search(
    r"gateway_command_extract_budget_ms\s*\([^;]*"
    r"SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS\s*,",
    reachability,
    re.DOTALL,
), "survey budget extraction must use its protocol-specific default"
assert "GATEWAY_COMMAND_BUDGET_MAX_MS" not in reachability, (
    "raising the explicit command maximum must not silently extend survey default"
)
assert re.search(
    r"gateway_command_budget_window_ms\s*\(\s*true\s*,\s*"
    r"command_budget_ms\s*,\s*1u\s*,\s*collection_delay_ms\s*\)",
    reachability,
), "survey collection must remain one indivisible phase under an explicit budget"

control_timeout = function_body(ANCHOR, "gateway_survey_control_timeout_ms")
natural_timeout = function_body(
    ANCHOR, "gateway_survey_natural_control_timeout_ms"
)
assert "gateway_survey_remaining_control_phases" not in ANCHOR, (
    "survey control deadlines must not be divided across future pair phases"
)
assert "survey_gateway_reverse_hint_for_target(" in natural_timeout
assert "survey_pair_control_timeout_ms(reverse_hint.hop_count)" in natural_timeout
assert "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS" in natural_timeout, (
    "unknown route depth must keep the established 90-second fallback"
)
assert re.search(
    r"gateway_command_budget_window_ms\s*\(\s*true\s*,\s*"
    r"remaining_ms\s*,\s*1u\s*,\s*"
    r"natural_timeout_ms\s*\)",
    control_timeout,
), "each survey control must use the natural timeout clipped by the global deadline"
assert "gateway_survey_budget_explicit" not in ANCHOR, (
    "the default operation deadline must be enforced as strongly as an explicit budget"
)

report_accept = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
assert "survey_gateway_hop_count_from_report_ttl(packet->ttl)" in report_accept
validate_endpoints = report_accept.index(
    "survey_reachability_report_endpoints_validate("
)
record_report = report_accept.index(
    "survey_gateway_note_reach_report_with_reverse_hint_status("
)
assert re.search(
    r"survey_reachability_report_endpoints_validate\s*\(\s*"
    r"anchor_id\s*,\s*DEVICE_ID\s*,\s*entries\s*,\s*entry_count\s*\)",
    report_accept,
), "gateway ingress must exclude its own role ID from the anchor graph"
assert validate_endpoints < record_report, (
    "all reported endpoint IDs must be validated before graph mutation"
)

cleanup = function_body(ANCHOR, "gateway_survey_prepare_cleanup_delivery")
cleanup_build = function_body(ANCHOR, "gateway_survey_build_cleanup_outbound")
frozen_deadline = cleanup.index(
    "survey_gateway_transaction_cleanup_deadline("
)
cleanup_initialize = cleanup.index(
    "*cleanup = (struct gateway_survey_cleanup_delivery)"
)
route_snapshot = cleanup.index(
    "gateway_survey_prepare_pair_control(&route, &hop_count)"
)
next_hop_snapshot = cleanup.index(
    "cleanup->next_hop_id = route.next_hop_id"
)
hop_snapshot = cleanup.index("cleanup->hop_count = hop_count")
assert frozen_deadline < cleanup_initialize < route_snapshot
assert route_snapshot < next_hop_snapshot
assert route_snapshot < hop_snapshot
assert cleanup.count("gateway_survey_prepare_pair_control(") == 1
assert cleanup.count("survey_gateway_transaction_cleanup_deadline(") == 1
assert "survey_pair_control_timeout_ms(" not in cleanup, (
    "route retries must not replace the transaction's frozen cleanup deadline"
)
assert "gateway_survey_natural_control_timeout_ms(" not in cleanup
assert "mesh_relay_find_current_downlink(" not in cleanup, (
    "cleanup must derive its route and hop deadline from one atomic snapshot"
)
assert "outbound->next_hop_id = cleanup->next_hop_id" in cleanup_build, (
    "cleanup transmission must reuse the next hop frozen with its deadline"
)
assert "gateway_survey_operation_deadline_ms" not in cleanup, (
    "cleanup must retain its bounded natural deadline after the host deadline"
)

assert reachability.count("gateway_emit_host_command_result(") == 1
assert re.search(
    r"gateway_emit_host_command_result\s*\(\s*host_packet,\s*"
    r"CMD_SURVEY_REACHABILITY,\s*COMMAND_OK,\s*0u\s*\)",
    reachability,
), "only the accepted survey path may emit its result outside the rejection owner"
assert "gateway_observe_host_terminal(" not in reachability
prepare_policy = reachability.index("app_operation_policy_prepare_payload(")
reserve_generation = reachability.index(
    "app_mesh_persistence_reserve_gateway_survey_generation("
)
begin_survey = reachability.index("survey_gateway_begin_operation(")
submit_discovery = reachability.index("app_node_comm_submit_delivery(")
commit_policy = reachability.index("app_operation_policy_commit_prepared(")
assert (
    prepare_policy
    < reserve_generation
    < begin_survey
    < submit_discovery
    < commit_policy
), (
    "survey policy validation must precede runtime mutation and its infallible "
    "commit must follow durable discovery admission"
)
assert re.search(
    r"survey_gateway_begin_operation\s*\(\s*&gateway_survey_context\s*,\s*"
    r"survey_id\s*,\s*operation_generation\s*,\s*sample_count\s*\)",
    reachability,
), (
    "the host survey ID and durable generation have the same scalar shape on "
    "some targets, so enumerate their production argument order"
)
assert "app_operation_policy_install(" not in reachability
for expression in re.findall(r"\breturn\s+(.+?);", reachability, re.DOTALL):
    normalized = " ".join(expression.split())
    assert normalized == "0" or normalized.startswith(
        "gateway_reject_survey_request("
    ), f"survey rejection bypasses its terminal owner: {normalized}"

worker = function_body(ANCHOR, "gateway_host_command_work_handler")
assert re.search(
    r"kind\s*==\s*GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY\s*&&\s*"
    r"item\.command_id\s*!=\s*CMD_SURVEY_REACHABILITY",
    worker,
), "worker must not synthesize a second reachability terminal"

survey_worker = function_body(ANCHOR, "gateway_survey_work_handler")
assert "if (!gateway_survey_context.topology_complete)" in survey_worker
plan_gate = survey_worker.index("survey_gateway_plan_pairs(")
budget_gate = survey_worker.index(
    "survey_gateway_transaction_pair_plan_fits_minimum_budget(",
    plan_gate,
)
topology_gate = survey_worker.index(
    "if (!gateway_survey_topology_accounted)",
    budget_gate,
)
round_drive = survey_worker.index("gateway_survey_round_drive()", topology_gate)
assert plan_gate < budget_gate < topology_gate < round_drive, (
    "final planning, minimum-budget rejection, and one-shot topology "
    "accounting must all precede the concurrent driver that can send PREPARE"
)
assert survey_worker.count("if (!gateway_survey_topology_accounted)") == 1
assert "gateway_survey_discovery_failure_count++" in survey_worker[
    topology_gate:
]
assert "GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE" in survey_worker[
    topology_gate:
], "partial useful topology must remain a non-success terminal"
round_start = function_body(ANCHOR, "gateway_survey_round_start")
round_driver = function_body(ANCHOR, "gateway_survey_round_drive")
assert "survey_gateway_plan_pairs(" not in round_start, (
    "the concurrent driver must not bypass final-plan validation"
)
assert "gateway_survey_topology_accounted" in round_start
assert "gateway_survey_emit_collection_telemetry(" not in round_driver, (
    "collection telemetry must be gated after final-plan validation, before "
    "the concurrent driver"
)
automatic_finish = function_body(ANCHOR, "gateway_survey_auto_finish")
terminal_map = automatic_finish[
    automatic_finish.index("gateway_command_survey_terminal_outcome(") :
]
assert "gateway_survey_context.topology_complete" in terminal_map, (
    "terminal mapping must fail incomplete topology independently of the "
    "one-shot failure counter"
)

# The DS-TWR identity is part of the survey protocol, rather than a local
# implementation detail.  Production pair execution must project the durable
# generation onto the 32-bit PHY session field, while the full 64-bit
# generation remains in the nonce so gateway reboot and host-ID reuse cannot
# admit stale POLL/RESP/FINAL traffic.
initiator = function_body(SURVEY_RUNTIME, "run_pair_initiator")
responder = function_body(SURVEY_RUNTIME, "run_pair_responder")
nonce = function_body(SURVEY_CORE, "survey_sample_nonce")
for body, assignments in (
    (
        initiator,
        (
            "request.session_id = operation_session_id",
            "result.session_id = operation_session_id",
        ),
    ),
    (
        responder,
        ("expected.session_id = operation_session_id",),
    ),
):
    assert re.search(
        r"operation_session_id\s*=\s*"
        r"survey_operation_session_id\s*\(\s*pair->operation_generation\s*\)",
        body,
    ), "each production DS-TWR role must derive its PHY session from generation"
    assert "operation_session_id == 0u" in body, (
        "an unrepresentable operation generation must fail before touching RF"
    )
    for assignment in assignments:
        assert assignment in body
    assert ".session_id = pair->survey_id" not in body, (
        "the host correlation ID must never identify a production PHY session"
    )
assert "pair->operation_generation != 0u" in nonce
assert "pair->operation_generation ^" in nonce, (
    "the full durable generation must distinguish operations that share a "
    "32-bit projected session after wrap"
)

host_route = function_body(ANCHOR, "gateway_route_host_packet")
abort_branch = host_route[host_route.index("command_id == CMD_SURVEY_ABORT") :]
abort_handler = function_body(ANCHOR, "gateway_handle_local_survey_abort")
assert "packet->dst_id == DEVICE_ID" in abort_branch
assert "return gateway_handle_local_survey_abort(" in abort_branch
assert "gateway_command_result_get_dispatch_token()" in abort_branch
assert "gateway_survey_auto_finish_status(" in abort_handler
assert re.search(
    r"ret\s*=\s*gateway_survey_auto_finish_status\s*\(",
    abort_handler,
), "host abort must inspect whether automatic-survey cleanup custody installed"
auto_finish_error = abort_handler.index("if (ret < 0)")
internal_result = abort_handler.index(
    "COMMAND_INTERNAL_ERROR", auto_finish_error
)
success_result = abort_handler.index("COMMAND_OK", internal_result)
assert auto_finish_error < internal_result < success_result, (
    "host abort must terminalize as internal error when automatic-survey "
    "cleanup custody cannot be installed"
)
assert re.search(
    r"gateway_emit_host_command_result_reserved\s*\(\s*"
    r"result_reservation_token\s*,\s*packet\s*,\s*command_id\s*,\s*"
    r"COMMAND_OK\s*,\s*0u\s*\)",
    abort_handler,
), "a gateway-local survey abort must emit an explicit successful result"
assert re.search(
    r"gateway_observe_host_terminal\s*\(\s*packet\s*,\s*command_id\s*,\s*"
    r"COMMAND_OK\s*,\s*GATEWAY_COMMAND_EVENT_REASON_NONE\s*\)",
    abort_handler,
), "a gateway-local survey abort must have one successful terminal owner"
assert abort_branch.index("gateway_handle_local_survey_abort") < abort_branch.index(
    "return gateway_route_mesh_host_packet("
), "a gateway-local survey abort must not fall through into mesh routing"
assert "struct gateway_host_abort_item" in ANCHOR
assert re.search(
    r"K_MSGQ_DEFINE\s*\(\s*gateway_host_abort_msgq\s*,\s*"
    r"sizeof\s*\(\s*struct gateway_host_abort_item\s*\)",
    ANCHOR,
), "local abort custody must not allocate generic max-payload ingress records"
assert "sizeof(struct gateway_host_abort_item) <= 48u" in ANCHOR

preemptive_predicate = function_body(
    ANCHOR, "gateway_host_command_is_preemptive"
)
assert "item->command_id == CMD_SURVEY_ABORT" in preemptive_predicate
assert "item->packet.dst_id == DEVICE_ID" in preemptive_predicate, (
    "only a gateway-local survey abort may bypass serialized command dispatch"
)

ingress_handler = function_body(
    COMMAND_INGRESS, "app_gateway_command_ingress_handle_frame"
)
semantic_preflight = ingress_handler.index(
    "app_gateway_command_ingress_validate_command("
)
preemptive_classification = ingress_handler.index("ops->is_preemptive")
ordinary_admission = ingress_handler.index("ops->admit(", semantic_preflight)
assert semantic_preflight < preemptive_classification < ordinary_admission, (
    "the complete command/options preflight must run before preemptive "
    "classification or either custody queue"
)
malformed_branch = ingress_handler[
    semantic_preflight:preemptive_classification
]
assert "COMMAND_MALFORMED_PAYLOAD" in malformed_branch
assert malformed_branch.count("ops->emit_result(") == 1

host_validation = function_body(
    COMMAND_INGRESS, "app_gateway_command_ingress_validate_command"
)
assert "gateway_command_extract_id(" in host_validation
assert "gateway_command_extract_options(" in host_validation
assert "item->command_id == CMD_SURVEY_ABORT" in host_validation
for canonical_field in (
    "item->packet.flags != 0u",
    "item->packet.src_id == 0u",
    "item->packet.src_id == gateway_id",
    "item->packet.session_id == 0u",
    "item->packet.seq == 0u",
    "item->packet.ttl != 1u",
    "item->packet.message_age_ms != 0u",
    "item->payload_len != PROTO_TLV_U16_ENCODED_LEN",
):
    assert canonical_field in host_validation

preemptive_submit = function_body(
    ANCHOR, "gateway_host_command_submit_preemptive"
)
assert "gateway_host_abort_msgq" in preemptive_submit
assert "gateway_host_abort_work" in preemptive_submit
assert "queued.packet = item->packet" in preemptive_submit
assert "queued.command_id = item->command_id" in preemptive_submit
assert "k_msgq_put(&gateway_host_abort_msgq, &queued" in preemptive_submit
assert preemptive_submit.index("gateway_host_abort_msgq") < (
    preemptive_submit.index("gateway_host_abort_work")
), "the exact abort must enter dedicated custody before its worker is submitted"

cancel_pending = function_body(
    ANCHOR, "gateway_host_command_cancel_pending_surveys"
)
assert "gateway_host_command_msgq" in cancel_pending
assert "gateway_command_uses_survey_mesh(" in cancel_pending, (
    "abort must remove every pending survey phase from normal command custody"
)
assert "gateway_host_command_lifecycle_discard_locked(" in cancel_pending, (
    "removing queued survey bytes must also terminalize their lifecycle owners"
)

abort_worker = function_body(ANCHOR, "gateway_host_abort_work_handler")
abort_route_worker = function_body(
    ANCHOR, "gateway_host_abort_route_work_handler"
)
submit_index = abort_worker.index("mesh_gateway_command_priority_submit(")
assert "gateway_host_command_cancel_pending_surveys(" not in abort_worker
assert "gateway_handle_local_survey_abort(" not in abort_worker, (
    "the system worker must not mutate route-owned queue or survey state"
)
assert "gateway_host_abort_msgq" in abort_route_worker, (
    "the route-owner abort worker must consume the exact command in custody"
)
cancel_index = abort_route_worker.index(
    "gateway_host_command_cancel_pending_surveys("
)
dispatch_index = abort_route_worker.index(
    "gateway_handle_local_survey_abort(", cancel_index
)
assert cancel_index < dispatch_index, (
    "the route owner must cancel queued survey commands before ending the "
    "active survey"
)
assert submit_index >= 0

ble_ingress = function_body(ANCHOR, "gateway_handle_ble_frame")
assert re.search(
    r"\.gateway_id\s*=\s*DEVICE_ID",
    ble_ingress,
), "BLE ingress must enable gateway-local closed-envelope validation"
assert re.search(
    r"\.is_preemptive\s*=\s*gateway_host_command_is_preemptive",
    ble_ingress,
)
assert re.search(
    r"\.submit_preemptive\s*=\s*gateway_host_command_submit_preemptive",
    ble_ingress,
), "BLE abort ingress must bypass the safe-boundary command submitter"

ingress_emit = function_body(ANCHOR, "gateway_host_command_emit_result")
bind_index = ingress_emit.index("gateway_command_result_bind_ingress(")
terminal_index = ingress_emit.index(
    "gateway_emit_host_command_result_reserved(", bind_index
)
assert bind_index < terminal_index, (
    "a pre-admission malformed command must bind and convert its reserved "
    "result credit instead of leaking it"
)
assert "bind_ret == 0 || bind_ret == -EALREADY" in ingress_emit
assert "gateway_command_result_release_ingress(" in ingress_emit
assert "COMMAND_MALFORMED_PAYLOAD" in ingress_emit
assert "GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST" in ingress_emit

survey_worker = function_body(ANCHOR, "gateway_survey_work_handler")
survey_reschedule = function_body(
    ANCHOR, "gateway_survey_work_reschedule"
)
assert re.search(
    r"ret\s*=\s*mesh_route_owner_work_reschedule\s*\(\s*"
    r"&gateway_survey_work\s*,\s*delay_ms\s*\)",
    survey_reschedule,
), "survey coordinator work must stay on the mesh-route owner queue"
assert "if (ret < 0)" in survey_reschedule
assert "app_watchdog_stop_feeding();" in survey_reschedule
assert len(re.findall(
    r"mesh_route_owner_work_reschedule\s*\(\s*&gateway_survey_work",
    ANCHOR,
)) == 1, "one checked wrapper must own the survey worker's queue selection"
assert not re.search(
    r"\bk_work_reschedule(?:_for_queue)?\s*\([^;]*"
    r"&gateway_survey_work",
    ANCHOR,
    re.DOTALL,
), "survey work must never fall back to a direct system-workqueue reschedule"
deadline_check = survey_worker.index("gateway_survey_operation_deadline_ms")
pair_planning = survey_worker.index("survey_gateway_plan_pairs(")
assert deadline_check < pair_planning, (
    "the stored operation deadline must win before collection can be closed "
    "or pairs planned"
)
assert re.search(
    r"if\s*\(\s*ret\s*==\s*-ETIMEDOUT\s*\)\s*\{.*?"
    r"gateway_survey_auto_finish_status\s*\(\s*COMMAND_TIMEOUT\s*,\s*"
    r"GATEWAY_COMMAND_EVENT_REASON_TIMEOUT\s*\)",
    survey_worker,
    re.DOTALL,
), "an exhausted stored operation budget must terminate the survey as a global timeout"

pair_prepare = function_body(
    SURVEY_RUNTIME, "app_anchor_survey_runtime_handle_pair_prepare"
)
assert "packet->flags != FLAG_DIAGNOSTIC" in pair_prepare, (
    "dedicated pair PREPARE admission must reject extra or missing flags"
)
pair_start = function_body(
    SURVEY_RUNTIME, "app_anchor_survey_runtime_start_pair_from_command"
)
assert pair_prepare.index("app_operation_policy_prepare_payload(") < (
    pair_prepare.index("survey_pair_lease_prepare_round_bound(")
)
assert re.search(
    r"app_operation_policy_prepare_payload\s*\(\s*payload\s*,\s*"
    r"payload_len\s*,\s*0u\s*,\s*APP_OPERATION_POLICY_PAIR_MASK\s*,",
    pair_prepare,
), "pair PREPARE must reject policies from unrelated operation families"
assert pair_prepare.index("survey_pair_lease_prepare_round_bound(") < (
    pair_prepare.index("app_operation_policy_commit_prepared(")
)
assert "app_operation_policy_install(" not in pair_prepare
assert pair_start.index("app_operation_policy_prepare_payload(") < (
    pair_start.index("survey_pair_lease_start_round_bound(")
)
assert re.search(
    r"app_operation_policy_prepare_payload\s*\(\s*payload\s*,\s*"
    r"payload_len\s*,\s*0u\s*,\s*APP_OPERATION_POLICY_PAIR_MASK\s*,",
    pair_start,
), "pair START must reject policies from unrelated operation families"
assert pair_start.index("survey_pair_lease_start_round_bound(") < (
    pair_start.index("app_operation_policy_commit_prepared(")
)
assert "app_operation_policy_install(" not in pair_start
assert "packet->src_id != GATEWAY_ID" in pair_start

print("survey command source invariants passed")
