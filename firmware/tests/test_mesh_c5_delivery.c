/*
 * Channel-5 delivery protocol: ACK depth/credit, explicit backpressure, batch
 * sequencing and local route repair.
 *
 * Documentation/Channel 5 Delivery Protocol.md sections 2, 3, 4 and 6.
 */
#include "mesh_relay.h"
#include "mesh.h"
#include "protocol.h"
#include "report.h"
#include "semantic_digest.h"
#include "uwb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ANCHOR_A 0x1111222233334444ull
#define ANCHOR_B 0x5555666677778888ull
#define ANCHOR_C 0x2222333344445555ull
#define GATEWAY  0x9999888877776666ull

static bool has_action(const struct mesh_relay_result *result,
                       uint32_t action)
{
    return (result->actions & action) != 0u;
}

static struct route_candidate upstream_route(uint64_t next_hop_id,
                                             uint32_t epoch,
                                             uint8_t hop_count,
                                             uint8_t quality)
{
    struct route_candidate candidate = {
        .next_hop_id = next_hop_id,
        .gateway_id = GATEWAY,
        .route_epoch = epoch,
        .last_seen_ms = 1000u,
        .hop_count = hop_count,
        .link_quality = quality,
        .valid = true,
    };
    return candidate;
}

static size_t build_click_report(struct proto_packet *packet,
                                 uint64_t anchor_id,
                                 uint32_t event_seq,
                                 uint16_t seq,
                                 uint8_t *payload,
                                 size_t payload_cap)
{
    const int32_t distance_samples_mm[] = {1200};
    const uint8_t range_round_indices[] = {0u};
    const uint64_t sequence_start_timestamps_ms[] = {1000u};
    const struct range_report_fields fields = {
        .clicker_id = ANCHOR_C,
        .anchor_id = anchor_id,
        .event_seq = event_seq,
        .timestamp_ms = 1000u,
        .distance_mm = 1200,
        .quality = 90u,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples_mm,
        .range_round_indices = range_round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = 1u,
        .distance_sample_count = 1u,
        .burst_id = event_seq,
        .burst_id_present = true,
        .omit_rsl = true,
        .omit_cir = true,
    };
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload,
                                    payload_cap,
                                    &payload_len,
                                    &fields) == PROTO_OK);
    assert(report_init_click_packet(packet,
                                    anchor_id,
                                    GATEWAY,
                                    proto_click_report_session_id(ANCHOR_C,
                                                                  event_seq),
                                    seq,
                                    (uint8_t)payload_len) == PROTO_OK);
    return payload_len;
}

/* ---------------------------------------------------------------- item 1 */

static void test_ack_flow_control_tlv_round_trip(void)
{
    struct mesh_ack_flow_control encoded = {
        .depth = 3u,
        .credit = 5u,
        .retry_after_ms = 250u,
        .depth_valid = true,
        .credit_valid = true,
        .retry_after_valid = true,
    };
    struct mesh_ack_flow_control decoded;
    struct proto_packet ack = {0};
    uint8_t payload[64];
    size_t offset = 0u;

    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &offset,
                                     7u) == PROTO_OK);
    assert(mesh_append_ack_flow_control(payload,
                                        sizeof(payload),
                                        &offset,
                                        &encoded) == PROTO_OK);
    assert(mesh_init_gateway_ack(&ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 0x1234u,
                                 9u,
                                 (uint8_t)offset) == PROTO_OK);

    assert(mesh_ack_flow_control_parse(&ack,
                                       payload,
                                       offset,
                                       &decoded) == PROTO_OK);
    assert(decoded.depth_valid && decoded.depth == 3u);
    assert(decoded.credit_valid && decoded.credit == 5u);
    assert(decoded.retry_after_valid && decoded.retry_after_ms == 250u);
    assert(decoded.identity_count == 0u);
    assert(!mesh_ack_flow_control_is_dead_end(&decoded));
    /* Credit five is not backpressure even with no identities listed. */
    assert(!mesh_ack_flow_control_is_backpressure(&decoded));
    printf("ok ack flow control tlv round trip\n");
}

static void test_ack_flow_control_missing_tlvs_are_unknown(void)
{
    struct mesh_ack_flow_control decoded;
    struct proto_packet ack = {0};
    uint8_t payload[64];
    size_t offset = 0u;

    /* Exactly what old firmware emits: requested seq only. */
    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &offset,
                                     7u) == PROTO_OK);
    assert(mesh_init_gateway_ack(&ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 0x1234u,
                                 9u,
                                 (uint8_t)offset) == PROTO_OK);
    assert(mesh_ack_flow_control_parse(&ack,
                                       payload,
                                       offset,
                                       &decoded) == PROTO_OK);
    assert(!decoded.depth_valid);
    assert(!decoded.credit_valid);
    assert(!decoded.retry_after_valid);
    assert(!mesh_ack_flow_control_is_dead_end(&decoded));
    assert(!mesh_ack_flow_control_is_backpressure(&decoded));
    printf("ok missing flow control tlvs read as unknown\n");
}

static void test_credit_reserves_one_slot_for_own_reports(void)
{
    assert(MESH_CUSTODY_OWN_RESERVE == 1u);
    assert(mesh_batch_credit_from_free_slots(0u) == 0u);
    assert(mesh_batch_credit_from_free_slots(1u) == 0u);
    assert(mesh_batch_credit_from_free_slots(2u) == 1u);
    assert(mesh_batch_credit_from_free_slots(9u) == 8u);
    assert(mesh_batch_credit_from_free_slots(1000u) == MESH_BATCH_CREDIT_MAX);
    printf("ok credit reserves one slot for own reports\n");
}

