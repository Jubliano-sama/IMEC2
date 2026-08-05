#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
REPORT_HEADER = (ROOT / "app/src/app_mesh_report.h").read_text()
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
ANCHOR_HEADER = (ROOT / "app/src/app_anchor.h").read_text()
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
PAIR_RESULT_DELIVERY = (
    ROOT / "app/src/app_anchor_survey_result_delivery.c"
).read_text()
SURVEY_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
CORE_SURVEY = (ROOT / "src/survey.c").read_text()
NODE_COMM_APP = (ROOT / "app/src/app_node_comm.c").read_text()
CONFIG = (ROOT / "app/src/app_config.h").read_text()
DRIVER = read_composed_source(ROOT / "app/src/dwm3000_driver.c")
PERSISTENCE = (ROOT / "app/src/app_mesh_persistence.c").read_text()
SURVEY_HEADER = (ROOT / "include/survey.h").read_text()


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


def braced_block(source: str, start: int) -> str:
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[start : index + 1]
    raise AssertionError("unterminated source block")


def assert_debug_record_fits(
    source: str, marker: str, unsigned_widths: tuple[int, ...]
) -> None:
    match = re.search(
        rf'status_debug_printf\("([^"\n]*{re.escape(marker)}[^"\n]*)"',
        source,
    )
    assert match is not None, f"missing debug record {marker}"
    rendered = match.group(1).replace(r"\n", "\n")
    rendered = re.sub(r"%08x", "f" * 8, rendered)
    widths = iter(unsigned_widths)
    rendered = re.sub(r"%u", lambda unused: "9" * next(widths), rendered)
    try:
        next(widths)
    except StopIteration:
        pass
    else:
        raise AssertionError(f"unused width for debug record {marker}")
    assert "%" not in rendered, f"unmodeled format in debug record {marker}"
    assert len(rendered) <= 127, (
        f"debug record {marker} can exceed status_debug_printf's 127-byte payload"
    )


tracked = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
for match in re.finditer(r"mesh_store_route_waiting_tx\(", tracked):
    prefix = tracked[max(0, match.start() - 100) : match.start()]
    assert "if (store_route_wait)" in prefix, "unguarded generic route-wait store"

owned = function_body(REPORT, "mesh_start_owned_tracked_tx")
assert "APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL" in owned
assert "rf_sent" in owned

direct = function_body(REPORT, "mesh_send_direct_gateway_payload_and_wait_ack")
assert "anchor_survey_delivery_gateway_confirmed" not in direct, (
    "a raw direct gateway ACK is phase one and cannot terminalize survey custody"
)
assert "observation->gateway_confirmed" not in direct
assert "mesh_schedule_tx_timeout()" in direct, (
    "the queued gateway ACK must drive the same durable ACK-confirm lifecycle "
    "as a relayed ACK"
)

gateway_accept_wrapper = function_body(
    REPORT, "mesh_report_gateway_handle_survey_discovery_report"
)
assert re.search(
    r"\bint\s*\(\*gateway_handle_survey_discovery_report\)", REPORT_HEADER
), "gateway survey callback must report semantic acceptance"
assert "return mesh_report_callbacks->gateway_handle_survey_discovery_report(" in (
    gateway_accept_wrapper
)
assert "return -ENOTSUP;" in gateway_accept_wrapper

gateway_schedule = function_body(ANCHOR, "gateway_survey_work_reschedule")
assert "mesh_route_owner_work_reschedule(" in gateway_schedule
assert "if (ret < 0)" in gateway_schedule
assert "app_watchdog_stop_feeding();" in gateway_schedule, (
    "a rejected survey-owner reschedule must force bounded reset recovery"
)
assert len(
    re.findall(
        r"mesh_route_owner_work_reschedule\s*\(\s*&gateway_survey_work\b",
        ANCHOR,
    )
) == 1, "survey code must not bypass the checked liveness scheduler"
assert not re.search(
    r"\bk_work_reschedule(?:_for_queue)?\s*\([^;]*&gateway_survey_work\b",
    ANCHOR,
    re.S,
), "survey work must stay on its checked route-owner scheduler"

gateway_accept = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
pair_accept = function_body(ANCHOR, "gateway_note_survey_pair_result")
semantic_preflight = function_body(
    ANCHOR, "gateway_survey_preflight_semantic_delivery"
)
discovery_preflight = function_body(
    ANCHOR, "gateway_survey_preflight_discovery_report"
)
pair_preflight = function_body(
    ANCHOR, "gateway_survey_preflight_pair_result"
)
round_preflight = function_body(
    ANCHOR, "gateway_survey_round_preflight_sample"
)
assert re.search(
    r"\bint\s+gateway_survey_preflight_semantic_delivery\s*\(",
    ANCHOR_HEADER,
), "the journal layer needs the pure survey semantic preflight API"
transport_check = semantic_preflight.index("packet->dst_id != DEVICE_ID")
pair_dispatch = semantic_preflight.index(
    "gateway_survey_preflight_pair_result("
)
report_decode = semantic_preflight.index(
    "gateway_survey_preflight_discovery_report("
)
assert transport_check < pair_dispatch < report_decode, (
    "both survey report classes must pass the common transport gate"
)
for rejected_transport in (
    "packet->payload_len != payload_len",
    "packet->src_id == DEVICE_ID",
    "radio_channel != UWB_CHANNEL_MESH_PAYLOAD",
    "packet->flags !=",
    "FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC",
    "!mesh_id_is_unicast(previous_hop_id)",
    "previous_hop_id == DEVICE_ID",
    "link_quality > 100u",
):
    assert rejected_transport in semantic_preflight
commit_preflight = gateway_accept.index(
    "gateway_survey_preflight_semantic_delivery("
)
commit_pair = gateway_accept.index("gateway_note_survey_pair_result(")
commit_report_decode = gateway_accept.index(
    "survey_extract_reach_report_tlvs("
)
assert commit_preflight < commit_pair < commit_report_decode, (
    "the serialized RX owner must preflight before any survey commit path"
)
for forbidden_mutation in (
    "survey_gateway_note_reach_report_with_reverse_hint(",
    "gateway_survey_collection_freeze_finalize_cutoff(",
    "gateway_survey_work_reschedule(",
    "gateway_survey_duplicate_count++",
):
    assert forbidden_mutation not in discovery_preflight, (
        f"discovery preflight mutates protocol state: {forbidden_mutation}"
    )
