#include "app_mesh_ch9_ack.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#define RELAY_ID UINT64_C(0x1111222233334444)
#define TRANSMITTER_ID UINT64_C(0x2222333344445555)
#define GATEWAY_ID_TEST UINT64_C(0xAAAABBBBCCCCDDDD)
#define SESSION_ID_TEST UINT32_C(0x12345678)
#define SENT_SEQ_TEST UINT16_C(0x2345)

static struct mesh_outbound gateway_bound_outbound(uint64_t src_id)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = src_id,
            .dst_id = GATEWAY_ID_TEST,
            .session_id = SESSION_ID_TEST,
            .seq = SENT_SEQ_TEST,
            .ttl = 4u,
            .payload_len = 0u,
        },
        .payload_len = 0u,
        .next_hop_id = GATEWAY_ID_TEST,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
    };
}

static struct proto_packet gateway_ack(uint64_t dst_id, uint16_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = MSG_GATEWAY_ACK,
        .flags = FLAG_GATEWAY_ACK,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = dst_id,
        .session_id = SESSION_ID_TEST,
        .seq = UINT16_C(0x9001),
        .ttl = 1u,
        .payload_len = payload_len,
    };
}

struct retry_queue_fixture {
    struct mesh_outbound item;
    uint8_t used;
};

static int retry_queue_put(const struct mesh_outbound *outbound, void *ctx)
{
    struct retry_queue_fixture *queue = ctx;

    if (queue->used != 0u) {
        return -ENOSPC;
    }
    queue->item = *outbound;
    queue->used = 1u;
    return 0;
}

static int retry_queue_get(struct mesh_outbound *outbound, void *ctx)
{
    struct retry_queue_fixture *queue = ctx;

    if (queue->used == 0u) {
        return -ENOENT;
    }
    *outbound = queue->item;
    queue->used = 0u;
    return 0;
}

static uint8_t retry_queue_used(void *ctx)
{
    return ((struct retry_queue_fixture *)ctx)->used;
}

static void test_unacked_retry_retains_ownership_until_queue_admits(void)
{
    struct retry_queue_fixture queue = {.used = 1u};
    struct app_mesh_ch9_tx_retry_entry entries[2] = {
        {.outbound = gateway_bound_outbound(TRANSMITTER_ID)},
        {.outbound = gateway_bound_outbound(TRANSMITTER_ID)},
    };
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = retry_queue_put,
        .get = retry_queue_get,
        .queue_used = retry_queue_used,
        .ctx = &queue,
    };
    struct app_mesh_ch9_tx_retry_result result;

    entries[1].outbound.packet.seq++;
    assert(app_mesh_ch9_tx_requeue_unacked(entries,
                                           2u,
                                           100u,
                                           &ops,
                                           &result) == PROTO_OK);
    assert(result.requeued == 0u);
    assert(result.retained == 2u);
    assert(result.dropped == 0u);
    assert(!entries[0].acked && !entries[1].acked);

    queue.used = 0u;
    assert(app_mesh_ch9_tx_requeue_unacked(entries,
                                           2u,
                                           200u,
                                           &ops,
                                           &result) == PROTO_OK);
    assert(result.requeued == 1u);
    assert(result.retained == 1u);
    assert(entries[0].acked && !entries[1].acked);
    assert(queue.item.packet.seq == SENT_SEQ_TEST);

    queue.used = 0u;
    memset(&result, 0, sizeof(result));
    assert(app_mesh_ch9_tx_requeue_unacked(entries,
                                           2u,
                                           300u,
                                           &ops,
                                           &result) == PROTO_OK);
    assert(result.requeued == 1u);
    assert(result.retained == 0u);
    assert(entries[0].acked && entries[1].acked);
    assert(queue.item.packet.seq == SENT_SEQ_TEST + 1u);
}

static size_t requested_seq_payload(uint8_t *payload, size_t payload_cap)
{
    size_t payload_len = 0u;

    assert(mesh_append_requested_seq(payload,
                                     payload_cap,
                                     &payload_len,
                                     SENT_SEQ_TEST) == PROTO_OK);
    return payload_len;
}