static void test_gateway_ack_carries_depth_zero_and_stream_credit(void)
{
    struct mesh_relay gateway;
    struct mesh_gateway_ack_store store;
    struct mesh_relay_result result;
    struct mesh_ack_flow_control flow;
    struct proto_packet report = {0};
    uint8_t payload[160];
    size_t payload_len;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 3u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &store) == PROTO_OK);
    /* The application reports the gateway's free BLE-stream record count. */
    mesh_relay_set_local_free_slots(&gateway, 5u);
    assert(mesh_relay_local_credit(&gateway) == 4u);
    assert(mesh_relay_local_depth(&gateway, 1000u) == 0u);

    payload_len = build_click_report(&report,
                                     ANCHOR_A,
                                     42u,
                                     9u,
                                     payload,
                                     sizeof(payload));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &report,
                                              payload,
                                              payload_len,
                                              ANCHOR_A,
                                              1000u,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_ack_flow_control_parse(&result.gateway_ack.packet,
                                       result.gateway_ack.payload,
                                       result.gateway_ack.payload_len,
                                       &flow) == PROTO_OK);
    assert(flow.depth_valid && flow.depth == 0u);
    assert(flow.credit_valid && flow.credit == 4u);
    assert(flow.identity_count == 1u);
    printf("ok gateway ack carries depth zero and stream credit\n");
}

static void test_hop_ack_carries_parent_depth(void)
{
    struct mesh_relay parent;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct mesh_ack_flow_control flow;
    struct route_candidate route = upstream_route(ANCHOR_C, 3u, 1u, 90u);
    struct proto_packet report = {0};
    uint8_t payload[160];
    size_t payload_len;

    mesh_relay_init(&parent, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&parent, &store) ==
           PROTO_OK);
    assert(route_upsert_candidate(&parent.upstream, &route) == PROTO_OK);
    /* Parent sits one hop below a depth-1 neighbour, so its depth is two. */
    assert(mesh_relay_local_depth(&parent, 1000u) == 2u);
    mesh_relay_set_local_free_slots(&parent, 3u);

    payload_len = build_click_report(&report,
                                     ANCHOR_A,
                                     42u,
                                     9u,
                                     payload,
                                     sizeof(payload));
    assert(mesh_relay_handle_rx(&parent,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(mesh_ack_flow_control_parse(&result.hop_ack.packet,
                                       result.hop_ack.payload,
                                       result.hop_ack.payload_len,
                                       &flow) == PROTO_OK);
    assert(flow.depth_valid && flow.depth == 2u);
    assert(flow.credit_valid && flow.credit == 2u);
    assert(flow.identity_count == 1u);
    printf("ok hop ack carries parent depth\n");
}

/* ------------------------------------------------------------ items 1 + 2 */

/* Bring an origin anchor to the point where it is waiting for a hop ACK. */
static size_t arm_pending_report(struct mesh_relay *origin,
                                 struct mesh_anchor_downlink_store *store,
                                 uint64_t parent_id,
                                 uint8_t parent_depth,
                                 struct proto_packet *report,
                                 uint8_t *payload,
                                 size_t payload_cap,
                                 struct mesh_outbound *sent)
{
    struct route_candidate route = upstream_route(parent_id,
                                                  3u,
                                                  parent_depth,
                                                  90u);
    size_t payload_len;

    mesh_relay_init(origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(origin, store) == PROTO_OK);
    assert(route_upsert_candidate(&origin->upstream, &route) == PROTO_OK);
    payload_len = build_click_report(report,
                                     ANCHOR_A,
                                     42u,
                                     9u,
                                     payload,
                                     payload_cap);
    assert(mesh_relay_start_tx(origin,
                               report,
                               payload,
                               payload_len,
                               1000u,
                               sent) == PROTO_OK);
    mesh_relay_note_tx_sent(origin, sent, 1000u);
    assert(origin->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(origin->pending.next_hop_id == parent_id);
    return payload_len;
}

/* An ACK that names nothing, with the responder's depth and credit. */
static size_t build_refusal_ack(struct proto_packet *ack,
                                uint8_t *payload,
                                size_t payload_cap,
                                uint64_t responder_id,
                                uint64_t origin_id,
                                const struct proto_packet *report,
                                uint8_t depth,
                                uint8_t credit,
                                uint16_t retry_after_ms,
                                bool retry_after_valid)
{
    struct mesh_ack_flow_control flow = {
        .depth = depth,
        .credit = credit,
        .retry_after_ms = retry_after_ms,
        .depth_valid = true,
        .credit_valid = true,
        .retry_after_valid = retry_after_valid,
    };
    size_t offset = 0u;

    assert(mesh_append_requested_seq(payload,
                                     payload_cap,
                                     &offset,
                                     report->seq) == PROTO_OK);
    assert(mesh_append_ack_flow_control(payload,
                                        payload_cap,
                                        &offset,
                                        &flow) == PROTO_OK);
    memset(ack, 0, sizeof(*ack));
    ack->msg_type = MSG_MESH_HOP_ACK;
    ack->flags = 0u;
    ack->src_id = responder_id;
    ack->dst_id = origin_id;
    ack->session_id = report->session_id;
    ack->seq = 5u;
    ack->ttl = MESH_GATEWAY_ACK_TTL;
    ack->payload_len = (uint16_t)offset;
    return offset;
}

static void test_dead_end_ack_holds_candidate_and_reselects(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct route_candidate alternate = upstream_route(ANCHOR_C, 3u, 0u, 80u);
    struct proto_packet report = {0};
    struct proto_packet ack = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];
    uint8_t ack_payload[64];
    size_t ack_payload_len;
    uint8_t failure_count_before = 0u;

    (void)arm_pending_report(&origin,
                             &store,
                             ANCHOR_B,
                             1u,
                             &report,
                             payload,
                             sizeof(payload),
                             &sent);
    assert(route_upsert_candidate(&origin.upstream, &alternate) == PROTO_OK);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (origin.upstream.candidates[i].valid &&
            origin.upstream.candidates[i].next_hop_id == ANCHOR_B) {
            failure_count_before = origin.upstream.candidates[i].failure_count;
        }
    }

    ack_payload_len = build_refusal_ack(&ack,
                                        ack_payload,
                                        sizeof(ack_payload),
                                        ANCHOR_B,
                                        ANCHOR_A,
                                        &report,
                                        MESH_ROUTE_DEPTH_UNREACHABLE,
                                        0u,
                                        0u,
                                        false);
    assert(mesh_relay_handle_rx(&origin,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                1100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_DEFERRED));
    /* Custody is retained: the frame was heard and refused, not lost. */
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    /* The dead end is parked and a different parent is now selected. */
    assert(origin.pending.next_hop_id == ANCHOR_C);
    assert(origin.pending.retry_after_ms == 1100u);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate =
            &origin.upstream.candidates[i];

        if (candidate->valid && candidate->next_hop_id == ANCHOR_B) {
            assert(candidate->hold_down_valid);
            assert(candidate->hold_down_until_ms ==
                   1100u + MESH_PARENT_DEAD_END_HOLD_MS);
            /* A dead end two hops away is never an RF failure here. */
            assert(candidate->failure_count == failure_count_before);
            assert(candidate->hop_count == MESH_ROUTE_DEPTH_UNREACHABLE);
        }
    }
    printf("ok dead end ack holds candidate and reselects\n");
}