assert "&gateway_survey_pair_result_mask" not in pair_preflight
assert "&gateway_survey_pair_responder_usable_mask" not in pair_preflight
assert "&gateway_survey_pair_initiator_unusable_mask" not in pair_preflight
assert "&gateway_survey_pair_responder_unusable_mask" not in pair_preflight
assert "gateway_survey_duplicate_count++" not in pair_preflight
assert "gateway_survey_round_ensure_observing(" not in round_preflight
assert "app_gateway_survey_round_mark_observing_after_go(" not in round_preflight
assert "app_gateway_survey_round_note_sample(" not in round_preflight
assert "gateway_survey_duplicate_count++" not in round_preflight
assert "app_node_comm_peek_delivery_attempts_started(" in round_preflight
assert "app_node_comm_delivery_attempts_started(" not in round_preflight
assert "app_node_comm_peek_delivery_attempts_started(" in discovery_preflight
assert "app_node_comm_delivery_attempts_started(" not in discovery_preflight
assert "gateway_survey_pair_sample_identity(" in pair_preflight
assert "&sample, packet->src_id, false, &duplicate" in pair_preflight
assert "app_gateway_survey_round_preflight_sample(" in round_preflight
assert "survey_pair_note_sample_masks(" not in pair_preflight
assert "survey_pair_note_sample_masks(" not in round_preflight
assert "survey_gateway_reach_report_compare(" in discovery_preflight
assert "slot->" not in discovery_preflight, (
    "discovery preflight must compare through the validated compact-context API"
)
assert "survey_reach_report_semantic_fingerprint(" not in discovery_preflight
pair_identity = pair_accept.index("gateway_survey_pair_sample_identity(")
pair_mask_mutation = pair_accept.index("survey_pair_note_sample_masks(")
pair_identity_commit = pair_accept.index(
    "gateway_survey_pair_sample_identity(", pair_identity + 1
)
assert pair_identity < pair_mask_mutation < pair_identity_commit, (
    "pair conflict classification must precede mask mutation and identity "
    "commit must follow successful mask admission"
)
assert "survey_operation_session_id(operation_generation)" in discovery_preflight
assert "packet->src_id != anchor_id" in discovery_preflight
assert "gateway_survey_context.operation_generation !=" in discovery_preflight
assert "return -ESTALE;" in discovery_preflight
duplicate_report = gateway_accept[
    gateway_accept.index("if (duplicate_report)") :
    gateway_accept.index("reverse_hint =")
]
assert "return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;" in duplicate_report, (
    "a valid previously recorded reach report must remain ACK-eligible"
)
assert gateway_accept.index("if (duplicate_report)") < gateway_accept.index(
    "if (gateway_survey_auto.running)"
), "an accepted report duplicate must survive orchestration phase advance"
plan_tail = gateway_accept[
    gateway_accept.index("ret = survey_gateway_plan_pairs(") :
]
assert re.search(
    r"if \(ret != PROTO_OK\).*?return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;",
    plan_tail,
    re.S,
), (
    "pair-planning failure after report storage must not revoke acceptance"
)

pair_decode = function_body(CORE_SURVEY, "survey_extract_pair_tlvs")
for required_pair_field in (
    "TLV_SURVEY_ID",
    "TLV_INITIATOR_ID",
    "TLV_RESPONDER_ID",
    "TLV_SAMPLE_COUNT",
):
    assert required_pair_field in pair_decode

sample_decode = function_body(CORE_SURVEY, "survey_extract_sample_tlvs")
assert "survey_extract_pair_tlvs(" in sample_decode
assert "survey_round_id_extract_tlv(" in sample_decode
for required_sample_field in (
    "TLV_SAMPLE_INDEX",
    "TLV_DISTANCE_MM",
    "TLV_QUALITY",
    "TLV_RANGE_STATUS",
):
    assert required_sample_field in sample_decode
assert "survey_sample_validate(&parsed)" in sample_decode
assert sample_decode.index("survey_sample_validate(&parsed)") < sample_decode.index(
    "*sample = parsed"
), "the central decoder must not mutate caller output before full validation"

assert pair_accept.count("survey_extract_sample_tlvs(") == 1, (
    "the gateway must use the shared complete survey-sample decoder"
)
assert "command_find_" not in pair_accept, (
    "gateway-local field parsing would bypass the shared round identity contract"
)
sample_result = function_body(ANCHOR, "anchor_queue_survey_sample_result")
assert "uint16_t round_id" in sample_result
assert sample_result.index("sample.round_id = round_id") < sample_result.index(
    "survey_append_sample_tlvs("
) < sample_result.index("survey_init_result_packet_from_reporter("), (
    "the synchronized round must enter the sample before result serialization"
)
assert "delivery_reservation_token == 0u" in sample_result
assert "app_anchor_survey_result_delivery_stage_reserved(" in sample_result, (
    "a sampled pair result must enter its dedicated durable source journal "
    "before its pre-reserved communication record is consumed"
)

pair_result_stage = function_body(
    PAIR_RESULT_DELIVERY,
    "app_anchor_survey_result_delivery_stage_reserved",
)
pair_journal_stage = pair_result_stage.index("app_mesh_local_delivery_stage(")
pair_admission_owner = pair_result_stage.index(
    "slot->admission_in_progress = true", pair_journal_stage
)
pair_stage_unlock = pair_result_stage.index(
    "RESULT_DELIVERY_UNLOCK();", pair_admission_owner
)
pair_comm_commit = pair_result_stage.index(
    "app_node_comm_commit_durable_reliable_uplink_reservation("
)
assert (
    pair_journal_stage
    < pair_admission_owner
    < pair_stage_unlock
    < pair_comm_commit
), (
    "the exact pair result must be durable before communication admission "
    "can expose it to RF, and the slot must retain an interleaving owner"
)
assert "app_mesh_local_delivery_cleanup_ack(" not in pair_result_stage, (
    "communication admission failure must retain the exact durable pair result"
)
assert "result_delivery_abandon_handle(handle" in pair_result_stage, (
    "a successful handle that loses staged-slot ownership must be explicitly "
    "abandoned before source retry"
)
assert "result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS)" in (
    pair_result_stage
), "a staged result must retain a worker-owned retry path after admission pressure"

pair_result_restore = function_body(
    PAIR_RESULT_DELIVERY,
    "app_anchor_survey_result_delivery_restore",
)
assert (
    "app_mesh_persistence_restore_survey_pair_result_delivery(" in
    pair_result_restore
)
restore_error = pair_result_restore.index("if (ret < 0)")
restore_return = pair_result_restore.index("return ret;", restore_error)
assert "clear_survey_pair_result" not in pair_result_restore[
    restore_error:restore_return
], "corrupt pair-result custody must fail closed instead of deleting evidence"
assert "app_mesh_local_delivery_rebase_after_boot(" in pair_result_restore

pair_terminal = function_body(
    PAIR_RESULT_DELIVERY,
    "result_delivery_service_terminal",
)

pair_service_slot = function_body(
    PAIR_RESULT_DELIVERY,
    "result_delivery_service_slot",
)
assert "if (slot->admission_in_progress)" in pair_service_slot
assert pair_service_slot.index("if (slot->admission_in_progress)") < (
    pair_service_slot.index(
        "app_node_comm_commit_durable_reliable_uplink_reservation("
    )
), "a concurrent service pass must not admit a second handle during staging"
pair_delivered = pair_terminal.index(
    "event->reason == NODE_COMM_TERMINAL_DELIVERED"
)
pair_ack_commit = pair_terminal.index(
    "app_mesh_local_delivery_commit_ack(", pair_delivered
)
pair_terminal_take = pair_terminal.index(
    "app_node_comm_take_delivery_event_for(", pair_ack_commit
)
pair_ack_cleanup = pair_terminal.index(
    "app_mesh_local_delivery_cleanup_ack(", pair_terminal_take
)
assert pair_delivered < pair_ack_commit < pair_terminal_take < pair_ack_cleanup, (
    "terminal pair-result proof must persist ACK_COMMITTED before consuming "
    "the communication event, then clear the raw journal afterward"
)