static size_t batched_ack_payload_with_matching_second(uint8_t *payload,
                                                       size_t payload_cap)
{
    uint8_t seq_list[2u * sizeof(uint16_t)];
    uint8_t session_list[2u * sizeof(uint32_t)];
    size_t payload_len = 0u;

    proto_put_u16_le(&seq_list[0], (uint16_t)(SENT_SEQ_TEST + 1u));
    proto_put_u16_le(&seq_list[sizeof(uint16_t)], SENT_SEQ_TEST);
    proto_put_u32_le(&session_list[0], SESSION_ID_TEST + 1u);
    proto_put_u32_le(&session_list[sizeof(uint32_t)], SESSION_ID_TEST);

    assert(tlv_append_bytes(payload,
                            payload_cap,
                            &payload_len,
                            TLV_MESH_ACK_SESSION_LIST,
                            session_list,
                            sizeof(session_list)) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            payload_cap,
                            &payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seq_list,
                            sizeof(seq_list)) == PROTO_OK);
    return payload_len;
}

static void test_ack_complete_keeps_idle_route_test_timing_open(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_does_not_close_when_work_remains(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .report_tx_queue_used = 1u,
        .route_waiting_tx_valid = true,
        .ack_batch_valid = true,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_policy_is_disabled_outside_route_test(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = false,
        .transmitter_role = false,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
    assert(!app_mesh_ch9_ack_complete_should_close_timing(NULL));
}

static void test_direct_gateway_ack_matches_transit_original_source(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const size_t payload_len = requested_seq_payload(payload, sizeof(payload));
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    const struct proto_packet ack = gateway_ack(TRANSMITTER_ID,
                                                (uint16_t)payload_len);

    assert(app_mesh_direct_gateway_ack_matches(&sent,
                                               &ack,
                                               payload,
                                               payload_len,
                                               GATEWAY_ID_TEST,
                                               GATEWAY_ID_TEST));
}

static void test_direct_gateway_ack_rejects_relay_address_for_transit(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const size_t payload_len = requested_seq_payload(payload, sizeof(payload));
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    const struct proto_packet ack = gateway_ack(RELAY_ID,
                                                (uint16_t)payload_len);

    assert(!app_mesh_direct_gateway_ack_matches(&sent,
                                                &ack,
                                                payload,
                                                payload_len,
                                                GATEWAY_ID_TEST,
                                                GATEWAY_ID_TEST));
}

static void test_direct_gateway_ack_matches_local_source(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const size_t payload_len = requested_seq_payload(payload, sizeof(payload));
    const struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);
    const struct proto_packet ack = gateway_ack(RELAY_ID,
                                                (uint16_t)payload_len);

    assert(app_mesh_direct_gateway_ack_matches(&sent,
                                               &ack,
                                               payload,
                                               payload_len,
                                               GATEWAY_ID_TEST,
                                               GATEWAY_ID_TEST));
}

static void test_direct_gateway_ack_matches_batched_session_list(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const size_t payload_len =
        batched_ack_payload_with_matching_second(payload, sizeof(payload));
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    struct proto_packet ack = gateway_ack(TRANSMITTER_ID,
                                          (uint16_t)payload_len);

    ack.session_id = SESSION_ID_TEST + 1u;

    assert(app_mesh_direct_gateway_ack_matches(&sent,
                                               &ack,
                                               payload,
                                               payload_len,
                                               GATEWAY_ID_TEST,
                                               GATEWAY_ID_TEST));
}

static void test_direct_gateway_legacy_ack_rejects_wrong_session(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const size_t payload_len = requested_seq_payload(payload, sizeof(payload));
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    struct proto_packet ack = gateway_ack(TRANSMITTER_ID,
                                          (uint16_t)payload_len);

    ack.session_id = SESSION_ID_TEST + 1u;

    assert(!app_mesh_direct_gateway_ack_matches(&sent,
                                                &ack,
                                                payload,
                                                payload_len,
                                                GATEWAY_ID_TEST,
                                                GATEWAY_ID_TEST));
}

static void test_gateway_ack_relay_path_keeps_configured_in_flight_limit(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet, RELAY_ID, 8u) == 8u);
}