static void test_dead_end_without_alternate_poisons_local_depth(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct proto_packet report = {0};
    struct proto_packet ack = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];
    uint8_t ack_payload[64];
    size_t ack_payload_len;

    (void)arm_pending_report(&origin,
                             &store,
                             ANCHOR_B,
                             1u,
                             &report,
                             payload,
                             sizeof(payload),
                             &sent);
    assert(mesh_relay_local_depth(&origin, 1000u) == 2u);

    ack_payload_len = build_refusal_ack(&ack,
                                        ack_payload,
                                        sizeof(ack_payload),
                                        ANCHOR_B,
                                        ANCHOR_A,
                                        &report,
                                        MESH_ROUTE_DEPTH_UNREACHABLE,
                                        0u,
                                        0u,
                                        false);
    assert(mesh_relay_handle_rx(&origin,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                1100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_DEFERRED));
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(mesh_relay_upstream_poisoned(&origin));
    assert(mesh_relay_local_depth(&origin, 1100u) ==
           MESH_ROUTE_DEPTH_UNREACHABLE);
    assert(mesh_relay_tx_active(&origin));
    printf("ok dead end without alternate poisons local depth\n");
}

static void test_backpressure_ack_reschedules_without_route_penalty(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct proto_packet report = {0};
    struct proto_packet ack = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];
    uint8_t ack_payload[64];
    size_t ack_payload_len;
    uint32_t retry_after_ms;

    (void)arm_pending_report(&origin,
                             &store,
                             ANCHOR_B,
                             1u,
                             &report,
                             payload,
                             sizeof(payload),
                             &sent);

    ack_payload_len = build_refusal_ack(&ack,
                                        ack_payload,
                                        sizeof(ack_payload),
                                        ANCHOR_B,
                                        ANCHOR_A,
                                        &report,
                                        2u,
                                        0u,
                                        400u,
                                        true);
    assert(mesh_relay_handle_rx(&origin,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                1100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_DEFERRED));
    assert(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    /* Same parent, no hold-down, no failure count, no poison. */
    assert(origin.pending.next_hop_id == ANCHOR_B);
    assert(!mesh_relay_upstream_poisoned(&origin));
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate =
            &origin.upstream.candidates[i];

        if (candidate->valid && candidate->next_hop_id == ANCHOR_B) {
            assert(!candidate->hold_down_valid);
            assert(candidate->failure_count == 0u);
            /* The responder's real depth was folded into the candidate. */
            assert(candidate->hop_count == 2u);
            assert(candidate->queue_free_hint == 0u);
        }
    }
    retry_after_ms = origin.pending.retry_after_ms - 1100u;
    assert(retry_after_ms >= 300u && retry_after_ms <= 500u);
    printf("ok backpressure ack reschedules without route penalty\n");
}