pair_gateway_confirmed = function_body(
    PAIR_RESULT_DELIVERY,
    "app_anchor_survey_result_delivery_gateway_confirmed",
)
assert "packet->msg_type != MSG_SURVEY_PAIR_RESULT" in pair_gateway_confirmed
assert "app_mesh_local_delivery_commit_ack(" in pair_gateway_confirmed
assert "app_mesh_local_delivery_cleanup_ack(" not in pair_gateway_confirmed, (
    "the final gateway-confirm callback may persist acceptance, but terminal "
    "event ownership must perform the later source-journal delete"
)
assert (
    "SURVEY_PAIR_RESULT_SOURCE_DEADLINE_MS UINT64_MAX"
    in PAIR_RESULT_DELIVERY
), "durable pair-result source custody must not expire without gateway proof"
assert (
    "result_delivery_retain_for_retry_locked(" in pair_terminal
), "every non-accepted pair-result terminal must retain the exact source"
assert "app_mesh_local_delivery_discard_failed(" not in PAIR_RESULT_DELIVERY, (
    "transport exhaustion must never delete an unconfirmed pair result"
)
prepare_transport = function_body(
    PAIR_RESULT_DELIVERY,
    "result_delivery_prepare_transport_envelope",
)
assert "outbound->queued_at_valid = false" in prepare_transport
assert "outbound->earliest_tx_valid = false" in prepare_transport
assert (
    pair_service_slot.count("result_delivery_prepare_transport_envelope(")
    >= 1
), "every restored/rearmed source record must enter nodecomm without pre-RF age"

runtime_schedule = function_body(
    SURVEY_RUNTIME, "schedule"
)
assert "runtime_work_queue_ready && runtime_ops.work_queue != NULL" in (
    runtime_schedule
), "survey work must reject scheduling before its private queue is running"
runtime_start = function_body(
    SURVEY_RUNTIME, "app_anchor_survey_runtime_start"
)
assert "app_anchor_survey_result_delivery_start(" not in runtime_start, (
    "restoring a result must not schedule onto the not-yet-started private queue"
)
runtime_post_start = function_body(
    SURVEY_RUNTIME, "app_anchor_survey_runtime_post_work_queue_start"
)
ready_publish = runtime_post_start.index("runtime_work_queue_ready = true")
restored_kick = runtime_post_start.index(
    "app_anchor_survey_result_delivery_start()", ready_publish
)
assert ready_publish < restored_kick
anchor_start = function_body(ANCHOR, "app_anchor_start_anchor_role")
queue_start = anchor_start.index("k_work_queue_start(&anchor_uwb_scan_work_q")
post_start = anchor_start.index(
    "app_anchor_survey_runtime_post_work_queue_start()", queue_start
)
discovery_restore_kick = anchor_start.index(
    "app_anchor_survey_discovery_retry_report()", post_start
)
assert queue_start < post_start < discovery_restore_kick, (
    "one queue-ready boundary must precede all restored survey delivery kicks"
)
assert re.search(
    r"BUILD_ASSERT\s*\(\s*APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS\s*==\s*"
    r"SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT",
    PAIR_RESULT_DELIVERY,
), "the source journal must cover every result in one maximum pair burst"
assert re.search(
    r"BUILD_ASSERT\s*\(\s*APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY\s*>=\s*"
    r"SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT",
    PAIR_RESULT_DELIVERY,
), "communication admission must cover every result in one maximum pair burst"

pair_worker = function_body(SURVEY_RUNTIME, "survey_work_handler")
pair_path_start = pair_worker.index("if (!pair_start_delivery_ready())")
pair_path = pair_worker[pair_path_start:]
pair_custody_gate = pair_path.index(
    "app_anchor_survey_result_delivery_occupied_count()"
)
pair_reserve = pair_path.index(
    "app_node_comm_reserve_durable_reliable_uplinks("
)
pair_radio = pair_path.index('radio_guard_uwb_start("survey pair DS-TWR")')
assert pair_custody_gate < pair_reserve < pair_radio, (
    "the worker must drain prior result custody and atomically reserve the "
    "complete next burst before starting pair RF"
)
assert re.search(
    r"app_node_comm_reserve_durable_reliable_uplinks\s*\(\s*"
    r"pair\.sample_count\s*,",
    pair_path,
), "the reservation count must match the exact commanded sample count"
assert pair_path.count(
    "app_anchor_survey_result_delivery_cancel_reservations("
) >= 3, (
    "every pre-RF rejection and post-run exit must release all unconsumed "
    "pair-result reservations"
)
cancel_reservations = function_body(
    PAIR_RESULT_DELIVERY,
    "app_anchor_survey_result_delivery_cancel_reservations",
)
assert "app_node_comm_cancel_durable_reliable_uplink_reservation(" in (
    cancel_reservations
)
assert "delivery_reservation_tokens[i] = 0u" in cancel_reservations
assert "app_watchdog_stop_feeding();" in cancel_reservations, (
    "a reservation-cancel failure loses the only RAM token and must force "
    "bounded reboot recovery instead of silently shrinking capacity"
)
pair_radio_start = pair_path.index(
    'ret = radio_guard_uwb_start("survey pair DS-TWR")'
)
radio_guard_failure = pair_path[
    pair_radio_start :
    pair_path.index("survey_rf_retry_reset(&pair_rf_retry)", pair_radio_start)
]
assert "reschedule && cancel_ret == 0" in radio_guard_failure, (
    "a leaked reservation must suppress another pair attempt until watchdog "
    "recovery clears node-communication RAM ownership"
)

for pair_runner_name in ("run_pair_initiator", "run_pair_responder"):
    pair_runner = function_body(SURVEY_RUNTIME, pair_runner_name)
    queue_call = pair_runner.index("runtime_ops.queue_sample_result(")
    reservation_arg = pair_runner.index(
        "delivery_reservation_tokens[", queue_call
    )
    queue_failure = pair_runner.index("if (ret < 0)", reservation_arg)
    failure_return = pair_runner.index("return ret;", queue_failure)
    consume_token = pair_runner.index(
        "delivery_reservation_tokens[sample_index] = 0u", failure_return
    )
    assert queue_call < reservation_arg < queue_failure < failure_return < (
        consume_token
    ), (
        f"{pair_runner_name} must stop immediately on custody failure and "
        "consume a reservation only after durable staging succeeds"
    )
