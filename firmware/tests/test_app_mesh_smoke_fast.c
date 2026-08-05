#include "app_mesh_smoke_fast.h"

#include <assert.h>
#include <string.h>

static struct mesh_smoke_fast_payload_input input(uint32_t packet_id)
{
    return (struct mesh_smoke_fast_payload_input) {
        .packet_id = packet_id,
        .build_uptime_ms = 1000u + packet_id,
        .packet_age_ms = 10u,
        .drop_count = 2u,
        .origin_id = 0x3333333333333301ull,
        .target_id = 0x1111222233334444ull,
        .selected_parent_id = 0x1111222233334444ull,
        .attempt = 1u,
        .device_role = ROLE_ANCHOR,
        .mesh_channel = 9u,
        .ch9_timing_state = MESH_SMOKE_FAST_CH9_TIMING_FRESH,
        .flags = 0x7u,
    };
}

static size_t append_payload(uint8_t *payload,
                             size_t payload_cap,
                             uint32_t packet_id)
{
    struct mesh_smoke_fast_payload_input in = input(packet_id);
    size_t payload_len = 0u;

    assert(mesh_smoke_fast_payload_append(payload,
                                          payload_cap,
                                          &payload_len,
                                          &in,
                                          128u) == PROTO_OK);
    return payload_len;
}

static void test_payload_round_trip_and_crc(void)
{
    uint8_t payload[192];
    struct mesh_smoke_fast_payload decoded;
    size_t payload_len = append_payload(payload, sizeof(payload), 42u);
    size_t extended_len;

    assert(mesh_smoke_fast_payload_decode(payload,
                                          payload_len,
                                          &decoded) == PROTO_OK);
    assert(decoded.packet_id == 42u);
    assert(decoded.build_uptime_ms == 1042u);
    assert(decoded.packet_age_ms == 10u);
    assert(decoded.drop_count == 2u);
    assert(decoded.origin_id == 0x3333333333333301ull);
    assert(decoded.target_id == 0x1111222233334444ull);
    assert(decoded.selected_parent_id == 0x1111222233334444ull);
    assert(decoded.attempt == 1u);
    assert(decoded.retry_count == 1u);
    assert(decoded.device_role == ROLE_ANCHOR);
    assert(decoded.mesh_channel == 9u);
    assert(decoded.ch9_timing_state == MESH_SMOKE_FAST_CH9_TIMING_FRESH);
    assert(decoded.flags == 0x7u);

    payload[4] ^= 0x01u;
    assert(mesh_smoke_fast_payload_decode(payload,
                                          payload_len,
                                          &decoded) == PROTO_ERR_BAD_CRC);

    payload_len = append_payload(payload, sizeof(payload), 42u);
    extended_len = payload_len;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &extended_len,
                          TLV_MESH_TEST_PACKET_ID,
                          43u) == PROTO_OK);
    assert(mesh_smoke_fast_payload_decode(payload,
                                          extended_len,
                                          &decoded) == PROTO_ERR_MALFORMED);
}

static void test_tx_decision_fast_and_busy(void)
{
    struct mesh_smoke_fast_tx_gate gate = {
        .queue_used = 1u,
        .queue_depth = 8u,
        .configured_interval_ms = 1000u,
        .fast_mode = true,
    };
    struct mesh_smoke_fast_tx_decision decision;

    mesh_smoke_fast_tx_decide(&gate, &decision);
    assert(decision.can_queue);
    assert(decision.queue_headroom == 7u);
    assert(decision.reason == MESH_SMOKE_FAST_DEFER_NONE);
    assert(decision.delay_ms == 0u);

    gate.relay_tx_active = true;
    mesh_smoke_fast_tx_decide(&gate, &decision);
    assert(!decision.can_queue);
    assert(decision.reason == MESH_SMOKE_FAST_DEFER_RELAY_TX);
    assert(decision.delay_ms == 100u);

    gate.relay_tx_active = false;
    gate.queue_used = 8u;
    mesh_smoke_fast_tx_decide(&gate, &decision);
    assert(!decision.can_queue);
    assert(decision.reason == MESH_SMOKE_FAST_DEFER_QUEUE_FULL);
}

static void test_gateway_verifier_counts_gap_duplicate_and_late(void)
{
    struct mesh_smoke_fast_state state;
    struct mesh_smoke_fast_summary summary;
    uint8_t payload[192];
    size_t len;

    mesh_smoke_fast_init(&state);
    len = append_payload(payload, sizeof(payload), 1u);
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2000u, 8u, 1u) ==
           PROTO_OK);
    len = append_payload(payload, sizeof(payload), 3u);
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2010u, 18u, 3u) ==
           PROTO_OK);
    len = append_payload(payload, sizeof(payload), 3u);
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2020u, 30u, 2u) ==
           PROTO_OK);
    len = append_payload(payload, sizeof(payload), 2u);
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2030u, 12u, 4u) ==
           PROTO_OK);
    mesh_smoke_fast_get_summary(&state, &summary);
    assert(summary.delivered_count == 4u);
    assert(summary.gap_count == 1u);
    assert(summary.missing_count == 1u);
    assert(summary.duplicate_count == 1u);
    assert(summary.later_delivered_missing_count == 1u);
    assert(summary.queue_depth_max == 4u);
    assert(summary.gateway_ack_latency_max_ms == 30u);
    assert(summary.gateway_ack_latency_p50_ms == 18u);
    assert(summary.gateway_ack_latency_p95_ms == 30u);
}

static void test_missing_reason_and_crc_counter(void)
{
    struct mesh_smoke_fast_state state;
    struct mesh_smoke_fast_summary summary;
    uint8_t payload[192];
    size_t len;

    mesh_smoke_fast_init(&state);
    len = append_payload(payload, sizeof(payload), 10u);
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2000u, 4u, 1u) ==
           PROTO_OK);
    len = append_payload(payload, sizeof(payload), 12u);
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2010u, 4u, 1u) ==
           PROTO_OK);
    mesh_smoke_fast_note_missing_reason(&state, 11u, MESH_SMOKE_FAST_DEFER_ACK_WAIT);

    len = append_payload(payload, sizeof(payload), 13u);
    payload[3] ^= 0x40u;
    assert(mesh_smoke_fast_note_delivery(&state, payload, len, 2020u, 4u, 1u) ==
           PROTO_ERR_BAD_CRC);
    mesh_smoke_fast_note_ch9_missed(&state);
    mesh_smoke_fast_note_c5_refresh(&state);

    mesh_smoke_fast_get_summary(&state, &summary);
    assert(summary.attributed_missing_count == 1u);
    assert(summary.last_drop_or_defer_reason == MESH_SMOKE_FAST_DEFER_ACK_WAIT);
    assert(summary.crc_fail_count == 1u);
    assert(summary.missed_ch9_events == 1u);
    assert(summary.c5_refreshes == 1u);
}

int main(void)
{
    test_payload_round_trip_and_crc();
    test_tx_decision_fast_and_busy();
    test_gateway_verifier_counts_gap_duplicate_and_late();
    test_missing_reason_and_crc_counter();
    return 0;
}