static void test_backpressure_retry_after_is_jittered(void)
{
    uint32_t seen_low = 0u;
    uint32_t seen_high = 0u;

    for (uint32_t random_value = 0u; random_value < 512u; random_value++) {
        struct mesh_relay origin;
        struct mesh_anchor_downlink_store store;
        struct mesh_relay_result result;
        struct proto_packet report = {0};
        struct proto_packet ack = {0};
        struct mesh_outbound sent;
        uint8_t payload[160];
        uint8_t ack_payload[64];
        size_t ack_payload_len;
        uint32_t delay_ms;

        (void)arm_pending_report(&origin,
                                 &store,
                                 ANCHOR_B,
                                 1u,
                                 &report,
                                 payload,
                                 sizeof(payload),
                                 &sent);
        ack_payload_len = build_refusal_ack(&ack,
                                            ack_payload,
                                            sizeof(ack_payload),
                                            ANCHOR_B,
                                            ANCHOR_A,
                                            &report,
                                            2u,
                                            0u,
                                            400u,
                                            true);
        assert(mesh_relay_handle_rx_with_random(&origin,
                                                &ack,
                                                ack_payload,
                                                ack_payload_len,
                                                ANCHOR_B,
                                                90u,
                                                1100u,
                                                random_value,
                                                &result) == PROTO_OK);
        delay_ms = origin.pending.retry_after_ms - 1100u;
        assert(delay_ms >= 300u && delay_ms <= 500u);
        if (delay_ms < 400u) {
            seen_low++;
        }
        if (delay_ms > 400u) {
            seen_high++;
        }
    }
    assert(seen_low > 0u && seen_high > 0u);
    printf("ok backpressure retry after is jittered both ways\n");
}

static void test_build_backpressure_ack_refuses_explicitly(void)
{
    struct mesh_relay parent;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct mesh_ack_flow_control flow;
    struct route_candidate route = upstream_route(ANCHOR_C, 3u, 1u, 90u);
    struct proto_packet report = {0};
    uint8_t payload[160];
    size_t payload_len;

    mesh_relay_init(&parent, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&parent, &store) ==
           PROTO_OK);
    assert(route_upsert_candidate(&parent.upstream, &route) == PROTO_OK);
    payload_len = build_click_report(&report,
                                     ANCHOR_A,
                                     42u,
                                     9u,
                                     payload,
                                     sizeof(payload));

    assert(mesh_relay_build_backpressure_ack(&parent,
                                             &report,
                                             payload,
                                             payload_len,
                                             ANCHOR_A,
                                             250u,
                                             1000u,
                                             &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(result.hop_ack.next_hop_id == ANCHOR_A);
    assert(mesh_ack_flow_control_parse(&result.hop_ack.packet,
                                       result.hop_ack.payload,
                                       result.hop_ack.payload_len,
                                       &flow) == PROTO_OK);
    /* Real depth, zero credit, explicit retry-after, nothing accepted. */
    assert(flow.depth_valid && flow.depth == 2u);
    assert(flow.credit_valid && flow.credit == 0u);
    assert(flow.retry_after_valid && flow.retry_after_ms == 250u);
    assert(flow.identity_count == 0u);
    assert(mesh_ack_flow_control_is_backpressure(&flow));
    printf("ok build backpressure ack refuses explicitly\n");
}

static void test_gateway_backpressure_ack_uses_gateway_ack_type(void)
{
    struct mesh_relay gateway;
    struct mesh_gateway_ack_store store;
    struct mesh_relay_result result;
    struct mesh_ack_flow_control flow;
    struct proto_packet report = {0};
    uint8_t payload[160];
    size_t payload_len;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 3u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &store) == PROTO_OK);
    mesh_relay_set_local_free_slots(&gateway, 0u);
    payload_len = build_click_report(&report,
                                     ANCHOR_A,
                                     42u,
                                     9u,
                                     payload,
                                     sizeof(payload));

    assert(mesh_relay_build_backpressure_ack(&gateway,
                                             &report,
                                             payload,
                                             payload_len,
                                             ANCHOR_A,
                                             0u,
                                             1000u,
                                             &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(result.gateway_ack.packet.msg_type == MSG_GATEWAY_ACK);
    assert(mesh_ack_flow_control_parse(&result.gateway_ack.packet,
                                       result.gateway_ack.payload,
                                       result.gateway_ack.payload_len,
                                       &flow) == PROTO_OK);
    assert(flow.depth_valid && flow.depth == 0u);
    assert(flow.credit == 0u);
    assert(flow.retry_after_ms == MESH_RELAY_BACKPRESSURE_RETRY_AFTER_MS);
    assert(flow.identity_count == 0u);
    printf("ok gateway backpressure ack uses gateway ack type\n");
}

static void test_old_firmware_ack_still_releases_custody(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct proto_packet report = {0};
    struct proto_packet ack = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];
    uint8_t ack_payload[96];
    size_t payload_len;
    size_t offset = 0u;

    payload_len = arm_pending_report(&origin,
                                     &store,
                                     ANCHOR_B,
                                     1u,
                                     &report,
                                     payload,
                                     sizeof(payload),
                                     &sent);

    /* Exactly the old wire shape: requested seq plus one semantic identity. */
    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &offset,
                                     report.seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack_payload,
                                             sizeof(ack_payload),
                                             &offset,
                                             &sent.packet,
                                             sent.payload,
                                             sent.payload_len) == PROTO_OK);
    (void)payload_len;
    ack.msg_type = MSG_MESH_HOP_ACK;
    ack.flags = 0u;
    ack.src_id = ANCHOR_B;
    ack.dst_id = ANCHOR_A;
    ack.session_id = report.session_id;
    ack.seq = 5u;
    ack.ttl = MESH_GATEWAY_ACK_TTL;
    ack.payload_len = (uint16_t)offset;

    assert(mesh_relay_handle_rx(&origin,
                                &ack,
                                ack_payload,
                                offset,
                                ANCHOR_B,
                                90u,
                                1100u,
                                &result) == PROTO_OK);
    /* Unknown depth and credit must not change custody handling at all. */
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_HOP_DEFERRED));
    assert(has_action(&result,
                      MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED));
    printf("ok old firmware ack still releases custody\n");
}