assert re.search(
    r"packet->session_id\s*!=\s*survey_operation_session_id\s*"
    r"\(\s*sample\.pair\.operation_generation\s*\)",
    pair_accept,
), "pair results must bind wire sessions to the durable operation generation"
assert "packet->src_id != sample.pair.initiator_id" in pair_accept
assert "packet->src_id != sample.pair.responder_id" in pair_accept
assert "survey_pair_note_sample_masks(" in pair_accept
assert "&gateway_survey_pair_result_mask" in pair_accept
assert "&gateway_survey_pair_responder_usable_mask" in pair_accept
sample_policy = function_body(CORE_SURVEY, "survey_pair_note_sample_masks")
usable_check = sample_policy.index("survey_sample_distance_usable(sample)")
sample_consumed = sample_policy.index("*usable_mask |= sample_bit")
assert usable_check < sample_consumed, (
    "the shared admission policy must distinguish usable geometry before "
    "consuming a sample index"
)
assert "if ((*usable_mask & sample_bit) != 0u)" in sample_policy[
    sample_consumed:
], "an unusable report cannot replace already usable geometry"
assert "*responder_usable_mask |= sample_bit" in sample_policy, (
    "RSL-bearing responder geometry must be tracked separately from "
    "initiator fallback geometry"
)
rerange_limit = re.search(
    r"#define\s+SURVEY_GATEWAY_PAIR_MAX_RERUNS\s+(\d+)u",
    SURVEY_HEADER,
)
assert rerange_limit is not None, "pair reranging must have an explicit bound"
assert int(rerange_limit.group(1)) == 2, (
    "the conservative recovery policy permits two automatic reruns"
)
for reporter_mask in (
    "gateway_survey_pair_initiator_unusable_mask",
    "gateway_survey_pair_responder_unusable_mask",
):
    assert reporter_mask in pair_accept, (
        "unusable results must retain reporter identity so one reporter's duplicate "
        "cannot masquerade as both pair participants"
    )
assert "reporter_id != sample->pair.initiator_id" in sample_policy
assert "reporter_id != sample->pair.responder_id" in sample_policy
assert "survey_pair_missing_samples_all_unusable(" not in pair_accept, (
    "unusable or initiator-only completion must preserve the frozen observation "
    "window for a later preferred responder result"
)

finalize_pair = function_body(ANCHOR, "gateway_survey_finalize_pair_observation")
preferred_gate = finalize_pair.index(
    "gateway_survey_pair_responder_usable_mask"
)
deadline_gate = finalize_pair.index("!deadline", preferred_gate)
rerun_start = finalize_pair.index("survey_gateway_auto_rerun_pair(")
assert preferred_gate < deadline_gate < rerun_start
rerun_end = finalize_pair.index(
    "event = gateway_observability_event(", rerun_start
)
rerun_path = finalize_pair[rerun_start:rerun_end]
assert "== PROTO_OK" in rerun_path
assert "GATEWAY_SURVEY_PAIR_FINALIZE_RETRY" in rerun_path
for forbidden_accounting in (
    "gateway_survey_pair_success_count++",
    "gateway_survey_pair_failure_count++",
    "survey_gateway_next_pair(",
    "gateway_survey_context.next_pair_index",
):
    assert forbidden_accounting not in rerun_path, (
        "reranging the retained pair must not complete, duplicate, or skip pair accounting"
    )
assert "if ((*responder_usable_mask & sample_bit) != 0u)" in sample_policy
assert "else if ((*usable_mask & sample_bit) != 0u)" in sample_policy
assert "return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;" in pair_accept, (
    "a valid lower- or equal-priority pair sample must remain ACK-eligible"
)

relay_ack = function_body(REPORT, "mesh_handle_result_actions")
confirm_complete = function_body(REPORT, "mesh_complete_gateway_ack_confirm")
parse_confirm = confirm_complete.index(
    "mesh_gateway_ack_confirm_identity_packet("
)
producer_cleanup = confirm_complete.index(
    "app_mesh_persistence_complete_confirmed_producer(", parse_confirm
)
survey_cleanup = confirm_complete.index(
    "anchor_survey_delivery_gateway_confirmed", producer_cleanup
)
node_comm_cleanup = confirm_complete.index(
    "app_node_comm_note_gateway_confirmed_digest_at(", survey_cleanup
)
core_terminal = confirm_complete.index(
    "mesh_relay_commit_gateway_ack_confirm_terminal(", node_comm_cleanup
)
durable_terminal = confirm_complete.index(
    'mesh_save_outbox_durable("gateway-ack-confirm-terminal")',
    core_terminal,
)
assert (
    parse_confirm
    < producer_cleanup
    < survey_cleanup
    < node_comm_cleanup
    < core_terminal
    < durable_terminal
)
assert "&original_packet, original_digest" in confirm_complete[
    survey_cleanup : core_terminal
], "survey ACK_COMMITTED must be selected by exact original bytes"
assert "app_node_comm_note_gateway_confirmed_at(" not in confirm_complete, (
    "a same-header/different-payload late confirm must not terminalize a newer "
    "node-communication record"
)
assert "MSG_GATEWAY_ACK_CONFIRM" in relay_ack
assert "mesh_complete_gateway_ack_confirm(" in relay_ack

tx_timeout = function_body(REPORT, "mesh_tx_timeout_handler")
dirty_fence = tx_timeout.index("if (mesh_outbox_persistence_dirty)")
relay_tick = tx_timeout.index("mesh_relay_tick_with_random(")
assert dirty_fence < relay_tick, (
    "an already queued timeout must not transmit a RAM ACK-confirm until its "
    "successor snapshot has passed durable write/readback"
)

preempt = function_body(REPORT, "mesh_preempt_clear_outbox")
assert "released_packet" in preempt
assert "mesh_packet_semantic_digest(" in preempt
assert re.search(
    r"anchor_survey_delivery_transport_released\s*\(\s*"
    r"released_packet,\s*semantic_digest,\s*true\s*\)",
    preempt,
)

assert ".anchor_survey_delivery_gateway_confirmed =" in ANCHOR
assert ".anchor_survey_delivery_transport_released =" in ANCHOR
assert "SURVEY_DELIVERY_LOCK()" in DISCOVERY
assert "app_mesh_local_delivery_recover" in DISCOVERY
assert re.search(
    r"#define\s+SURVEY_DISCOVERY_REPORT_SOURCE_DEADLINE_MS\s+UINT64_MAX",
    DISCOVERY,
), (
    "a discovery report must remain in durable source custody while an "
    "unrelated reliable owner blocks its first RF admission"
)
assert re.search(
    r"BUILD_ASSERT\s*\(\s*SURVEY_DISCOVERY_REPORT_CUSTODY_MAX_MS\s*<=\s*"
    r"SURVEY_DISCOVERY_REPORT_DELIVERY_TAIL_MS",
    CONFIG,
), "maximum hop-aware custody must remain inside the gateway delivery tail"
retry = function_body(DISCOVERY, "app_anchor_survey_discovery_retry_report")
assert retry.count("app_node_comm_submit_delivery(") == 1
assert "NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK" in retry
assert "outbound = *app_mesh_local_delivery_outbound(delivery);" in retry, (
    "the communication facade must receive the exact persisted survey envelope"
)
assert (
    "absolute_deadline_ms =\n"
    "        SURVEY_DISCOVERY_REPORT_SOURCE_DEADLINE_MS"
) in retry
assert "&delivery_handle" in retry
assert "survey_delivery_handle = delivery_handle" in retry
assert "app_node_comm_abandon_delivery(stale_handle)" in retry, (
    "a submit that races with supersede must release its accepted handle"
)
for synchronous_or_inline_api in (
    "app_node_comm_start_delivery(",
    "app_node_comm_start_owned_delivery(",
    "app_node_comm_request_path(",
    "app_node_comm_service_deliveries(",
    "mesh_start_tracked_tx(",
    "mesh_start_owned_tracked_tx(",
    "mesh_request_route(",
    "dwm3000_driver_",
):
    assert synchronous_or_inline_api not in retry, (
        "survey retry must submit immutable work without running RF inline: "
        f"{synchronous_or_inline_api}"
    )
