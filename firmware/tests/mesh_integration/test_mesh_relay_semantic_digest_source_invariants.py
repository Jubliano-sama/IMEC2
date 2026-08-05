#!/usr/bin/env python3
"""Source guards for full-width relay custody and replay commitments."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "include" / "mesh_relay.h").read_text()
ROUTE_PATH_HEADER = (ROOT / "include" / "mesh_route_path.h").read_text()
PROTOCOL_HEADER = (ROOT / "include" / "protocol.h").read_text()
PROTOCOL_SOURCE = (ROOT / "src" / "protocol.c").read_text()
MESH_SOURCE = (ROOT / "src" / "mesh.c").read_text()
ROUTE_HEADER = (ROOT / "include" / "route.h").read_text()
ROUTE_SOURCE = (ROOT / "src" / "route.c").read_text()
RELAY_SOURCE = (ROOT / "src" / "mesh_relay.c").read_text()
CUSTODY_SOURCE = (ROOT / "src" / "mesh_relay_custody.inc").read_text()
DELIVERY_SOURCE = (ROOT / "src" / "mesh_relay_delivery.inc").read_text()
ROUTE_RX_SOURCE = (ROOT / "src" / "mesh_relay_route_rx.inc").read_text()
ROUTES_SOURCE = (ROOT / "src" / "mesh_relay_routes.inc").read_text()
APP_ROUTE_SOURCE = (
    ROOT / "app" / "src" / "app_mesh_report_route_control.inc"
).read_text()
APP_COORDINATION_SOURCE = (
    ROOT / "app" / "src" / "app_mesh_report_coordination.inc"
).read_text()
APP_DELIVERY_SOURCE = (
    ROOT / "app" / "src" / "app_mesh_report_delivery.inc"
).read_text()
APP_REPORT_SOURCE = (
    ROOT / "app" / "src" / "app_mesh_report.c"
).read_text()
APP_TRANSPORT_SOURCE = (
    ROOT / "app" / "src" / "app_mesh_report_transport.inc"
).read_text()
APP_ANCHOR_INIT_SOURCE = (
    ROOT / "app" / "src" / "app_anchor_init.inc"
).read_text()
APP_RX_SOURCE = (
    ROOT / "app" / "src" / "app_mesh_report_rx.inc"
).read_text()
APP_ACK_SOURCE = (ROOT / "app" / "src" / "app_mesh_ch9_ack.c").read_text()
SIM_HEADER = (ROOT / "sim" / "mesh_sim.h").read_text()
SIM_RELAY_SOURCE = (ROOT / "sim" / "mesh_sim_relay.c").read_text()
TEST_SOURCE = (ROOT / "tests" / "test_mesh_relay.c").read_text()
ROUTE_ADV_POLICY_TEST = (
    ROOT / "tests" / "test_mesh_route_adv_policy.c"
).read_text()
MESH_TEST_SOURCE = (ROOT / "tests" / "test_mesh.c").read_text()
APP_ACK_TEST_SOURCE = (ROOT / "tests" / "test_app_mesh_ch9_ack.c").read_text()
ROUTE_FORMATION_TEST_SOURCE = (
    ROOT / "tests" / "mesh_integration" /
    "test_mesh_route_formation_scenarios.c"
).read_text()


def between(source: str, start: str, end: str) -> str:
    return source[source.index(start) : source.index(end, source.index(start))]


duplicate_entry = between(
    HEADER, "struct mesh_duplicate_entry {", "struct mesh_command_replay_window"
)
command_replay_window = between(
    HEADER,
    "struct mesh_command_replay_window {",
    "struct mesh_gateway_ack_identity_entry {",
)
ack_entry = between(
    HEADER,
    "struct mesh_gateway_ack_identity_entry {",
    "struct mesh_gateway_ack_store {",
)
offer_reservation = between(
    HEADER,
    "struct mesh_result_offer_reservation {",
    "struct mesh_relay_child_custody_snapshot {",
)
route_reply_expectation = between(
    HEADER,
    "struct mesh_route_reply_ack_expectation {",
    "struct mesh_result_bundle_entry {",
)

for structure in (
    duplicate_entry,
    ack_entry,
    offer_reservation,
    route_reply_expectation,
):
    assert "SEMANTIC_DIGEST_SHA256_LEN" in structure

assert "payload_crc" not in duplicate_entry
assert "payload_len" not in duplicate_entry
assert "payload_crc" not in ack_entry
assert "payload_len" not in ack_entry
assert "result_crc" not in offer_reservation
assert "SEMANTIC_DIGEST_SHA256_LEN" in command_replay_window

command_replay_logic = between(
    RELAY_SOURCE,
    "enum broadcast_command_replay_classification {",
    "static bool broadcast_command_replay_rollback(",
)
assert "BROADCAST_COMMAND_REPLAY_CONFLICT" in command_replay_logic
assert "semantic_digest_equal(window->newest_semantic_digest" in (
    command_replay_logic
)
assert "relay_semantic_packet_digest(" in command_replay_logic

duplicate_logic = between(
    RELAY_SOURCE,
    "static enum duplicate_classification duplicate_classify(",
    "static void duplicate_store_payload_identity(",
)
ack_history_logic = between(
    RELAY_SOURCE,
    "static bool gateway_ack_history_seen(",
    "static int gateway_ack_history_accept_generic(",
)
offer_match_logic = between(
    RELAY_SOURCE,
    "static bool result_offer_reservation_matches_offer(",
    "static int build_result_offer_from_pending(",
)
assert "proto_crc16_ccitt_false" not in duplicate_logic
assert "proto_crc16_ccitt_false" not in ack_history_logic
assert "proto_crc16_ccitt_false" not in offer_match_logic
assert duplicate_logic.count("semantic_digest_equal(") >= 1
assert ack_history_logic.count("semantic_digest_equal(") >= 3
assert offer_match_logic.count("semantic_digest_equal(") >= 2

assert "uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]" in HEADER
assert "MESH_RELAY_OUTBOX_SNAPSHOT_VERSION 4u" in HEADER
assert "uint32_t route_epoch;" in HEADER[
    HEADER.index("struct mesh_relay_outbox_snapshot {") :
    HEADER.index(
        "struct mesh_route_discovery_state {",
        HEADER.index("struct mesh_relay_outbox_snapshot {"),
    )
]
assert "MESH_RELAY_CHILD_CUSTODY_SNAPSHOT_VERSION 2u" in HEADER
assert "semantic_digest_equal(record->semantic_digest" in DELIVERY_SOURCE
assert "semantic_digest_equal(entry->semantic_digest" in DELIVERY_SOURCE
assert "result_bundle_entry_classify" in CUSTODY_SOURCE
assert "semantic_digest_equal(queue->records[i].semantic_digest" in CUSTODY_SOURCE

assert "TLV_RESULT_SHA256_COMMITMENT = 0xB4" in PROTOCOL_HEADER
assert "TLV_ROUTE_REPLY_SHA256_COMMITMENT = 0xB5" in PROTOCOL_HEADER
assert "TLV_MESH_ACK_SEMANTIC_IDENTITY = 0xB8" in PROTOCOL_HEADER
assert "uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN]" in PROTOCOL_HEADER
assert "payload_cap - *offset < RESULT_OFFER_TLV_BYTES" in PROTOCOL_SOURCE
assert "result_digest_len != SEMANTIC_DIGEST_SHA256_LEN" in PROTOCOL_SOURCE
assert "offer.result_id.node_id != packet->src_id" in CUSTODY_SOURCE
assert TEST_SOURCE.count(
    "test_result_offer_rejects_cross_child_and_zero_identity"
) == 2

pending_ack_match = between(
    CUSTODY_SOURCE,
    "static bool pending_ack_matches(",
    "static bool gateway_ack_targets_pending_transit_origin(",
)
gateway_ack_builder = between(
    CUSTODY_SOURCE,
    "static int build_gateway_ack(",
    "static int build_hop_ack(",
)
hop_ack_builder = between(
    CUSTODY_SOURCE,
    "static int build_hop_ack(",
    "static int parse_relay_busy_tlvs(",
)
transit_ack_commit = between(
    CUSTODY_SOURCE,
    "int mesh_relay_commit_transit_gateway_ack_forward(",
    "static int build_forward(",
)
app_ack_apply = between(
    APP_ACK_SOURCE,
    "int app_mesh_ch9_tx_ack_apply(",
    "int app_mesh_ch9_tx_requeue_unacked(",
)
app_ack_match_helper = between(
    APP_ACK_SOURCE,
    "static int ack_payload_contains_packet(",
    "int app_mesh_ch9_tx_ack_apply(",
)
app_direct_ack = between(
    APP_ACK_SOURCE,
    "bool app_mesh_direct_gateway_ack_matches(",
    "bool app_mesh_ch9_ack_complete_should_close_timing(",
)
assert "mesh_packet_semantic_digest(" in MESH_SOURCE
assert "mesh_ack_payload_contains_packet(" in pending_ack_match
assert "mesh_ack_payload_contains(" not in pending_ack_match
assert "packet->src_id != previous_hop_id" in pending_ack_match
assert "mesh_append_ack_semantic_identity(" in gateway_ack_builder
assert "mesh_append_ack_semantic_identity(" in hop_ack_builder
assert "mesh_ack_payload_contains_packet(" in transit_ack_commit
assert "mesh_ack_payload_contains_packet(" in app_ack_match_helper
assert "entries[i].outbound" in app_ack_apply
assert "ack_payload_contains_packet(" in app_direct_ack
assert "mesh_ack_payload_contains(" not in APP_ACK_SOURCE
assert TEST_SOURCE.count(
    "test_local_gateway_bound_tx_waits_for_gateway_ack"
) == 2
assert MESH_TEST_SOURCE.count(
    "test_ack_payload_requires_exact_semantic_identity"
) == 2
assert APP_ACK_TEST_SOURCE.count(
    "test_direct_gateway_ack_rejects_reused_id_different_payload"
) == 2
assert APP_ACK_TEST_SOURCE.count(
    "test_ack_table_rejects_same_id_different_semantic_packet"
) == 2

route_reply_validation = between(
    ROUTE_RX_SOURCE,
    "static int validate_route_reply_commitment(",
    "static int build_route_reply_ack(",
)
route_reply_ack_acceptance = between(
    ROUTE_RX_SOURCE,
    "int mesh_relay_accept_route_reply_ack(",
    "static int handle_route_reply_ack(",
)
route_reply_ack_builder = between(
    ROUTE_RX_SOURCE,
    "static int build_route_reply_ack(",
    "static int validate_route_reply_identity(",
)
app_route_reply_matcher = between(
    APP_ROUTE_SOURCE,
    "static bool mesh_route_reply_ack_matches(",
    "static int mesh_listen_for_route_reply_ack(",
)
app_route_reply_capture = APP_ROUTE_SOURCE[
    APP_ROUTE_SOURCE.index("static int mesh_listen_for_route_reply(") :
]
gateway_route_adv_validation = between(
    ROUTE_RX_SOURCE,
    "static int validate_gateway_route_adv_packet(",
    "int mesh_relay_validate_gateway_route_adv(",
)
gateway_route_epoch_expansion = between(
    ROUTE_RX_SOURCE,
    "static int gateway_route_epoch_expand(",
    "static int validate_gateway_route_adv_packet(",
)
gateway_route_adv_handler = between(
    ROUTE_RX_SOURCE,
    "static int handle_gateway_route_adv(",
    "static int build_route_reply(",
)
route_request_validation = between(
    ROUTE_RX_SOURCE,
    "static int validate_route_request_packet(",
    "int mesh_relay_validate_route_request(",
)
app_rx_queue_admission = between(
    APP_RX_SOURCE,
    "static bool mesh_queue_from_frame_at_internal(",
    "static bool mesh_queue_from_frame_at(",
)
app_rx_drain = APP_DELIVERY_SOURCE[
    APP_DELIVERY_SOURCE.index("static uint32_t mesh_drain_rx_queue_locked(") :
]
app_c5_semantic_gate = between(
    APP_COORDINATION_SOURCE,
    "static bool mesh_c5_control_rx_semantically_admitted(",
    "static const char *mesh_ch9_event_state_name(",
)
route_reply_delivery = between(
    DELIVERY_SOURCE,
    "if (packet->msg_type == MSG_ROUTE_REPLY && packet->dst_id != MESH_BROADCAST_ID) {",
    "if (packet->msg_type == MSG_GATEWAY_ROUTE_ADV && packet->dst_id == MESH_BROADCAST_ID) {",
)
app_route_reply_handoff = APP_DELIVERY_SOURCE[
    APP_DELIVERY_SOURCE.index(
        "if (route_reply_downstream_handoff_required) {"
    ) :
    APP_DELIVERY_SOURCE.index(
        "if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ)"
    )
]
app_gateway_route_adv_action = between(
    APP_DELIVERY_SOURCE,
    "if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) {",
    "if (result->actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY) {",
)
assert "route_reply_semantic_commitment(" in route_reply_validation
assert "semantic_digest_equal(" in route_reply_validation
assert "route_reply_ack_expectation.active" in route_reply_ack_acceptance
assert "semantic_digest_equal(" in route_reply_ack_acceptance
assert "TLV_ROUTE_REPLY_SHA256_COMMITMENT" in route_reply_ack_builder
assert "TLV_REPLY_NONCE" not in route_reply_ack_builder
assert "TLV_METRIC_CRC" not in route_reply_ack_builder
assert (
    "#define MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN \\\n"
    "    (PROTO_TLV_HEADER_LEN + SEMANTIC_DIGEST_SHA256_LEN)"
    in ROUTE_PATH_HEADER
)
assert "mesh_relay_accept_route_reply_ack(&mesh_runtime" in app_route_reply_matcher
assert "TLV_REPLY_NONCE" not in app_route_reply_matcher
assert "TLV_METRIC_CRC" not in app_route_reply_matcher
assert "int mesh_relay_validate_route_reply(" in HEADER
assert "int mesh_relay_validate_route_request(" in HEADER
assert "int mesh_relay_validate_gateway_route_adv(" in HEADER
assert (
    app_route_reply_capture.index("mesh_relay_validate_route_reply(")
    < app_route_reply_capture.index("mesh_queue_from_frame_at_internal(")
    < app_route_reply_capture.index(
        "app_mesh_c5_route_capture_completes_discovery("
    )
)
assert (
    app_route_reply_capture.index("mesh_relay_validate_gateway_route_adv(")
    < app_route_reply_capture.index("mesh_queue_from_frame_at_internal(")
    < app_route_reply_capture.index(
        "app_mesh_c5_route_capture_completes_discovery("
    )
)
assert "packet->session_id != fields->gateway_route_seq" in (
    gateway_route_adv_validation
)
assert "fields->flood_epoch_id != fields->gateway_route_seq" in (
    gateway_route_adv_validation
)
assert "gateway_route_adv_slot_seed(" in gateway_route_adv_validation
assert "fields->flood_packet_age_ms != packet->message_age_ms" in (
    gateway_route_adv_validation
)
assert "relay->role != MESH_RELAY_ROLE_ANCHOR" in (
    gateway_route_adv_validation
)
assert "relay->upstream.current_epoch" in gateway_route_adv_validation
assert "gateway_route_adv_required_tlvs_present(" in (
    gateway_route_adv_validation
)
assert "gateway_route_adv_sequence_strictly_newer(" in (
    gateway_route_adv_validation
)
assert "route_epoch_strictly_newer" in ROUTE_HEADER
assert "delta < UINT32_C(0x80000000)" in ROUTE_SOURCE
assert "delta >= UINT16_C(0x8000)" in gateway_route_epoch_expansion
assert "*expanded = current + delta;" in gateway_route_epoch_expansion
assert (
    gateway_route_adv_validation.index("gateway_route_epoch_expand(")
    < gateway_route_adv_validation.index(
        "gateway_route_adv_sequence_strictly_newer("
    )
)
assert "(uint16_t)next_epoch == 0u" in ROUTES_SOURCE
assert RELAY_SOURCE.count("static void commit_route_epoch_transition(") == 1
assert RELAY_SOURCE.count("clear_all_downlinks(relay);") == 1
assert RELAY_SOURCE.count(
    "memset(relay->event_timings, 0, sizeof(relay->event_timings));"
) == 1
assert RELAY_SOURCE.count("mesh_relay_reset_route_discovery(relay);") == 1
assert ROUTE_RX_SOURCE.count("commit_route_epoch_transition(") == 4
assert "test_gateway_route_epoch_serial_wrap_and_ambiguity" in TEST_SOURCE
assert "UINT32_MAX" in TEST_SOURCE[
    TEST_SOURCE.index("test_gateway_route_epoch_serial_wrap_and_ambiguity") :
    TEST_SOURCE.index(
        "static void test_route_ancestry_blocks_three_node_cycle",
        TEST_SOURCE.index("test_gateway_route_epoch_serial_wrap_and_ambiguity"),
    )
]
assert (
    gateway_route_adv_handler.index("build_gateway_route_adv_forward(")
    < gateway_route_adv_handler.index("upsert_reactive_route(")
    < gateway_route_adv_handler.index(
        "relay->gateway_route_adv_seq = fields.gateway_route_seq;"
    )
)
assert "rollback->gateway_route_adv_seq =" in CUSTODY_SOURCE
assert "relay->gateway_route_adv_seq =" in CUSTODY_SOURCE
assert "!admission->route_state_durable" in RELAY_SOURCE
assert "packet->flags != 0u" in route_request_validation
assert "fields->flood_epoch_id != packet->session_id" in (
    route_request_validation
)
assert "route_discovery_slot_seed(" in route_request_validation
assert "mesh_route_path_validate(" in route_request_validation
assert "route_discovery_required_tlvs_present(" in route_request_validation
assert "else if (packet->dst_id == relay->local_id)" in route_reply_delivery
assert (
    app_route_reply_handoff.index("mesh_send_route_reply_action(")
    < app_route_reply_handoff.index(
        "app_mesh_route_reply_upstream_ack_allowed("
    )
    < app_route_reply_handoff.index(
        "mesh_send_c5_control(route_reply_ack,"
    )
)
assert "downstream custody not confirmed" in app_route_reply_handoff
assert "struct mesh_outbound route_reply_upstream_ack;" in SIM_HEADER
assert "bool route_reply_upstream_ack_valid;" in SIM_HEADER
assert (
    SIM_RELAY_SOURCE.index("node->route_reply_upstream_ack =")
    < SIM_RELAY_SOURCE.index(
        "&result->route_reply);"
    )
    < SIM_RELAY_SOURCE.index(
        "MESH_RELAY_ACTION_ROUTE_REPLY_ACKED"
    )
    < SIM_RELAY_SOURCE.index(
        "&node->route_reply_upstream_ack);"
    )
)
assert "reply_to_origin_downstream_ack_timeout" in ROUTE_FORMATION_TEST_SOURCE
assert (
    "reply_to_origin_retry_releases_chained_custody"
    in ROUTE_FORMATION_TEST_SOURCE
)
assert "ttl_ladder_reply_ack_hop_%zu" in ROUTE_FORMATION_TEST_SOURCE
assert "mesh_note_c5_control_rx(" not in app_rx_queue_admission
assert (
    app_rx_drain.index("mesh_relay_validate_route_request(")
    < app_rx_drain.index(
        "mesh_gateway_route_test_should_reject_route_request("
    )
    < app_rx_drain.index("mesh_relay_handle_rx_with_random(")
    < app_rx_drain.index("mesh_c5_control_rx_semantically_admitted(")
    < app_rx_drain.index("mesh_handle_result_actions(")
)
assert (
    app_rx_drain.index("mesh_relay_handle_rx_with_random(")
    < app_rx_drain.index("app_mesh_route_state_save(&mesh_runtime)")
    < app_rx_drain.index(
        "mesh_relay_mark_route_state_durable(&mesh_runtime, result)"
    )
    < app_rx_drain.index("mesh_handle_result_actions(")
)
assert "mesh_rollback_c5_forward_admission(" not in app_gateway_route_adv_action
assert "mesh_route_adv_deferred" in APP_REPORT_SOURCE
assert "entry = &mesh_route_adv_deferred;" in APP_TRANSPORT_SOURCE
assert (
    APP_ANCHOR_INIT_SOURCE.index(
        "app_mesh_route_state_restore(&mesh_runtime)"
    )
    < APP_ANCHOR_INIT_SOURCE.index(
        "app_mesh_persistence_restore_outbox(&mesh_runtime"
    )
    < APP_ANCHOR_INIT_SOURCE.index(
        "app_mesh_persistence_restore_child_custody(&mesh_runtime"
    )
)
assert "result->status == PROTO_ERR_MALFORMED" in app_c5_semantic_gate
assert "return admitted_event_control;" in app_c5_semantic_gate
assert ROUTE_ADV_POLICY_TEST.count(
    "test_header_relevant_adv_must_pass_full_capture_admission"
) == 2
assert ROUTE_ADV_POLICY_TEST.count(
    "test_route_request_requires_full_read_only_admission"
) == 2
assert ROUTE_ADV_POLICY_TEST.count(
    "test_gateway_adv_sequence_freshness_is_commit_late_and_wrap_safe"
) == 2
assert TEST_SOURCE.count(
    "test_route_reply_ack_full_commitment_rejects_crc_collision_and_replay"
) == 2
assert TEST_SOURCE.count(
    "test_route_reply_requires_exact_commitment_before_route_mutation"
) == 2

assert "build_deliberate_crc16_collision_results" in TEST_SOURCE
for regression in (
    "test_crc16_collision_cannot_alias_duplicate_or_gateway_ack_history",
    "test_crc16_collision_cannot_cross_result_offer_reservation_snapshot",
    "test_crc16_collision_cannot_alias_outbox_snapshot_payload",
    "test_crc16_collision_cannot_alias_restored_result_bundle_entry",
):
    assert TEST_SOURCE.count(regression) == 2, regression

print("mesh relay semantic digest source invariants passed")