/* ---------------------------------------------------------------- item 3 */

static void test_batch_pending_and_remaining_tlv_round_trip(void)
{
    uint8_t payload[32];
    size_t offset = 0u;
    uint8_t pending = 0xAAu;
    uint8_t remaining = 0xAAu;

    /* Zero is the default and costs no wire bytes. */
    assert(mesh_append_batch_pending(payload,
                                     sizeof(payload),
                                     &offset,
                                     0u) == PROTO_OK);
    assert(mesh_append_batch_remaining(payload,
                                       sizeof(payload),
                                       &offset,
                                       0u) == PROTO_OK);
    assert(offset == 0u);
    assert(mesh_batch_pending_from_payload(payload, offset, &pending) ==
           PROTO_OK);
    assert(pending == 0u);

    assert(mesh_append_batch_pending(payload,
                                     sizeof(payload),
                                     &offset,
                                     4u) == PROTO_OK);
    assert(mesh_append_batch_remaining(payload,
                                       sizeof(payload),
                                       &offset,
                                       2u) == PROTO_OK);
    assert(offset == 2u * PROTO_TLV_U8_ENCODED_LEN);
    assert(mesh_batch_pending_from_payload(payload, offset, &pending) ==
           PROTO_OK);
    assert(mesh_batch_remaining_from_payload(payload, offset, &remaining) ==
           PROTO_OK);
    assert(pending == 4u);
    assert(remaining == 2u);
    printf("ok batch pending and remaining tlv round trip\n");
}

static void test_batch_credit_sequences_one_burst_with_one_ack(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct proto_packet report = {0};
    struct proto_packet ack = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];
    uint8_t ack_payload[96];
    uint8_t batch_tlvs[16];
    size_t offset = 0u;
    uint8_t pending_on_wire = 0u;
    uint8_t remaining = 0xFFu;

    (void)arm_pending_report(&origin,
                             &store,
                             ANCHOR_B,
                             1u,
                             &report,
                             payload,
                             sizeof(payload),
                             &sent);

    /* Four frames for this next hop: this one plus three more. */
    assert(mesh_relay_note_batch_pending(&origin, 3u) == PROTO_OK);
    assert(mesh_relay_batch_pending(&origin) == 3u);
    assert(mesh_relay_append_batch_pending(&origin,
                                           batch_tlvs,
                                           sizeof(batch_tlvs),
                                           &offset) == PROTO_OK);
    assert(mesh_batch_pending_from_payload(batch_tlvs,
                                           offset,
                                           &pending_on_wire) == PROTO_OK);
    assert(pending_on_wire == 3u);
    /* Nothing may burst before the peer grants credit. */
    assert(mesh_relay_next_burst_frame(&origin, &remaining) ==
           PROTO_ERR_NOT_FOUND);

    /* Positive ACK granting credit for two more frames. */
    offset = 0u;
    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &offset,
                                     report.seq) == PROTO_OK);
    {
        struct mesh_ack_flow_control flow = {
            .depth = 2u,
            .credit = 2u,
            .depth_valid = true,
            .credit_valid = true,
        };

        assert(mesh_append_ack_flow_control(ack_payload,
                                            sizeof(ack_payload),
                                            &offset,
                                            &flow) == PROTO_OK);
    }
    assert(mesh_append_ack_semantic_identity(ack_payload,
                                             sizeof(ack_payload),
                                             &offset,
                                             &sent.packet,
                                             sent.payload,
                                             sent.payload_len) == PROTO_OK);
    ack.msg_type = MSG_MESH_HOP_ACK;
    ack.flags = 0u;
    ack.src_id = ANCHOR_B;
    ack.dst_id = ANCHOR_A;
    ack.session_id = report.session_id;
    ack.seq = 5u;
    ack.ttl = MESH_GATEWAY_ACK_TTL;
    ack.payload_len = (uint16_t)offset;
    assert(mesh_relay_handle_rx(&origin,
                                &ack,
                                ack_payload,
                                offset,
                                ANCHOR_B,
                                90u,
                                1100u,
                                &result) == PROTO_OK);
    assert(mesh_relay_batch_credit(&origin) == 2u);

    /* Exactly two burst-eligible followers, the last carrying REMAINING 0. */
    assert(mesh_relay_next_burst_frame(&origin, &remaining) == PROTO_OK);
    assert(remaining == 1u);
    assert(mesh_relay_next_burst_frame(&origin, &remaining) == PROTO_OK);
    assert(remaining == 0u);
    assert(mesh_relay_next_burst_frame(&origin, &remaining) ==
           PROTO_ERR_NOT_FOUND);
    /* One frame is still owed and goes in the next burst. */
    assert(mesh_relay_batch_pending(&origin) == 1u);
    printf("ok batch credit sequences one burst with one ack\n");
}