submit_index = retry.index("app_node_comm_submit_delivery(")
assert retry.rfind("SURVEY_DELIVERY_UNLOCK();", 0, submit_index) >= 0, (
    "submission must not run while the persistent-custody lock is held"
)
assert submit_index < retry.index("survey_delivery_handle = delivery_handle")

poll = function_body(DISCOVERY, "survey_delivery_poll_comm_result")
assert "app_node_comm_peek_delivery_event_for(handle, &event)" in poll
assert "app_node_comm_take_delivery_event_for(handle, &event)" in poll
assert "survey_delivery_handle != handle" in poll, (
    "a stale terminal event must not mutate current survey custody"
)
assert "outbound = delivery->snapshot.outbound;" in poll
delivered_index = poll.index(
    "event.reason == NODE_COMM_TERMINAL_DELIVERED"
)
commit_index = poll.index(
    "app_anchor_survey_delivery_gateway_confirmed(",
    delivered_index,
)
digest_index = poll.index(
    "mesh_packet_semantic_digest(&outbound.packet", delivered_index
)
committed_check_index = poll.index(
    "app_mesh_local_delivery_ack_committed(delivery)", commit_index
)
take_index = poll.index(
    "app_node_comm_take_delivery_event_for(handle, &event)",
    committed_check_index,
)
cleanup_index = poll.index(
    "app_mesh_local_delivery_cleanup_ack(delivery)", take_index
)
assert (
    delivered_index
    < digest_index
    < commit_index
    < committed_check_index
    < take_index
    < cleanup_index
), (
    "DELIVERED must remain a read-only peek until exact ACK_COMMITTED "
    "persistence succeeds, then consume the terminal before tombstone cleanup"
)
retain_index = poll.index("survey_delivery_retain_for_retry_locked(delivery)")
assert retain_index < take_index, (
    "terminal communication failure must persist RETRY and renew an exhausted "
    "attempt tranche before consuming its event"
)
assert "app_mesh_local_delivery_discard_failed(delivery)" not in poll
assert "retained=%u" in poll and "1u" in poll

confirmed = function_body(
    DISCOVERY, "app_anchor_survey_delivery_gateway_confirmed"
)
assert "app_mesh_local_delivery_commit_ack(" in confirmed
assert "delivery, packet, semantic_digest" in confirmed
assert "app_mesh_local_delivery_cleanup_ack(" not in confirmed, (
    "the ACK callback may persist semantic acceptance but terminal polling "
    "owns the later journal tombstone"
)
assert confirmed.index("app_mesh_local_delivery_commit_ack(") < (
    confirmed.index("app_stack_workload_diag_anchor_survey_release(packet")
), "delivered custody clears transactionally before terminal diagnostics"
assert "app_mesh_local_delivery_identity_matches(&identity, packet)" in confirmed
assert "survey_delivery_semantic_digest_matches(delivery," in confirmed
assert "-EBADMSG" in confirmed, (
    "a wrapped packet header cannot commit a journal that owns different bytes"
)

local_delivery = (ROOT / "app/src/app_mesh_local_delivery.c").read_text()
stage = function_body(local_delivery, "app_mesh_local_delivery_stage")
recover = function_body(local_delivery, "app_mesh_local_delivery_recover")
assert "app_mesh_local_delivery_occupied(delivery)" in stage, (
    "an ACK_COMMITTED tombstone debt must block reuse of the singleton journal"
)
assert "app_mesh_local_delivery_ack_committed(delivery)" in recover
assert "recovery->retry_required = true" in recover, (
    "boot-time ACK tombstone failure is cleanup debt, not RF replay or "
    "destructive quarantine"
)

assert "app_node_comm_retry_backoff_ms(" in DISCOVERY
assert "NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK" in DISCOVERY
assert "DBG_SURVEY_REPORT_BACKOFF" in DISCOVERY
released = function_body(
    DISCOVERY, "app_anchor_survey_delivery_transport_released"
)
assert "survey_delivery_next_retry_delay_ms(" in released
assert "schedule_work_ms(0u)" not in released, (
    "a released RF attempt must use randomized exponential backoff"
)
delivery_service = function_body(
    NODE_COMM_APP, "app_node_comm_service_deliveries"
)
assert delivery_service.count("node_comm_lease_defer_pre_rf_retry(") == 2
assert delivery_service.count("node_comm_lease_defer_pre_rf(") == 1
assert delivery_service.count("node_comm_lease_wait_resource(") == 1
single_flight_wait = delivery_service.index(
    "node_comm_lease_wait_resource("
)
assert delivery_service.index(
    "node_comm_reliable_uplink_inflight_handle != 0u"
) < single_flight_wait
assert "node_comm_release_resource_wait(" in NODE_COMM_APP, (
    "single-flight occupancy is a resource wait, so owner release must wake "
    "blocked protocol and uplink deliveries without consuming a retry"
)
assert re.search(
    r"if\s*\(\s*scheduled_retry_delay_ms\s*>\s*0u\s*\).*?"
    r"node_comm_lease_defer_pre_rf\s*\(.*?not_before_ms",
    delivery_service,
    re.S,
), (
    "only an exact backend-scheduled radio boundary may use deterministic "
    "pre-RF deferral"
)
assert delivery_service.index("node_comm_lease_defer_pre_rf(") < (
    delivery_service.index("node_comm_lease_defer_pre_rf_retry(",
                           delivery_service.index(
                               "node_comm_lease_defer_pre_rf("))
), "unscheduled retryable deferrals must retain randomized retry policy"

survey_rx = function_body(DISCOVERY, "receive_survey_probes_until")
assert "dwm3000_driver_receive_frame_continuous(" in survey_rx, (
    "survey discovery must keep RX armed for the complete listen interval"
)
assert "dwm3000_driver_receive_frame(" not in survey_rx, (
    "the short preamble-hunt API collapses the survey listen interval"
)
probe_decode = survey_rx.index("uwb_decode_survey_discovery_probe(")
probe_generation_check = survey_rx.index(
    "probe.operation_generation != config->operation_generation"
)
reach_commit = survey_rx.index("survey_add_reach_entry(")
assert probe_decode < probe_generation_check < reach_commit, (
    "a decoded probe must match the complete active operation generation "
    "before it can mutate retained reachability"
)