static void test_anchor_tracks_transit_direct_gateway_send(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
}

static void test_downstream_pressure_drops_transit_timeout(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_DROP_TRANSIT);
}

static void test_downstream_pressure_defers_nonpriority_local_timeout(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);

    sent.packet.msg_type = MSG_ANCHOR_HEARTBEAT;
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_DEFER_LOCAL);
}

static void test_downstream_pressure_allows_local_click_preemption(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);

    sent.packet.msg_type = MSG_CLICK_REPORT;
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL);

    sent.packet.msg_type = MSG_COMMAND_RESULT;
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL);
}

static void test_timeout_pressure_is_inactive_without_downstream(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, false, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_RETRY);
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, false, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_RETRY);
}

static void test_local_direct_gateway_send_tracks_ack(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);

    assert(app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
}

static void test_local_destination_does_not_track_ack(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.packet.dst_id = RELAY_ID;
    assert(!app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
}

static void test_non_ch9_send_does_not_track_ack(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    assert(!app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
}

static void test_direct_next_hop_keeps_configured_in_flight_limit(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet, GATEWAY_ID_TEST, 8u) == 8u);
}

static void test_non_ack_payload_keeps_configured_in_flight_limit(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet, RELAY_ID, 8u) == 8u);
}

static void test_direct_gateway_timeout_counts_gateway_failure(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_tx_timeout_counts_gateway_failure(&sent,
                                                          GATEWAY_ID_TEST,
                                                          GATEWAY_ID_TEST));
}

static void test_relay_hop_timeout_does_not_count_gateway_failure(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.next_hop_id = RELAY_ID;

    assert(!app_mesh_ch9_tx_timeout_counts_gateway_failure(&sent,
                                                           RELAY_ID,
                                                           GATEWAY_ID_TEST));
}

static void test_non_gateway_ack_timeout_does_not_count_gateway_failure(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.packet.flags = 0u;
    assert(!app_mesh_ch9_tx_timeout_counts_gateway_failure(&sent,
                                                           GATEWAY_ID_TEST,
                                                           GATEWAY_ID_TEST));

    sent = gateway_bound_outbound(TRANSMITTER_ID);
    sent.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    assert(!app_mesh_ch9_tx_timeout_counts_gateway_failure(&sent,
                                                           GATEWAY_ID_TEST,
                                                           GATEWAY_ID_TEST));
}

int main(void)
{
    test_ack_complete_keeps_idle_route_test_timing_open();
    test_ack_complete_does_not_close_when_work_remains();
    test_ack_complete_policy_is_disabled_outside_route_test();
    test_direct_gateway_ack_matches_transit_original_source();
    test_direct_gateway_ack_rejects_relay_address_for_transit();
    test_direct_gateway_ack_matches_local_source();
    test_direct_gateway_ack_matches_batched_session_list();
    test_direct_gateway_legacy_ack_rejects_wrong_session();
    test_gateway_ack_relay_path_keeps_configured_in_flight_limit();
    test_anchor_tracks_transit_direct_gateway_send();
    test_downstream_pressure_drops_transit_timeout();
    test_downstream_pressure_defers_nonpriority_local_timeout();
    test_downstream_pressure_allows_local_click_preemption();
    test_timeout_pressure_is_inactive_without_downstream();
    test_local_direct_gateway_send_tracks_ack();
    test_local_destination_does_not_track_ack();
    test_non_ch9_send_does_not_track_ack();
    test_direct_next_hop_keeps_configured_in_flight_limit();
    test_non_ack_payload_keeps_configured_in_flight_limit();
    test_direct_gateway_timeout_counts_gateway_failure();
    test_relay_hop_timeout_does_not_count_gateway_failure();
    test_non_gateway_ack_timeout_does_not_count_gateway_failure();
    test_unacked_retry_retains_ownership_until_queue_admits();
    return 0;
}