static void test_receiver_extends_listen_until_burst_ends(void)
{
    struct mesh_relay parent;
    struct mesh_anchor_downlink_store store;
    uint8_t payload[16];
    size_t offset = 0u;

    mesh_relay_init(&parent, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&parent, &store) ==
           PROTO_OK);
    assert(!mesh_relay_rx_expects_more(&parent, 1000u));

    /* First frame of a burst announces two followers. */
    assert(mesh_append_batch_pending(payload,
                                     sizeof(payload),
                                     &offset,
                                     2u) == PROTO_OK);
    assert(mesh_relay_note_rx_batch_frame(&parent,
                                          ANCHOR_A,
                                          payload,
                                          offset,
                                          1000u) == PROTO_OK);
    assert(mesh_relay_rx_expects_more(&parent, 1000u));
    assert(mesh_relay_rx_batch_deadline_ms(&parent) ==
           1000u + MESH_BATCH_FOLLOWER_GAP_MS);

    /* Follower one: one more still to come, listen window is extended. */
    offset = 0u;
    assert(mesh_append_batch_remaining(payload,
                                       sizeof(payload),
                                       &offset,
                                       1u) == PROTO_OK);
    assert(mesh_relay_note_rx_batch_frame(&parent,
                                          ANCHOR_A,
                                          payload,
                                          offset,
                                          1004u) == PROTO_OK);
    assert(mesh_relay_rx_expects_more(&parent, 1004u));
    assert(mesh_relay_rx_batch_deadline_ms(&parent) ==
           1004u + MESH_BATCH_FOLLOWER_GAP_MS);

    /* Last frame: REMAINING absent means the burst is over, ACK now. */
    assert(mesh_relay_note_rx_batch_frame(&parent,
                                          ANCHOR_A,
                                          NULL,
                                          0u,
                                          1008u) == PROTO_OK);
    assert(!mesh_relay_rx_expects_more(&parent, 1008u));
    printf("ok receiver extends listen until burst ends\n");
}

static void test_receiver_gives_up_after_follower_gap(void)
{
    struct mesh_relay parent;
    struct mesh_anchor_downlink_store store;
    uint8_t payload[16];
    size_t offset = 0u;

    mesh_relay_init(&parent, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&parent, &store) ==
           PROTO_OK);
    assert(mesh_append_batch_pending(payload,
                                     sizeof(payload),
                                     &offset,
                                     3u) == PROTO_OK);
    assert(mesh_relay_note_rx_batch_frame(&parent,
                                          ANCHOR_A,
                                          payload,
                                          offset,
                                          1000u) == PROTO_OK);
    assert(mesh_relay_rx_expects_more(&parent, 1000u));
    /* The promised follower never arrives: the single ACK is owed anyway. */
    assert(!mesh_relay_rx_expects_more(&parent,
                                       1000u + MESH_BATCH_FOLLOWER_GAP_MS));
    printf("ok receiver gives up after follower gap\n");
}

/* ---------------------------------------------------------------- item 4 */

static void test_local_depth_tracks_selected_parent(void)
{
    struct mesh_relay anchor;
    struct mesh_anchor_downlink_store store;
    struct route_candidate route = upstream_route(ANCHOR_B, 3u, 2u, 90u);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&anchor, &store) ==
           PROTO_OK);
    assert(mesh_relay_local_depth(&anchor, 1000u) ==
           MESH_ROUTE_DEPTH_UNREACHABLE);
    assert(route_upsert_candidate(&anchor.upstream, &route) == PROTO_OK);
    assert(mesh_relay_local_depth(&anchor, 1000u) == 3u);

    mesh_relay_poison_upstream(&anchor);
    assert(mesh_relay_local_depth(&anchor, 1000u) ==
           MESH_ROUTE_DEPTH_UNREACHABLE);
    printf("ok local depth tracks selected parent\n");
}

static void test_loop_freedom_rejects_equal_or_deeper_candidates(void)
{
    struct mesh_relay anchor;
    struct mesh_anchor_downlink_store store;
    struct route_candidate route = upstream_route(ANCHOR_B, 3u, 2u, 90u);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&anchor, &store) ==
           PROTO_OK);
    assert(route_upsert_candidate(&anchor.upstream, &route) == PROTO_OK);
    assert(mesh_relay_local_depth(&anchor, 1000u) == 3u);

    assert(mesh_relay_depth_is_forwardable(&anchor, 0u, 1000u));
    assert(mesh_relay_depth_is_forwardable(&anchor, 2u, 1000u));
    /* A former child advertising our own depth or deeper would loop. */
    assert(!mesh_relay_depth_is_forwardable(&anchor, 3u, 1000u));
    assert(!mesh_relay_depth_is_forwardable(&anchor, 4u, 1000u));
    assert(!mesh_relay_depth_is_forwardable(&anchor,
                                            MESH_ROUTE_DEPTH_UNREACHABLE,
                                            1000u));

    /* Once poisoned, any finite depth is usable, including a former child. */
    mesh_relay_poison_upstream(&anchor);
    assert(mesh_relay_depth_is_forwardable(&anchor, 4u, 1000u));
    assert(!mesh_relay_depth_is_forwardable(&anchor,
                                            MESH_ROUTE_DEPTH_UNREACHABLE,
                                            1000u));
    printf("ok loop freedom rejects equal or deeper candidates\n");
}