measured = re.search(
    r"#define SURVEY_DISCOVERY_PHY_PREP_MEASURED_MAX_MS\s+(\d+)u", CONFIG
)
margin = re.search(
    r"#define SURVEY_DISCOVERY_PHY_PREP_MARGIN_MS\s+(\d+)u", CONFIG
)
assert measured is not None and int(measured.group(1)) == 63
assert margin is not None and int(margin.group(1)) >= 40
assert "SURVEY_DISCOVERY_PHY_PREP_MEASURED_MAX_MS +" in CONFIG
assert "default survey start delay must allow early PHY preparation" in CONFIG

start = function_body(DISCOVERY, "app_anchor_survey_discovery_handle_start")
custody_status = function_body(
    DISCOVERY, "app_anchor_survey_discovery_report_custody_status"
)
timing_index = start.index("survey_discovery_timing_from_age(")
start_at_index = start.index("survey_discovery_start_at_ms(")
queue_index = start.index("discovery_ops.queue_start(")
assert timing_index < start_at_index < queue_index, (
    "survey timing must be populated before reconstructing the absolute start"
)
queue_call_index = start.index("ret = discovery_ops.queue_start(&config")
queue_call_end = start.index(";", queue_call_index)
queue_call = start[queue_call_index:queue_call_end]
assert "start_at_ms" in queue_call and "schedule_delay_ms" in queue_call
assert "uptime_ms_until_deadline(now_ms, start_at_ms)" in start
runtime_worker = function_body(SURVEY_RUNTIME, "survey_work_handler")
custody_gate_index = runtime_worker.index(
    "app_anchor_survey_discovery_report_custody_status("
)
pending_index = runtime_worker.index("if (discovery_pending)")
assert pending_index < custody_gate_index, (
    "the private worker must recheck older report custody before taking a "
    "queued discovery generation"
)
assert "app_mesh_local_delivery_occupied(delivery)" in custody_status, (
    "a later survey must observe both unacknowledged report custody and "
    "committed ACK cleanup debt"
)
assert "survey_report_stage_retry_valid" in custody_status
assert custody_status.count("survey_operation_generation_extract_tlv(") == 2
for destructive_api in (
    "app_node_comm_abandon_delivery(",
    "app_mesh_local_delivery_note_failed(",
    "app_mesh_local_delivery_discard_failed(",
    "app_mesh_local_delivery_cleanup_ack(",
    "memset(&survey_report_stage_retry_outbound",
):
    assert destructive_api not in custody_status, (
        "a newer survey may inspect old report custody but must not mutate it: "
        f"{destructive_api}"
    )
assert "operation_generation == owned_generation ?" in custody_status

generation_admit = function_body(
    SURVEY_RUNTIME, "survey_generation_admit_locked"
)
generation_custody = generation_admit.index(
    "app_anchor_survey_discovery_report_custody_status("
)
generation_persist = generation_admit.index(
    "app_mesh_persistence_advance_anchor_survey_generation("
)
assert generation_custody < generation_persist, (
    "durable report custody G must reject G+1 before the anchor persists or "
    "publishes the newer generation"
)
assert "custody_status != -EALREADY" in generation_admit, (
    "same-generation pair work may continue while a different generation "
    "remains blocked"
)
worker_conflict = runtime_worker.index("if (ret < 0)", custody_gate_index)
worker_return = runtime_worker.index("return;", worker_conflict)
worker_conflict_block = runtime_worker[worker_conflict:worker_return]
for destructive_api in (
    "app_node_comm_abandon_delivery(",
    "app_mesh_local_delivery_note_failed(",
    "app_mesh_local_delivery_discard_failed(",
):
    assert destructive_api not in worker_conflict_block
assert "discovery_pending = false" in worker_conflict_block
assert "DBG_SURVEY_DISCOVERY_REPORT_CUSTODY_BLOCKED" in worker_conflict_block

restore = function_body(
    DISCOVERY, "app_anchor_survey_discovery_restore"
)
retry = function_body(
    DISCOVERY, "app_anchor_survey_discovery_retry_report"
)
assert "app_mesh_local_delivery_recover(" in restore
assert "recovery.restored" in restore
assert "app_mesh_local_delivery_rebase_after_boot(" in restore
assert "app_node_comm_submit_delivery(" in retry
assert "app_mesh_local_delivery_discard_failed(delivery)" not in (
    custody_status + worker_conflict_block
), (
    "after reset, restored G custody must remain deliverable and must not be "
    "discarded when G+1 is received"
)

drain = function_body(REPORT, "mesh_drain_rx_queue_locked")
actions_index = drain.index("mesh_handle_result_actions(")
delivery_index = drain.index("mesh_report_anchor_handle_survey_discovery_start(")
refresh_indices = [
    match.start()
    for match in re.finditer(r"mesh_rx_pending_refresh_age\(", drain)
]
assert any(actions_index < index < delivery_index for index in refresh_indices), (
    "local delivery must refresh message age after a potentially long relay action"
)

run = function_body(DISCOVERY, "app_anchor_survey_discovery_run")
assert run.count("dwm3000_driver_configure_wake_mode(") == 1, (
    "each discovery run must perform exactly one deliberate full wake-PHY configure"
)
assert "dwm3000_driver_idle(" in run and "sleep_until_ms(start_ms)" in run
assert "sleep_with_uwb_standby_until_ms(" not in run
send_index = run.index("send_local_survey_probe(")
ensure_index = run.index("dwm3000_driver_ensure_wake_mode(", send_index)
post_tx_rx_index = run.index("receive_survey_probes_until(", ensure_index)
assert send_index < ensure_index < post_tx_rx_index
probe_send = function_body(DISCOVERY, "send_local_survey_probe")
assert "dwm3000_driver_send_frame_tracked_until(" in probe_send
assert "absolute_tx_deadline_ms" in probe_send
assert ".operation_generation = config->operation_generation" in probe_send, (
    "every physical survey probe must carry the complete accepted operation "
    "generation"
)
assert "survey_expand_future_uptime32(" in run
assert "nominal.latest_tx_start_ms + 1u" in run
assert run.count("receive_survey_probes_until(") >= 2, (
    "discovery must listen both before and after its own probe airtime"
)

ensure_wake = function_body(DRIVER, "dwm3000_driver_ensure_wake_mode")
assert "ensure_phy_mode(DWM3000_PHY_WAKE)" in ensure_wake
assert "configure_radio_from_reset(" not in ensure_wake