static void test_next_hop_skips_poisoned_candidate(void)
{
    struct mesh_relay anchor;
    struct mesh_anchor_downlink_store store;
    struct route_candidate near = upstream_route(ANCHOR_B, 3u, 0u, 95u);
    struct route_candidate far = upstream_route(ANCHOR_C, 3u, 1u, 90u);
    struct proto_packet report = {0};
    uint8_t payload[160];
    size_t payload_len;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&anchor, &store) ==
           PROTO_OK);
    assert(route_upsert_candidate(&anchor.upstream, &near) == PROTO_OK);
    assert(route_upsert_candidate(&anchor.upstream, &far) == PROTO_OK);
    payload_len = build_click_report(&report,
                                     ANCHOR_A,
                                     42u,
                                     9u,
                                     payload,
                                     sizeof(payload));
    assert(mesh_relay_select_next_hop_for_packet(&anchor,
                                                 &report,
                                                 payload,
                                                 payload_len,
                                                 1000u,
                                                 &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    /* The best parent answers UNREACHABLE: its depth is poisoned locally. */
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &anchor.upstream.candidates[i];

        if (candidate->valid && candidate->next_hop_id == ANCHOR_B) {
            candidate->hop_count = MESH_ROUTE_DEPTH_UNREACHABLE;
            candidate->hold_down_until_ms = 1000u +
                                            MESH_PARENT_DEAD_END_HOLD_MS;
            candidate->hold_down_valid = true;
        }
    }
    assert(mesh_relay_select_next_hop_for_packet(&anchor,
                                                 &report,
                                                 payload,
                                                 payload_len,
                                                 1000u,
                                                 &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_C);
    printf("ok next hop skips poisoned candidate\n");
}

static void test_gateway_answers_solicit_with_depth_zero(void)
{
    struct mesh_relay gateway;
    struct mesh_gateway_ack_store store;
    struct mesh_outbound reply;
    struct proto_packet solicit = {0};
    uint8_t depth = 0xAAu;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 3u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &store) == PROTO_OK);

    solicit.msg_type = MSG_ROUTE_SOLICIT;
    solicit.src_id = ANCHOR_A;
    solicit.dst_id = MESH_BROADCAST_ID;
    solicit.session_id = 0x7788u;
    solicit.seq = 4u;
    solicit.ttl = 1u;
    solicit.payload_len = 0u;

    assert(mesh_relay_build_solicit_reply(&gateway,
                                          &solicit,
                                          ANCHOR_A,
                                          1000u,
                                          &reply) == PROTO_OK);
    assert(reply.packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    assert(reply.packet.src_id == GATEWAY);
    /* Unicast at the radio, ordinary advert on the wire. */
    assert(reply.next_hop_id == ANCHOR_A);
    assert(reply.packet.dst_id == MESH_BROADCAST_ID);
    assert(tlv_find_unique(reply.payload,
                           reply.payload_len,
                           TLV_HOP_COUNT,
                           &(const uint8_t *){NULL},
                           &(uint8_t){0u}) == PROTO_OK);
    {
        const uint8_t *value = NULL;
        uint8_t value_len = 0u;

        assert(tlv_find_unique(reply.payload,
                               reply.payload_len,
                               TLV_HOP_COUNT,
                               &value,
                               &value_len) == PROTO_OK);
        assert(value_len == sizeof(uint8_t));
        depth = value[0];
    }
    assert(depth == 0u);
    assert(MESH_SOLICIT_REPLY_JITTER_MS == 40u);
    printf("ok gateway answers solicit with depth zero\n");
}

static void test_unreachable_node_never_answers_solicit(void)
{
    struct mesh_relay anchor;
    struct mesh_anchor_downlink_store store;
    struct mesh_outbound reply;
    struct proto_packet solicit = {0};

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&anchor, &store) ==
           PROTO_OK);

    solicit.msg_type = MSG_ROUTE_SOLICIT;
    solicit.src_id = ANCHOR_A;
    solicit.dst_id = MESH_BROADCAST_ID;
    solicit.session_id = 0x7788u;
    solicit.seq = 4u;
    solicit.ttl = 1u;

    assert(mesh_relay_local_depth(&anchor, 1000u) ==
           MESH_ROUTE_DEPTH_UNREACHABLE);
    assert(mesh_relay_build_solicit_reply(&anchor,
                                          &solicit,
                                          ANCHOR_A,
                                          1000u,
                                          &reply) == PROTO_ERR_NOT_FOUND);
    printf("ok unreachable node never answers solicit\n");
}

static void test_solicit_is_never_forwarded(void)
{
    struct mesh_relay anchor;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct route_candidate route = upstream_route(ANCHOR_C, 3u, 0u, 90u);
    struct proto_packet solicit = {0};

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_attach_anchor_downlink_store(&anchor, &store) ==
           PROTO_OK);
    assert(route_upsert_candidate(&anchor.upstream, &route) == PROTO_OK);

    solicit.msg_type = MSG_ROUTE_SOLICIT;
    solicit.src_id = ANCHOR_A;
    solicit.dst_id = MESH_BROADCAST_ID;
    solicit.session_id = 0x7788u;
    solicit.seq = 4u;
    solicit.ttl = 1u;
    solicit.payload_len = 0u;

    assert(mesh_relay_handle_rx(&anchor,
                                &solicit,
                                NULL,
                                0u,
                                ANCHOR_A,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    printf("ok solicit is never forwarded\n");
}

static void test_returned_own_packet_is_readopted(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct route_candidate alternate = upstream_route(ANCHOR_C, 3u, 0u, 80u);
    struct proto_packet report = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];
    size_t payload_len;

    payload_len = arm_pending_report(&origin,
                                     &store,
                                     ANCHOR_B,
                                     1u,
                                     &report,
                                     payload,
                                     sizeof(payload),
                                     &sent);
    /* The parent takes custody of exactly these bytes. */
    mesh_relay_note_parent_handoff(&origin, &sent.packet, ANCHOR_B);
    mesh_relay_cancel_tx(&origin);
    assert(!mesh_relay_tx_active(&origin));
    assert(route_upsert_candidate(&origin.upstream, &alternate) == PROTO_OK);

    /* An unrelated packet coming back from a node we never used is not ours. */
    assert(!mesh_relay_should_readopt_returned_packet(&origin,
                                                      &sent.packet,
                                                      ANCHOR_C));
    assert(mesh_relay_should_readopt_returned_packet(&origin,
                                                     &sent.packet,
                                                     ANCHOR_B));

    assert(mesh_relay_handle_rx(&origin,
                                &sent.packet,
                                sent.payload,
                                sent.payload_len,
                                ANCHOR_B,
                                90u,
                                2000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    /* The former parent gets an ACK so it can drop its copy. */
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(result.hop_ack.next_hop_id == ANCHOR_B);
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.packet.session_id == report.session_id);
    assert(origin.pending.packet.seq == report.seq);
    assert(origin.pending.payload_len == payload_len);
    printf("ok returned own packet is readopted\n");
}

static void test_gateway_acked_packet_is_not_readopted(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct proto_packet report = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];

    (void)arm_pending_report(&origin,
                             &store,
                             ANCHOR_B,
                             1u,
                             &report,
                             payload,
                             sizeof(payload),
                             &sent);
    mesh_relay_note_parent_handoff(&origin, &sent.packet, ANCHOR_B);
    mesh_relay_note_handoff_gateway_acked(&origin,
                                          report.session_id,
                                          report.seq);
    mesh_relay_cancel_tx(&origin);

    assert(!mesh_relay_should_readopt_returned_packet(&origin,
                                                      &sent.packet,
                                                      ANCHOR_B));
    /* A delivered packet coming back is our own frame, not new custody. */
    assert(mesh_relay_handle_rx(&origin,
                                &sent.packet,
                                sent.payload,
                                sent.payload_len,
                                ANCHOR_B,
                                90u,
                                2000u,
                                &result) == PROTO_ERR_ARG);
    printf("ok gateway acked packet is not readopted\n");
}

static void test_readoption_refuses_explicitly_when_busy(void)
{
    struct mesh_relay origin;
    struct mesh_anchor_downlink_store store;
    struct mesh_relay_result result;
    struct mesh_ack_flow_control flow;
    struct proto_packet report = {0};
    struct mesh_outbound sent;
    uint8_t payload[160];

    (void)arm_pending_report(&origin,
                             &store,
                             ANCHOR_B,
                             1u,
                             &report,
                             payload,
                             sizeof(payload),
                             &sent);
    mesh_relay_note_parent_handoff(&origin, &sent.packet, ANCHOR_B);
    /* Custody slot is still busy with the pending copy. */
    assert(mesh_relay_tx_active(&origin));

    assert(mesh_relay_handle_rx(&origin,
                                &sent.packet,
                                sent.payload,
                                sent.payload_len,
                                ANCHOR_B,
                                90u,
                                2000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(mesh_ack_flow_control_parse(&result.hop_ack.packet,
                                       result.hop_ack.payload,
                                       result.hop_ack.payload_len,
                                       &flow) == PROTO_OK);
    assert(flow.identity_count == 0u);
    assert(flow.credit_valid && flow.credit == 0u);
    assert(flow.retry_after_valid);
    printf("ok readoption refuses explicitly when busy\n");
}

int main(void)
{
    test_ack_flow_control_tlv_round_trip();
    test_ack_flow_control_missing_tlvs_are_unknown();
    test_credit_reserves_one_slot_for_own_reports();
    test_gateway_ack_carries_depth_zero_and_stream_credit();
    test_hop_ack_carries_parent_depth();
    test_dead_end_ack_holds_candidate_and_reselects();
    test_dead_end_without_alternate_poisons_local_depth();
    test_backpressure_ack_reschedules_without_route_penalty();
    test_backpressure_retry_after_is_jittered();
    test_build_backpressure_ack_refuses_explicitly();
    test_gateway_backpressure_ack_uses_gateway_ack_type();
    test_old_firmware_ack_still_releases_custody();
    test_batch_pending_and_remaining_tlv_round_trip();
    test_batch_credit_sequences_one_burst_with_one_ack();
    test_receiver_extends_listen_until_burst_ends();
    test_receiver_gives_up_after_follower_gap();
    test_local_depth_tracks_selected_parent();
    test_loop_freedom_rejects_equal_or_deeper_candidates();
    test_next_hop_skips_poisoned_candidate();
    test_gateway_answers_solicit_with_depth_zero();
    test_unreachable_node_never_answers_solicit();
    test_solicit_is_never_forwarded();
    test_returned_own_packet_is_readopted();
    test_gateway_acked_packet_is_not_readopted();
    test_readoption_refuses_explicitly_when_busy();
    printf("all channel 5 delivery tests passed\n");
    return 0;
}