receive_response = function_body(DRIVER, "receive_response")
assert "read_rx_diagnostics(" not in receive_response
assert "capture_rx_diag_raw(" not in receive_response
initiator = function_body(DRIVER, "dwm3000_driver_range_initiator")
delayed_final = initiator.index("DWT_START_TX_DELAYED")
assert "request->capture_rsl" not in initiator[:delayed_final], (
    "optional diagnostics must stay out of the RESP-to-delayed-FINAL path"
)
response_wait = initiator.index("receive_response(")
poll_timestamp = initiator.index("capture_completed_tx_timestamp(")
assert poll_timestamp < response_wait, (
    "the poll TX timestamp must be captured during the poll-to-RESP interval"
)
final_prestage = initiator.index('take_port_error("final-prestage")')
assert poll_timestamp < final_prestage < response_wait, (
    "the invariant FINAL bytes must be staged before waiting for RESP"
)
final_build = initiator.index("final_tx_time =", response_wait)
critical_path = initiator[final_build:delayed_final]
assert "patch_tx_frame(" in critical_path
assert "start_prepared_range_frame(" in critical_path
assert "send_range_frame(" not in critical_path, (
    "the full FINAL frame write must stay out of delayed-TX arm headroom"
)
assert "clear_status(" not in critical_path, (
    "receive_response already clears TXFRS before FINAL preparation"
)
assert "dwt_setpreambledetecttimeout(" not in critical_path, (
    "the unchanged delayed-RX preamble timeout must not be rewritten"
)
assert "read_tx_timestamp_u64(" not in critical_path, (
    "timestamp SPI reads must stay out of the RESP-to-delayed-FINAL path"
)
assert "status_debug_printf(" not in critical_path, (
    "RTT formatting/output must stay out of the RESP-to-delayed-FINAL path"
)
delayed_final_arm = initiator.index("start_prepared_range_frame(", final_build)
post_arm_diagnostics = initiator.index("if (request->capture_rsl)", delayed_final_arm)
assert delayed_final_arm < post_arm_diagnostics
assert "read_rx_diagnostics(" in initiator[post_arm_diagnostics:]
assert "capture_rx_diag_raw(" in initiator[post_arm_diagnostics:]
assert_debug_record_fits(initiator, "stage=final-armed", (10, 3, 3, 1))
assert_debug_record_fits(initiator, "stage=final-raw", (3,))
responder = function_body(DRIVER, "responder_poll_once")
response_arm = responder.index("DWT_START_TX_DELAYED")
matched_poll = responder.index("reply_delay_uus = request_reply_delay_uus(")
responder_critical_path = responder[matched_poll:response_arm]
assert "status_debug_printf(" not in responder_critical_path, (
    "RTT formatting/output must stay out of the POLL-to-delayed-RESP path"
)
assert_debug_record_fits(responder, "stage=resp-armed", (10, 3, 3))
assert_debug_record_fits(responder, "stage=resp-raw", (3, 5))
survey_initiator = function_body(SURVEY_RUNTIME, "run_pair_initiator")
assert "request.capture_rsl = false" in survey_initiator

restore = function_body(PERSISTENCE, "app_mesh_persistence_restore_local_delivery")
assert "app_mesh_local_delivery_snapshot_valid(snapshot)" in restore
assert "return -EBADMSG" in restore

prepare_report = function_body(DISCOVERY, "prepare_discovery_report")
stage_call = prepare_report.index("app_mesh_local_delivery_stage(delivery,")
inactive_guard = prepare_report.index(
    "!app_mesh_local_delivery_active(delivery)", stage_call
)
retry_copy = prepare_report.index(
    "survey_report_stage_retry_outbound = outbound", inactive_guard
)
retry_generation = prepare_report.index(
    "survey_report_stage_retry_generation = operation_session_id", retry_copy
)
retry_valid = prepare_report.index(
    "survey_report_stage_retry_valid = true", retry_generation
)
assert stage_call < inactive_guard < retry_copy < retry_generation < retry_valid, (
    "a failed first journal write must retain the exact encoded report and its "
    "survey generation before returning pressure to the runtime"
)

retry_report = function_body(
    DISCOVERY, "app_anchor_survey_discovery_retry_report"
)
retry_guard = retry_report.index("if (survey_report_stage_retry_valid &&")
retry_inactive = retry_report.index(
    "!app_mesh_local_delivery_active(delivery)", retry_guard
)
retry_stage = retry_report.index(
    "app_mesh_local_delivery_stage(", retry_inactive
)
retry_outbound = retry_report.index(
    "&survey_report_stage_retry_outbound", retry_stage
)
retry_generation_arg = retry_report.index(
    "survey_report_stage_retry_generation", retry_outbound
)
retry_success = retry_report.index("if (ret == 0)", retry_generation_arg)
retry_clear_outbound = retry_report.index(
    "memset(&survey_report_stage_retry_outbound", retry_success
)
retry_clear_generation = retry_report.index(
    "survey_report_stage_retry_generation = 0u", retry_clear_outbound
)
retry_clear_valid = retry_report.index(
    "survey_report_stage_retry_valid = false", retry_clear_generation
)
retry_failure = retry_report.index("} else {", retry_clear_valid)
retry_reschedule = retry_report.index(
    "schedule_work_ms(REPORT_TX_RETRY_DELAY_MS)", retry_failure
)
assert (
    retry_guard
    < retry_inactive
    < retry_stage
    < retry_outbound
    < retry_generation_arg
    < retry_success
    < retry_clear_outbound
    < retry_clear_generation
    < retry_clear_valid
    < retry_failure
    < retry_reschedule
), (
    "stage retry must replay the exact cached packet and may clear it only after "
    "durable journal admission; pressure must leave custody intact and reschedule"
)
retry_failure_slice = retry_report[retry_failure:retry_reschedule]
for forbidden_clear in (
    "memset(&survey_report_stage_retry_outbound",
    "survey_report_stage_retry_generation = 0u",
    "survey_report_stage_retry_valid = false",
):
    assert forbidden_clear not in retry_failure_slice, (
        "stage pressure must not discard cached exact-report custody"
    )

staged_probe = function_body(
    DISCOVERY, "app_anchor_survey_discovery_report_staged"
)
assert "app_mesh_local_delivery_active(delivery)" in staged_probe
assert "delivery->snapshot.generation == operation_session_id" in staged_probe, (
    "runtime release must observe durable custody for the exact survey generation"
)

discovery_restore = function_body(
    DISCOVERY, "app_anchor_survey_discovery_restore"
)
assert "app_mesh_local_delivery_rebase_after_boot(" in discovery_restore, (
    "persisted boot-relative report timing must be invalidated after reset"
)
assert "discovery_ops.seed_sequence(outbound->packet.seq)" in discovery_restore, (
    "restored report identity must advance the next local sequence"
)
rebase = function_body(
    (ROOT / "app/src/app_mesh_local_delivery.c").read_text(),
    "app_mesh_local_delivery_rebase_after_boot",
)
assert "candidate.outbound.queued_at_valid = false" in rebase
assert "candidate.outbound.earliest_tx_valid = false" in rebase

bounded_control = function_body(REPORT, "mesh_try_send_c5_flood_view")
handoff_begin = bounded_control.index("mesh_rx_handoff_begin_control(")
handoff_wait = bounded_control.index("mesh_rx_handoff_wait_for_control(")
control_send = bounded_control.index("mesh_send_c5_flood_now_until(")
handoff_end = bounded_control.index("mesh_rx_handoff_end_control(")
scan_restart = bounded_control.index("mesh_restart_role_scan(")
assert handoff_begin < handoff_wait < control_send < handoff_end < scan_restart, (
    "bounded gateway control must own the RX handoff through its complete send"
)
assert re.search(
    r"mesh_send_c5_flood_now_until\s*\([^;]+?\btrue\s*,\s*"
    r"NULL\s*,\s*NULL\s*,\s*NULL\s*,\s*"
    r"view->absolute_deadline_ms\s*,\s*observation\s*\)",
    bounded_control,
    re.S,
), "node-communication control attempts must request one lower-layer opportunity"

control_backend = function_body(NODE_COMM_APP, "app_node_comm_service_deliveries")
assert "mesh_try_send_c5_flood_view(" in control_backend
assert "app_mesh_flood_send_bounded(" not in control_backend

command_result = function_body(ANCHOR, "anchor_submit_command_result")
assert "command_id == CMD_SURVEY_PREPARE_PAIR" in command_result
assert "command_id == CMD_SURVEY_START_PAIR" in command_result
assert "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS" in command_result
assert "GATEWAY_COMMAND_RESULT_TIMEOUT_MS" in command_result
assert re.search(
    r"app_node_comm_submit_protocol_response\s*\(\s*&outbound\s*,\s*"
    r"absolute_deadline_ms",
    command_result,
    re.S,
), (
    "survey command results must carry the selected end-to-end deadline into "
    "protocol-response custody"
)

pair_failure = function_body(ANCHOR, "gateway_survey_auto_log_skipped_pair")
assert "failed_command_id" in pair_failure
assert "event.anchor_id = failed_target_id" in pair_failure
assert "event.progress_count" in pair_failure
assert "event.total_count" in pair_failure
assert "event.attempt" in pair_failure
survey_worker = function_body(ANCHOR, "gateway_survey_work_handler")
assert re.search(
    r"gateway_survey_auto_log_skipped_pair\s*\(\s*\"send-failed\".*?"
    r"action\.command_id\s*,\s*action\.target_id\s*\)",
    survey_worker,
    re.S,
), "send admission failures must identify the exact survey phase and target"

gateway_rx_worker = function_body(REPORT, "mesh_uwb_rx_work_handler")
assert gateway_rx_worker.count("mesh_rx_radio_start(") == 2
assert gateway_rx_worker.count("mesh_rx_radio_stop(") == 2
assert "mesh_transport_radio_start(" not in gateway_rx_worker

slice_start = gateway_rx_worker.index(
    "uint32_t gateway_rx_slice_deadline_ms"
)
slice_end = gateway_rx_worker.index(
    "gateway_route_preempt = mesh_gateway_route_test_preempt_active",
    slice_start,
)
continuous_slice = gateway_rx_worker[slice_start:slice_end]
for required_slice_boundary in (
    "APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS",
    "gateway_rx_slice_deadline_ms",
    "app_mesh_rx_policy_gateway_ch9_work_slice_ms(",
    "recoverable_errors_in_slice++",
    "app_mesh_rx_policy_gateway_ch9_should_yield_recovery(",
    "app_mesh_rx_policy_gateway_ch9_rearm_delay_ms()",
):
    assert required_slice_boundary in continuous_slice, (
        "continuous gateway RX omitted bounded workqueue slice boundary: "
        f"{required_slice_boundary}"
    )
assert continuous_slice.index(
    "app_mesh_rx_policy_gateway_ch9_work_slice_ms("
) < continuous_slice.index(
    'mesh_rx_radio_start("mesh gateway continuous channel9 RX")'
), "each driver receive must be clipped to the current workqueue slice"
assert continuous_slice.index(
    "recoverable_errors_in_slice++"
) < continuous_slice.index(
    "app_mesh_rx_policy_gateway_ch9_should_yield_recovery("
), "immediate recoverable errors must count before the yield decision"
post_slice_done = continuous_slice[
    continuous_slice.index("DBG_GATEWAY_CH9_RX_CONT_DONE") :
]
assert re.search(
    r"mesh_schedule_uwb_rx\s*\(\s*"
    r"app_mesh_rx_policy_gateway_ch9_rearm_delay_ms\s*\(\s*\)\s*\)",
    post_slice_done,
), (
    "clean timeout and immediate-error threshold exits must share one "
    "positive cooperative post-slice rearm"
)
assert "recovery_yield ?" not in post_slice_done, (
    "clean continuous-RX slices must not bypass cooperative workqueue yield"
)

continuous_start = gateway_rx_worker.index(
    'ret = mesh_rx_radio_start("mesh gateway continuous channel9 RX")'
)
continuous_guard_start = gateway_rx_worker.index(
    "if (ret < 0)", continuous_start
)
continuous_guard = braced_block(gateway_rx_worker, continuous_guard_start)
assert "if (ret == -ECANCELED)" in continuous_guard, (
    "continuous gateway RX start cancellation must have a dedicated "
    "safe-boundary handoff branch"
)
cancel_start = continuous_guard.index("if (ret == -ECANCELED)")
cancel_boundary = braced_block(continuous_guard, cancel_start)
assert cancel_boundary.count(
    "app_node_comm_gateway_delivery_safe_boundary()"
) == 1, (
    "start-time gateway RX cancellation must acknowledge exactly one "
    "node-communication delivery boundary"
)
assert "mesh_schedule_uwb_rx(" not in cancel_boundary, (
    "start-time cancellation must transfer radio ownership instead of "
    "trying to rearm RX while the handoff gate is pending"
)
assert cancel_boundary.index(
    "app_node_comm_gateway_delivery_safe_boundary()"
) < cancel_boundary.index("return;"), (
    "gateway delivery boundary must be acknowledged before RX worker exits"
)

rx_schedule = function_body(REPORT, "mesh_schedule_uwb_rx")
assert "mesh_rx_handoff_scan_rearm_allowed()" in rx_schedule, (
    "continuous RX must not rearm while bounded control owns the radio"
)

accepted_result = function_body(
    ANCHOR, "gateway_survey_auto_note_command_result"
)
close_call = accepted_result.index("ret = gateway_survey_complete_accepted_delivery()")
eagain = accepted_result.index("if (ret == -EAGAIN)")
abandon = accepted_result.index("gateway_survey_abandon_current(", eagain)
assert "gateway_survey_work_reschedule(" in accepted_result[eagain:abandon], (
    "an accepted early result must wait for an active backend call to return "
    "through the route-owner queue"
)
assert "memset(&gateway_survey_result_preflight" not in accepted_result[close_call:eagain], (
    "accepted result identity must remain cached while terminal take is deferred"
)
delivery_poll = function_body(ANCHOR, "gateway_survey_service_active_delivery")
assert "gateway_survey_result_preflight.valid" in delivery_poll
assert "gateway_survey_auto_note_command_result(" in delivery_poll, (
    "terminal polling must resume the already accepted semantic result"
)

anchor_start = function_body(ANCHOR, "app_anchor_start_anchor_role")
restore_journal = anchor_start.index("app_anchor_survey_discovery_restore(")
bind_journal = anchor_start.index("app_anchor_survey_discovery_retry_report(")
resume_outbox = anchor_start.index("mesh_report_resume_restored_outbox(")
assert restore_journal < bind_journal < resume_outbox, (
    "restored survey custody must bind to node communication before outbox RF resumes"
)
assert "if (!delivery_restored || delivery_bind_ret == 0)" in anchor_start

print("survey delivery source invariants passed")
