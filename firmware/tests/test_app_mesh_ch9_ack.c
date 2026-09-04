#include "app_mesh_ch9_ack.h"
#include "firmware_state_machines.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#define RELAY_ID UINT64_C(0x1111222233334444)
#define TRANSMITTER_ID UINT64_C(0x2222333344445555)
#define SECOND_RELAY_ID UINT64_C(0x3333444455556666)
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

static struct mesh_outbound ack_outbound(uint64_t peer_id, uint8_t msg_type)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = msg_type,
            .flags = msg_type == MSG_GATEWAY_ACK ? FLAG_GATEWAY_ACK : 0u,
            .src_id = GATEWAY_ID_TEST,
            .dst_id = peer_id,
            .session_id = SESSION_ID_TEST,
            .seq = UINT16_C(0x9001),
            .ttl = 1u,
        },
        .next_hop_id = peer_id,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
    };
}

static struct app_mesh_ch9_ack_batch_entry ack_batch_entry(uint32_t session_id,
                                                            uint16_t seq,
                                                            uint32_t packet_id)
{
    return (struct app_mesh_ch9_ack_batch_entry) {
        .session_id = session_id,
        .packet_id = packet_id,
        .seq = seq,
        .has_packet_id = true,
    };
}

static void bind_ack_to_batch_entry(
    struct mesh_outbound *ack,
    const struct app_mesh_ch9_ack_batch_entry *entry)
{
    const struct proto_packet acknowledged = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ack->packet.dst_id,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = entry->session_id,
        .seq = entry->seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
    size_t payload_len = 0u;

    ack->packet.session_id = entry->session_id;
    assert(mesh_append_requested_seq(ack->payload,
                                     sizeof(ack->payload),
                                     &payload_len,
                                     entry->seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack->payload,
                                             sizeof(ack->payload),
                                             &payload_len,
                                             &acknowledged,
                                             NULL,
                                             0u) == PROTO_OK);
    ack->payload_len = (uint16_t)payload_len;
    ack->packet.payload_len = (uint16_t)payload_len;
}

static int queue_ack_with_semantic_identity(
    struct app_mesh_ch9_ack_table *table,
    struct mesh_outbound *ack,
    const struct app_mesh_ch9_ack_batch_entry *entry,
    enum app_mesh_ch9_ack_queue_result *result)
{
    bind_ack_to_batch_entry(ack, entry);
    return app_mesh_ch9_ack_table_queue(table, ack, entry, result);
}

static int queue_raw_ack(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    const struct app_mesh_ch9_ack_batch_entry *entry,
    enum app_mesh_ch9_ack_queue_result *result)
{
    return app_mesh_ch9_ack_table_queue(table, ack, entry, result);
}

/* Every generated test ACK must exercise the production semantic binding. */
#define app_mesh_ch9_ack_table_queue(table, ack, entry, result) \
    queue_ack_with_semantic_identity((table), (ack), (entry), (result))

static void assert_built_ack_entries(const struct mesh_outbound *outbound,
                                     const uint32_t *session_ids,
                                     const uint16_t *seqs,
                                     const uint32_t *packet_ids,
                                     uint8_t count)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(outbound != NULL);
    assert(tlv_find(outbound->payload,
                    outbound->payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    assert(proto_get_u16_le(value) == seqs[0]);

    assert(tlv_find(outbound->payload,
                    outbound->payload_len,
                    TLV_MESH_ACK_SESSION_LIST,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == count * sizeof(uint32_t));
    for (uint8_t i = 0u; i < count; i++) {
        assert(proto_get_u32_le(&value[i * sizeof(uint32_t)]) ==
               session_ids[i]);
    }

    assert(tlv_find(outbound->payload,
                    outbound->payload_len,
                    TLV_MESH_ACK_SEQ_LIST,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == count * sizeof(uint16_t));
    for (uint8_t i = 0u; i < count; i++) {
        assert(proto_get_u16_le(&value[i * sizeof(uint16_t)]) == seqs[i]);
    }

    assert(tlv_find(outbound->payload,
                    outbound->payload_len,
                    TLV_MESH_ACK_PACKET_ID_LIST,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == count * sizeof(uint32_t));
    for (uint8_t i = 0u; i < count; i++) {
        struct mesh_ack_semantic_identity identity;

        assert(proto_get_u32_le(&value[i * sizeof(uint32_t)]) ==
               packet_ids[i]);
        assert(mesh_ack_semantic_identity_at(outbound->payload,
                                             outbound->payload_len,
                                             i,
                                             &identity) == PROTO_OK);
        assert(identity.session_id == session_ids[i]);
        assert(identity.seq == seqs[i]);
    }
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
    struct mesh_outbound outbound[2] = {
        gateway_bound_outbound(TRANSMITTER_ID),
        gateway_bound_outbound(TRANSMITTER_ID),
    };
    bool acked[2] = {false, false};
    struct app_mesh_ch9_tx_retry_entry entries[2] = {
        {.outbound = &outbound[0], .acked = &acked[0]},
        {.outbound = &outbound[1], .acked = &acked[1]},
    };
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = retry_queue_put,
        .get = retry_queue_get,
        .queue_used = retry_queue_used,
        .ctx = &queue,
    };
    struct app_mesh_ch9_tx_retry_result result;

    outbound[1].packet.seq++;
    assert(app_mesh_ch9_tx_requeue_unacked(entries,
                                           2u,
                                           100u,
                                           &ops,
                                           &result) == PROTO_OK);
    assert(result.requeued == 0u);
    assert(result.retained == 2u);
    assert(result.dropped == 0u);
    assert(!acked[0] && !acked[1]);

    queue.used = 0u;
    assert(app_mesh_ch9_tx_requeue_unacked(entries,
                                           2u,
                                           200u,
                                           &ops,
                                           &result) == PROTO_OK);
    assert(result.requeued == 1u);
    assert(result.retained == 1u);
    assert(acked[0] && !acked[1]);
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
    assert(acked[0] && acked[1]);
    assert(queue.item.packet.seq == SENT_SEQ_TEST + 1u);
}

static size_t requested_seq_payload(uint8_t *payload,
                                    size_t payload_cap,
                                    const struct mesh_outbound *sent)
{
    size_t payload_len = 0u;

    assert(mesh_append_requested_seq(payload,
                                     payload_cap,
                                     &payload_len,
                                     SENT_SEQ_TEST) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             payload_cap,
                                             &payload_len,
                                             &sent->packet,
                                             sent->payload,
                                             sent->payload_len) == PROTO_OK);
    return payload_len;
}

static size_t batched_ack_payload_with_matching_second(uint8_t *payload,
                                                       size_t payload_cap,
                                                       const struct mesh_outbound *sent)
{
    struct proto_packet first = sent->packet;
    uint8_t seq_list[2u * sizeof(uint16_t)];
    uint8_t session_list[2u * sizeof(uint32_t)];
    size_t payload_len = 0u;

    first.session_id++;
    first.seq++;
    proto_put_u16_le(&seq_list[0], (uint16_t)(SENT_SEQ_TEST + 1u));
    proto_put_u16_le(&seq_list[sizeof(uint16_t)], SENT_SEQ_TEST);
    proto_put_u32_le(&session_list[0], SESSION_ID_TEST + 1u);
    proto_put_u32_le(&session_list[sizeof(uint32_t)], SESSION_ID_TEST);

    assert(mesh_append_requested_seq(payload,
                                     payload_cap,
                                     &payload_len,
                                     first.seq) == PROTO_OK);
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
    assert(mesh_append_ack_semantic_identity(payload,
                                             payload_cap,
                                             &payload_len,
                                             &first,
                                             first.payload_len == 0u ?
                                                 NULL : sent->payload,
                                             first.payload_len) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             payload_cap,
                                             &payload_len,
                                             &sent->packet,
                                             sent->payload,
                                             sent->payload_len) == PROTO_OK);
    return payload_len;
}

static void test_ack_complete_keeps_idle_route_test_timing_open(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .cadence_parent = false,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_closes_idle_cadence_parent(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .cadence_parent = true,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
    };

    assert(app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_does_not_close_gateway_peer(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .cadence_parent = false,
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
        .cadence_parent = true,
        .report_tx_queue_used = 1u,
        .route_waiting_tx_valid = true,
        .ack_batch_valid = true,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_does_not_close_with_source_delivery_pending(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .cadence_parent = true,
        .source_delivery_pending = true,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
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
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    const size_t payload_len =
        requested_seq_payload(payload, sizeof(payload), &sent);
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
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    const size_t payload_len =
        requested_seq_payload(payload, sizeof(payload), &sent);
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
    const struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);
    const size_t payload_len =
        requested_seq_payload(payload, sizeof(payload), &sent);
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
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    const size_t payload_len =
        batched_ack_payload_with_matching_second(payload,
                                                 sizeof(payload),
                                                 &sent);
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

static void test_direct_gateway_ack_rejects_wrong_header_session(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    const size_t payload_len =
        requested_seq_payload(payload, sizeof(payload), &sent);
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

static void test_direct_gateway_ack_rejects_reused_id_different_payload(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);
    struct mesh_outbound stale = sent;
    const struct proto_packet ack_template =
        gateway_ack(TRANSMITTER_ID, 0u);
    struct proto_packet ack = ack_template;
    size_t payload_len;

    stale.packet.payload_len = 1u;
    stale.payload_len = 1u;
    stale.payload[0] = 0xa5u;
    payload_len = requested_seq_payload(payload, sizeof(payload), &stale);
    ack.payload_len = (uint16_t)payload_len;

    assert(!app_mesh_direct_gateway_ack_matches(&sent,
                                                &ack,
                                                payload,
                                                payload_len,
                                                GATEWAY_ID_TEST,
                                                GATEWAY_ID_TEST));
}

static void test_gateway_ack_relay_path_uses_single_core_tracked_packet(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet, RELAY_ID, 8u) == 1u);
    assert(app_mesh_ch9_tx_requires_tracked_single(&packet, RELAY_ID, 8u));

    packet.flags = 0u;
    assert(!app_mesh_ch9_tx_requires_tracked_single(&packet, RELAY_ID, 8u));
}

static void test_direct_local_gateway_ack_uses_single_core_owner(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = RELAY_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet,
                                         GATEWAY_ID_TEST,
                                         8u) == 1u);
    assert(app_mesh_ch9_tx_requires_tracked_single(&packet,
                                                    GATEWAY_ID_TEST,
                                                    8u));
}

static void test_gateway_ack_required_worst_case_is_always_single(void)
{
    static const uint8_t message_types[] = {
        MSG_CLICK_REPORT,
        MSG_COMMAND_RESULT,
        MSG_RESULT_BUNDLE,
        MSG_MESH_DATA,
    };
    static const uint8_t configured_limits[] = {1u, 4u, 8u, UINT8_MAX};

    for (size_t message_index = 0u;
         message_index < sizeof(message_types);
         message_index++) {
        for (size_t limit_index = 0u;
             limit_index < sizeof(configured_limits);
             limit_index++) {
            struct proto_packet packet = {
                .msg_type = message_types[message_index],
                .flags = FLAG_GATEWAY_ACK_REQUIRED,
                .src_id = RELAY_ID,
                .dst_id = GATEWAY_ID_TEST,
            };
            uint8_t configured_limit = configured_limits[limit_index];

            assert(app_mesh_ch9_tx_max_in_flight(
                       &packet, GATEWAY_ID_TEST, configured_limit) == 1u);
            assert(app_mesh_ch9_tx_requires_tracked_single(
                &packet, GATEWAY_ID_TEST, configured_limit));

            packet.src_id = TRANSMITTER_ID;
            assert(app_mesh_ch9_tx_max_in_flight(
                       &packet, RELAY_ID, configured_limit) == 1u);
            assert(app_mesh_ch9_tx_requires_tracked_single(
                &packet, RELAY_ID, configured_limit));
        }
    }
}

static void test_retry_waits_for_next_local_tx_slot(void)
{
    struct mesh_event_timing timing = {
        .event_interval_ms = 440u,
        .next_event_time_ms = 105045u,
        .event_counter = 23u,
        .guard_ms = 20u,
        .local_tx_on_even_events = true,
    };
    uint32_t prepare_ms = 0u;

    assert(app_mesh_ch9_retry_next_local_tx_prepare_ms(&timing,
                                                       30u,
                                                       &prepare_ms));
    assert(prepare_ms == 105455u);
    assert(timing.next_event_time_ms == 105045u);
    assert(timing.event_counter == 23u);

    timing.next_event_time_ms = UINT32_MAX - 439u;
    assert(app_mesh_ch9_retry_next_local_tx_prepare_ms(&timing,
                                                       30u,
                                                       &prepare_ms));
    assert(prepare_ms == UINT32_MAX - 29u);

    timing.next_event_time_ms = 105045u;
    timing.event_counter = 24u;
    assert(!app_mesh_ch9_retry_next_local_tx_prepare_ms(&timing,
                                                        30u,
                                                        &prepare_ms));
}

static void test_wait_plan_retries_at_slot_prepare_boundary(void)
{
    uint32_t delay_ms = 0u;

    assert(app_mesh_ch9_wait_plan_retry_delay_ms(315561u,
                                                  315727u,
                                                  30u,
                                                  &delay_ms));
    assert(delay_ms == 136u);

    assert(app_mesh_ch9_wait_plan_retry_delay_ms(315710u,
                                                  315727u,
                                                  30u,
                                                  &delay_ms));
    assert(delay_ms == 1u);

    assert(app_mesh_ch9_wait_plan_retry_delay_ms(UINT32_MAX - 100u,
                                                  50u,
                                                  30u,
                                                  &delay_ms));
    assert(delay_ms == 121u);
    assert(app_mesh_ch9_wait_plan_retry_delay_ms(UINT32_MAX - 100u,
                                                 0u,
                                                 30u,
                                                 &delay_ms));
    assert(delay_ms == 71u);
    assert(app_mesh_ch9_wait_plan_retry_delay_ms(1u, 0u, 30u, &delay_ms));
    assert(delay_ms == 1u);
}

static void test_core_retry_backoff_keeps_channel9_rx_available(void)
{
    struct mesh_pending_tx pending = {
        .packet.msg_type = MSG_GATEWAY_ACK_CONFIRM,
        .state = MESH_RELAY_TX_WAIT_GATEWAY_ACK,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = RELAY_ID,
    };

    assert(app_mesh_ch9_core_pending_allows_rx(&pending, true));
    pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    assert(app_mesh_ch9_core_pending_allows_rx(&pending, true));
    pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, true));
    pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, false));
    pending.state = MESH_RELAY_TX_IDLE;
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, true));
}

static void test_relay_path_stays_out_of_final_ack_batch_tracker(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.next_hop_id = RELAY_ID;
    assert(!app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
}

static void assert_pending_coordinator_decision(
    const struct mesh_pending_tx *pending,
    bool relay_tx_active,
    enum fw_radio_activity_state expected_state,
    bool expected_uwb_rx_allowed,
    bool expected_c5_tx_allowed)
{
    const struct fw_radio_activity_capture capture = {
        .relay_tx_active = relay_tx_active,
        .route_waiting_tx_active = true,
        .report_queue_used = 1u,
        .ch9_ack_wait_active =
            app_mesh_ch9_core_ack_wait_active(pending, relay_tx_active),
        .ch9_ack_receive_eligible =
            app_mesh_ch9_core_pending_allows_rx(pending, relay_tx_active),
    };
    struct fw_radio_activity_decision decision;

    assert(fw_radio_activity_decide(&capture, NULL, &decision, NULL) == 0);
    assert(decision.state == expected_state);
    assert(decision.uwb_rx_allowed == expected_uwb_rx_allowed);
    assert(decision.c5_tx_allowed == expected_c5_tx_allowed);
}

static void test_retry_backoff_keeps_coordinator_rx_visible(void)
{
    struct mesh_pending_tx pending = {
        .packet.msg_type = MSG_GATEWAY_ACK_CONFIRM,
        .state = MESH_RELAY_TX_WAIT_GATEWAY_ACK,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = RELAY_ID,
    };

    assert(app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(app_mesh_ch9_core_pending_allows_rx(&pending, true));

    pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    assert(!app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(app_mesh_ch9_core_pending_allows_rx(&pending, true));
    assert_pending_coordinator_decision(&pending,
                                        true,
                                        FW_RADIO_ACTIVITY_MESH_RX,
                                        true,
                                        true);

    pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    assert(app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(app_mesh_ch9_core_pending_allows_rx(&pending, true));
    assert_pending_coordinator_decision(&pending,
                                        true,
                                        FW_RADIO_ACTIVITY_MESH_RX,
                                        true,
                                        false);

    assert(!app_mesh_ch9_core_ack_wait_active(&pending, false));
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, false));

    pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    assert(!app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, true));
    assert_pending_coordinator_decision(&pending,
                                        true,
                                        FW_RADIO_ACTIVITY_MESH_TX,
                                        false,
                                        true);
    pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;

    pending.next_hop_id = 0u;
    assert(!app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, true));
    pending.next_hop_id = RELAY_ID;

    pending.state = MESH_RELAY_TX_WAIT_RESULT_GRANT;
    assert(!app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, true));
    pending.state = MESH_RELAY_TX_IDLE;
    assert(!app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(!app_mesh_ch9_core_pending_allows_rx(&pending, true));
    assert_pending_coordinator_decision(&pending,
                                        true,
                                        FW_RADIO_ACTIVITY_MESH_TX,
                                        false,
                                        true);

    pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    assert_pending_coordinator_decision(&pending,
                                        false,
                                        FW_RADIO_ACTIVITY_MESH_TX,
                                        false,
                                        true);
}

static void test_only_exact_forwarded_ack_route_repair_may_use_c5(void)
{
    struct mesh_pending_tx pending = {
        .state = MESH_RELAY_TX_WAIT_GATEWAY_ACK,
        .packet = {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = TRANSMITTER_ID,
            .dst_id = GATEWAY_ID_TEST,
        },
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = GATEWAY_ID_TEST,
        .transit_previous_hop_id = SECOND_RELAY_ID,
    };
    struct mesh_outbound route_request = {
        .packet = {
            .msg_type = MSG_ROUTE_REQ,
            .src_id = RELAY_ID,
            .dst_id = MESH_BROADCAST_ID,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct mesh_outbound event_propose = {
        .packet = {
            .msg_type = MSG_MESH_EVENT_PROPOSE,
            .src_id = RELAY_ID,
            .dst_id = SECOND_RELAY_ID,
        },
        .next_hop_id = SECOND_RELAY_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct mesh_outbound event_accept = event_propose;
    struct app_mesh_ch9_ack_batch batch = {
        .template_ack = {
            .packet = {
                .msg_type = MSG_GATEWAY_ACK,
                .src_id = GATEWAY_ID_TEST,
                .dst_id = TRANSMITTER_ID,
                .payload_len = 2u,
            },
            .payload = {UINT8_C(0x5a), UINT8_C(0xa5)},
            .payload_len = 2u,
            .next_hop_id = SECOND_RELAY_ID,
            .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        },
        .peer_id = SECOND_RELAY_ID,
        .count = 1u,
        .valid = true,
        .preserve_payload = true,
        .owner = APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE,
    };
    struct mesh_outbound wrong_route_request = route_request;
    struct app_mesh_c5_tx_authorization_token authorization;
    struct app_mesh_c5_tx_authorization_token event_authorization;
    struct app_mesh_c5_tx_authorization_token late_authorization;
    struct app_mesh_c5_tx_authorization_token none = {0};
    size_t route_payload_len = 0u;
    size_t wrong_route_payload_len = 0u;

    event_accept.packet.msg_type = MSG_MESH_EVENT_ACCEPT;

    assert(tlv_append_u64(route_request.payload,
                          sizeof(route_request.payload),
                          &route_payload_len,
                          TLV_RESPONDER_ID,
                          TRANSMITTER_ID) == PROTO_OK);
    route_request.payload_len = (uint16_t)route_payload_len;
    route_request.packet.payload_len = (uint16_t)route_payload_len;
    assert(tlv_append_u64(wrong_route_request.payload,
                          sizeof(wrong_route_request.payload),
                          &wrong_route_payload_len,
                          TLV_RESPONDER_ID,
                          SECOND_RELAY_ID) == PROTO_OK);
    wrong_route_request.payload_len = (uint16_t)wrong_route_payload_len;
    wrong_route_request.packet.payload_len = (uint16_t)wrong_route_payload_len;

    /* Before a forwarded batch exists, only a captured authorization for the
     * exact retained transit owner opens the exact child route request. */
    assert(app_mesh_ch9_c5_repair_authorization_capture(
        &authorization,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR,
        &pending, true, NULL, TRANSMITTER_ID));
    assert(app_mesh_ch9_c5_repair_owner_matches(
        &authorization, &pending, true, NULL));
    assert(app_mesh_ch9_c5_repair_allowed(
        &authorization, &pending, true, NULL, &route_request));
    assert(!app_mesh_ch9_c5_repair_allowed(
        &none, &pending, true, NULL, &route_request));
    assert(!app_mesh_ch9_c5_repair_allowed(
        &authorization, &pending, true, NULL, &wrong_route_request));
    assert(!app_mesh_ch9_c5_repair_allowed(
        &authorization, &pending, true, NULL, &event_propose));
    assert(!app_mesh_ch9_c5_repair_authorization_capture(
        &none,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR,
        &pending, true, NULL, SECOND_RELAY_ID));
    assert(!none.valid);

    pending.packet.seq++;
    assert(!app_mesh_ch9_c5_repair_owner_matches(
        &authorization, &pending, true, NULL));
    pending.packet.seq--;
    pending.packet.flags = 0u;
    assert(!app_mesh_ch9_c5_repair_authorization_capture(
        &none,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR,
        &pending, true, NULL, TRANSMITTER_ID));
    pending.packet.flags = FLAG_GATEWAY_ACK_REQUIRED;

    /* After the gateway ACK is retained, event timing repair targets the
     * immediate relay while the logical ACK remains addressed to the deeper
     * packet origin. It is also bound to the transit owner and ACK digest. */
    pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    pending.gateway_ack_forward_pending = true;
    assert(app_mesh_ch9_c5_repair_authorization_capture(
        &event_authorization,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
        &pending, true, &batch, SECOND_RELAY_ID));
    assert(event_authorization.retained_ack_valid);
    assert(!app_mesh_ch9_c5_repair_allowed(
        &event_authorization, &pending, true, &batch, &event_propose));
    assert(app_mesh_ch9_c5_repair_allowed(
        &event_authorization, &pending, true, &batch, &event_accept));
    assert(!app_mesh_ch9_c5_repair_allowed(
        &event_authorization, &pending, true, &batch, &route_request));
    assert(!app_mesh_ch9_c5_repair_authorization_capture(
        &none,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
        &pending, true, &batch, TRANSMITTER_ID));
    assert(!none.valid);

    batch.preserve_payload = false;
    assert(!app_mesh_ch9_c5_repair_owner_matches(
        &event_authorization, &pending, true, &batch));
    batch.preserve_payload = true;
    batch.template_ack.packet.seq++;
    assert(!app_mesh_ch9_c5_repair_owner_matches(
        &event_authorization, &pending, true, &batch));
    batch.template_ack.packet.seq--;
    pending.gateway_ack_forward_pending = false;
    assert(!app_mesh_ch9_c5_repair_owner_matches(
        &event_authorization, &pending, true, &batch));

    /* A late terminal ACK owns event repair through its retained bytes, even
     * though the exact transit core has already been released. */
    batch.owner = APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD;
    assert(app_mesh_ch9_c5_repair_authorization_capture(
        &late_authorization,
        APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR,
        NULL, false, &batch, SECOND_RELAY_ID));
    assert(app_mesh_ch9_c5_repair_owner_matches(
        &late_authorization, NULL, false, &batch));
    assert(app_mesh_ch9_c5_repair_allowed(
        &late_authorization, NULL, false, &batch, &event_propose));
    assert(!app_mesh_ch9_c5_repair_allowed(
        &late_authorization, NULL, false, &batch, &route_request));
    assert(!app_mesh_ch9_c5_repair_authorization_capture(
        &none,
        APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR,
        NULL, false, &batch, TRANSMITTER_ID));
    assert(!app_mesh_ch9_c5_repair_authorization_capture(
        &none,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
        NULL, false, &batch, SECOND_RELAY_ID));

    batch.template_ack.payload[0] ^= UINT8_C(0xff);
    assert(!app_mesh_ch9_c5_repair_owner_matches(
        &late_authorization, NULL, false, &batch));
    batch.template_ack.payload[0] ^= UINT8_C(0xff);
    batch.owner = APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE;
    assert(!app_mesh_ch9_c5_repair_owner_matches(
        &late_authorization, NULL, false, &batch));
    batch.owner = APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD;
    batch.template_ack.packet.msg_type = MSG_MESH_HOP_ACK;
    assert(!app_mesh_ch9_c5_repair_authorization_capture(
        &none,
        APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR,
        NULL, false, &batch, SECOND_RELAY_ID));
}

static void test_durable_gateway_result_stays_in_core_tracker(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);

    sent.packet.msg_type = MSG_COMMAND_RESULT;
    assert(!app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
    assert(app_mesh_ch9_tx_max_in_flight(&sent.packet,
                                         GATEWAY_ID_TEST,
                                         8u) == 1u);

    sent.packet.msg_type = MSG_RESULT_BUNDLE;
    assert(!app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
    assert(app_mesh_ch9_tx_max_in_flight(&sent.packet,
                                         GATEWAY_ID_TEST,
                                         8u) == 1u);
}

static void test_anchor_tracks_transit_direct_gateway_send(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_tx_should_track_sent(&sent, RELAY_ID));
}

static void test_forced_hop_transit_retries_without_local_pressure(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, false, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_RETRY);
}

static void test_local_priority_pressure_preserves_transit_timeout_owner(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_RETRY);
}

static void test_downstream_pressure_defers_nonpriority_local_timeout(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);

    sent.packet.msg_type = MSG_ANCHOR_HEARTBEAT;
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, false, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_DEFER_LOCAL);
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_DEFER_LOCAL);
}

static void test_downstream_pressure_allows_local_click_preemption(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(RELAY_ID);

    sent.packet.msg_type = MSG_CLICK_REPORT;
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, false, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL);

    sent.packet.msg_type = MSG_COMMAND_RESULT;
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL);
}

static void test_timeout_pressure_is_inactive_without_downstream(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, false, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_RETRY);
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, false, true, true, RELAY_ID) ==
           APP_MESH_CH9_TIMEOUT_RETRY);
    assert(app_mesh_ch9_timeout_pressure_decide(
               &sent, true, true, true, 0u) ==
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

static void test_zero_configured_in_flight_stays_zero(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = RELAY_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet, GATEWAY_ID_TEST, 0u) == 0u);
    assert(!app_mesh_ch9_tx_requires_tracked_single(&packet,
                                                     GATEWAY_ID_TEST,
                                                     0u));
}

static void test_non_ack_transit_keeps_configured_in_flight_limit(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID_TEST,
    };

    assert(app_mesh_ch9_tx_max_in_flight(&packet, RELAY_ID, 8u) == 8u);
}

static void test_direct_gateway_timeout_counts_route_failure(void)
{
    const struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    assert(app_mesh_ch9_tx_timeout_counts_route_failure(&sent,
                                                        GATEWAY_ID_TEST,
                                                        GATEWAY_ID_TEST));
}

static void test_selected_relay_timeout_counts_route_failure(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.next_hop_id = RELAY_ID;

    assert(app_mesh_ch9_tx_timeout_counts_route_failure(&sent,
                                                        RELAY_ID,
                                                        GATEWAY_ID_TEST));
}

static void test_other_peer_timeout_cannot_charge_this_route(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.next_hop_id = RELAY_ID;

    assert(!app_mesh_ch9_tx_timeout_counts_route_failure(
        &sent, SECOND_RELAY_ID, GATEWAY_ID_TEST));
}

static void test_non_gateway_ack_timeout_does_not_count_route_failure(void)
{
    struct mesh_outbound sent = gateway_bound_outbound(TRANSMITTER_ID);

    sent.packet.flags = 0u;
    assert(!app_mesh_ch9_tx_timeout_counts_route_failure(&sent,
                                                         GATEWAY_ID_TEST,
                                                         GATEWAY_ID_TEST));

    sent = gateway_bound_outbound(TRANSMITTER_ID);
    sent.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    assert(!app_mesh_ch9_tx_timeout_counts_route_failure(&sent,
                                                         GATEWAY_ID_TEST,
                                                         GATEWAY_ID_TEST));

    sent = gateway_bound_outbound(TRANSMITTER_ID);
    assert(!app_mesh_ch9_tx_timeout_counts_route_failure(&sent,
                                                         0u,
                                                         GATEWAY_ID_TEST));
}

static void test_ack_table_interleaves_two_peers(void)
{
    struct app_mesh_ch9_ack_table table;
    struct mesh_outbound relay_ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound downstream_ack =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry relay_first =
        ack_batch_entry(UINT32_C(0x1001), UINT16_C(0x0011), UINT32_C(101));
    struct app_mesh_ch9_ack_batch_entry downstream =
        ack_batch_entry(UINT32_C(0x2001), UINT16_C(0x0021), UINT32_C(201));
    struct app_mesh_ch9_ack_batch_entry relay_second =
        ack_batch_entry(UINT32_C(0x1002), UINT16_C(0x0012), UINT32_C(102));
    enum app_mesh_ch9_ack_queue_result result;
    struct mesh_outbound built;
    const uint32_t relay_sessions[] = {UINT32_C(0x1001), UINT32_C(0x1002)};
    const uint16_t relay_seqs[] = {UINT16_C(0x0011), UINT16_C(0x0012)};
    const uint32_t relay_packet_ids[] = {UINT32_C(101), UINT32_C(102)};
    const uint32_t downstream_sessions[] = {UINT32_C(0x2001)};
    const uint16_t downstream_seqs[] = {UINT16_C(0x0021)};
    const uint32_t downstream_packet_ids[] = {UINT32_C(201)};

    app_mesh_ch9_ack_table_init(&table);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &relay_ack,
                                        &relay_first,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &downstream_ack,
                                        &downstream,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &relay_ack,
                                        &relay_second,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);

    assert(app_mesh_ch9_ack_table_peer_count(&table) == 2u);
    assert(app_mesh_ch9_ack_table_any_pending(&table));
    assert(app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)->count == 2u);
    assert(app_mesh_ch9_ack_table_get_peer(&table, TRANSMITTER_ID)->count == 1u);

    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             RELAY_ID,
                                             &built) == PROTO_OK);
    assert(built.next_hop_id == RELAY_ID);
    assert_built_ack_entries(&built,
                             relay_sessions,
                             relay_seqs,
                             relay_packet_ids,
                             2u);
    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             TRANSMITTER_ID,
                                             &built) == PROTO_OK);
    assert(built.next_hop_id == TRANSMITTER_ID);
    assert_built_ack_entries(&built,
                             downstream_sessions,
                             downstream_seqs,
                             downstream_packet_ids,
                             1u);
}

static void test_ack_table_timeout_clear_is_peer_scoped(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound relay_ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound downstream_ack =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry relay_entry =
        ack_batch_entry(1u, 1u, 1u);
    struct app_mesh_ch9_ack_batch_entry downstream_entry =
        ack_batch_entry(2u, 2u, 2u);
    struct mesh_outbound built;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &relay_ack,
                                        &relay_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &downstream_ack,
                                        &downstream_entry,
                                        NULL) == PROTO_OK);

    assert(app_mesh_ch9_ack_table_clear_peer(&table, RELAY_ID));
    assert(!app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));
    assert(app_mesh_ch9_ack_table_peer_count(&table) == 1u);
    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             TRANSMITTER_ID,
                                             &built) == PROTO_OK);
    assert(built.next_hop_id == TRANSMITTER_ID);
    assert(!app_mesh_ch9_ack_table_clear_peer(&table, RELAY_ID));
}

static void test_ack_table_duplicate_is_scoped_by_session_and_sequence(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry first =
        ack_batch_entry(UINT32_C(0x1010), UINT16_C(0x0055), UINT32_C(10));
    struct app_mesh_ch9_ack_batch_entry duplicate = first;
    struct app_mesh_ch9_ack_batch_entry same_seq_new_session =
        ack_batch_entry(UINT32_C(0x2020), UINT16_C(0x0055), UINT32_C(20));
    enum app_mesh_ch9_ack_queue_result result;
    const struct app_mesh_ch9_ack_batch *batch;

    duplicate.packet_id = UINT32_C(99);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &first,
                                        &result) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &duplicate,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
    batch = app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID);
    assert(batch != NULL && batch->count == 1u);
    assert(batch->entries[0].packet_id == UINT32_C(10));

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &same_seq_new_session,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)->count == 2u);
}

static void test_ack_table_rejects_same_id_different_semantic_packet(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    const struct app_mesh_ch9_ack_batch_entry entry =
        ack_batch_entry(UINT32_C(0x1010),
                        UINT16_C(0x0055),
                        UINT32_C(10));
    struct proto_packet conflicting_packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = RELAY_ID,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = entry.session_id,
        .seq = entry.seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 1u,
    };
    const uint8_t conflicting_payload[1] = {0xa5u};
    enum app_mesh_ch9_ack_queue_result result;
    size_t payload_len = 0u;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &entry,
                                        &result) == PROTO_OK);
    ack.packet.session_id = entry.session_id;
    assert(mesh_append_requested_seq(ack.payload,
                                     sizeof(ack.payload),
                                     &payload_len,
                                     entry.seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack.payload,
                                             sizeof(ack.payload),
                                             &payload_len,
                                             &conflicting_packet,
                                             conflicting_payload,
                                             sizeof(conflicting_payload)) ==
           PROTO_OK);
    ack.payload_len = (uint16_t)payload_len;
    ack.packet.payload_len = (uint16_t)payload_len;
    assert(queue_raw_ack(&table, &ack, &entry, &result) ==
           PROTO_ERR_MALFORMED);
    assert(result == APP_MESH_CH9_ACK_QUEUE_SEMANTIC_CONFLICT);
    assert(app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)->count == 1u);
}

static void test_ack_table_pressure_rejects_without_eviction_and_reuses_slot(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound first_ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound second_ack =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound third_ack =
        ack_outbound(SECOND_RELAY_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry first = ack_batch_entry(1u, 1u, 1u);
    struct app_mesh_ch9_ack_batch_entry second = ack_batch_entry(2u, 2u, 2u);
    struct app_mesh_ch9_ack_batch_entry third = ack_batch_entry(3u, 3u, 3u);
    struct app_mesh_ch9_ack_batch_entry first_again =
        ack_batch_entry(4u, 4u, 4u);
    enum app_mesh_ch9_ack_queue_result result;

    assert(APP_MESH_CH9_ACK_PEER_MAX == 2u);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &first_ack,
                                        &first,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &second_ack,
                                        &second,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &third_ack,
                                        &third,
                                        &result) == PROTO_ERR_NO_SPACE);
    assert(result == APP_MESH_CH9_ACK_QUEUE_TABLE_FULL);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));
    assert(!app_mesh_ch9_ack_table_pending_for_peer(&table, SECOND_RELAY_ID));

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &first_ack,
                                        &first_again,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)->count == 2u);

    assert(app_mesh_ch9_ack_table_clear_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &third_ack,
                                        &third,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(table.batches[0].peer_id == SECOND_RELAY_ID);
    assert(table.batches[1].peer_id == TRANSMITTER_ID);
}

static void test_ack_send_failure_attempt_budget_releases_peer_slot(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry entry = ack_batch_entry(1u, 1u, 1u);
    uint32_t delay_ms = 0u;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &entry,
                                        NULL) == PROTO_OK);
    for (uint8_t attempt = 1u;
         attempt < APP_MESH_CH9_ACK_SEND_FAILURE_MAX;
         attempt++) {
        assert(app_mesh_ch9_ack_table_note_send_failure(
                   &table,
                   RELAY_ID,
                   UINT32_C(1000) + attempt,
                   attempt,
                   &delay_ms) == PROTO_OK);
        assert(delay_ms > 0u);
        assert(app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    }
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table,
               RELAY_ID,
               UINT32_C(2000),
               UINT32_C(99),
               &delay_ms) == PROTO_ERR_STALE);
    assert(delay_ms == 0u);
    assert(!app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_peer_count(&table) == 0u);
}

static void test_ack_send_failure_lifetime_releases_peer_slot(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry entry = ack_batch_entry(1u, 1u, 1u);
    uint32_t delay_ms = 0u;
    const uint32_t first_failure_ms = UINT32_C(1000);

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table,
               RELAY_ID,
               first_failure_ms,
               UINT32_C(1),
               &delay_ms) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table,
               RELAY_ID,
               first_failure_ms + APP_MESH_CH9_ACK_OWNER_LIFETIME_MS,
               UINT32_C(2),
               &delay_ms) == PROTO_ERR_STALE);
    assert(delay_ms == 0u);
    assert(!app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
}

struct ack_flush_fixture {
    struct mesh_outbound outbound;
    int ret;
    uint8_t calls;
};

static int ack_flush_capture(const struct mesh_outbound *outbound, void *ctx)
{
    struct ack_flush_fixture *fixture = ctx;

    fixture->outbound = *outbound;
    fixture->calls++;
    return fixture->ret;
}

static void test_ack_table_flush_retains_failure_and_clears_only_sent_peer(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound relay_ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound downstream_ack =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry relay_entry =
        ack_batch_entry(1u, 1u, 1u);
    struct app_mesh_ch9_ack_batch_entry downstream_entry =
        ack_batch_entry(2u, 2u, 2u);
    struct ack_flush_fixture fixture = {.ret = -EBUSY};

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &relay_ack,
                                        &relay_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &downstream_ack,
                                        &downstream_entry,
                                        NULL) == PROTO_OK);

    assert(app_mesh_ch9_ack_table_flush_peer(&table,
                                             RELAY_ID,
                                             ack_flush_capture,
                                             &fixture) == -EBUSY);
    assert(fixture.calls == 1u);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));

    fixture.ret = 0;
    assert(app_mesh_ch9_ack_table_flush_peer(&table,
                                             RELAY_ID,
                                             ack_flush_capture,
                                             &fixture) == 0);
    assert(fixture.calls == 2u);
    assert(fixture.outbound.next_hop_id == RELAY_ID);
    assert(!app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));
}

static void test_forwarded_gateway_ack_does_not_replace_other_peer(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound other_ack =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound forwarded = ack_outbound(RELAY_ID, MSG_GATEWAY_ACK);
    struct mesh_outbound hop_ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry other_entry =
        ack_batch_entry(1u, 1u, 1u);
    struct app_mesh_ch9_ack_batch_entry hop_entry =
        ack_batch_entry(2u, 2u, 2u);
    enum app_mesh_ch9_ack_queue_result result;
    struct mesh_outbound built;

    forwarded.payload[0] = UINT8_C(0x5a);
    forwarded.payload[1] = UINT8_C(0xa5);
    forwarded.payload_len = 2u;
    forwarded.packet.payload_len = 2u;
    /* The ACK is routed through RELAY_ID to a deeper logical descendant. */
    forwarded.packet.dst_id = TRANSMITTER_ID;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &other_ack,
                                        &other_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &forwarded,
                                                  &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &hop_ack,
                                        &hop_entry,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_SUPPRESSED_BY_FORWARDED_ACK);
    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             RELAY_ID,
                                             &built) == PROTO_OK);
    assert(built.packet.msg_type == MSG_GATEWAY_ACK);
    assert(built.packet.dst_id == TRANSMITTER_ID);
    assert(built.payload_len == 2u);
    assert(memcmp(built.payload, forwarded.payload, 2u) == 0);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));
}

static void test_overlapping_forwarded_gateway_acks_preserve_first_peer_custody(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound first = ack_outbound(RELAY_ID, MSG_GATEWAY_ACK);
    struct mesh_outbound duplicate;
    struct mesh_outbound overlapping;
    struct mesh_outbound built;
    enum app_mesh_ch9_ack_queue_result result;
    uint32_t retry_delay_ms = 0u;

    first.packet.session_id = UINT32_C(0x12345678);
    first.packet.seq = UINT16_C(0x1111);
    first.payload[0] = UINT8_C(0x5a);
    first.payload[1] = UINT8_C(0xa5);
    first.payload_len = 2u;
    first.packet.payload_len = 2u;
    duplicate = first;
    overlapping = first;
    overlapping.packet.seq++;
    overlapping.payload[0] ^= UINT8_C(0xff);

    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &first,
                                                  &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table, RELAY_ID, 1000u, 7u, &retry_delay_ms) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &duplicate,
                                                  &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
    assert(app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)->retry_round ==
           1u);

    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &overlapping,
                                                  &result) ==
           PROTO_ERR_NO_SPACE);
    assert(result == APP_MESH_CH9_ACK_QUEUE_FORWARDED_BUSY);
    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             RELAY_ID,
                                             &built) == PROTO_OK);
    assert(built.packet.session_id == first.packet.session_id);
    assert(built.packet.seq == first.packet.seq);
    assert(built.payload_len == first.payload_len);
    assert(memcmp(built.payload, first.payload, first.payload_len) == 0);
}

static void test_forwarded_gateway_ack_owner_class_tracks_real_custody(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound forwarded = ack_outbound(RELAY_ID, MSG_GATEWAY_ACK);
    enum app_mesh_ch9_ack_queue_result result;
    const struct app_mesh_ch9_ack_batch *batch;

    forwarded.packet.session_id = UINT32_C(0x31415926);
    forwarded.packet.seq = UINT16_C(0x2718);
    forwarded.packet.dst_id = TRANSMITTER_ID;
    forwarded.payload[0] = UINT8_C(0x5a);
    forwarded.payload[1] = UINT8_C(0xa5);
    forwarded.payload_len = 2u;
    forwarded.packet.payload_len = 2u;

    assert(app_mesh_ch9_ack_table_queue_late_forwarded(
               &table, &forwarded, &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    batch = app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID);
    assert(batch != NULL && batch->preserve_payload);
    assert(batch->owner == APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD);

    /* A live transit owner may strengthen an exact queued replay, while a
     * later unowned copy must never weaken that custody. */
    assert(app_mesh_ch9_ack_table_queue_forwarded(
               &table, &forwarded, &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
    assert(app_mesh_ch9_ack_table_get_peer(
               &table, RELAY_ID)->owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE);
    assert(app_mesh_ch9_ack_table_queue_late_forwarded(
               &table, &forwarded, &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
    assert(app_mesh_ch9_ack_table_get_peer(
               &table, RELAY_ID)->owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE);

    assert(app_mesh_ch9_ack_table_clear_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_queue_late_forwarded(
               &table, &forwarded, &result) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_get_peer(
               &table, RELAY_ID)->owner ==
           APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD);
}

static void test_forwarded_gateway_ack_table_full_preserves_existing_peers(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound first = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound second =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound forwarded =
        ack_outbound(SECOND_RELAY_ID, MSG_GATEWAY_ACK);
    struct app_mesh_ch9_ack_batch_entry first_entry =
        ack_batch_entry(1u, 1u, 1u);
    struct app_mesh_ch9_ack_batch_entry second_entry =
        ack_batch_entry(2u, 2u, 2u);
    enum app_mesh_ch9_ack_queue_result result;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &first,
                                        &first_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &second,
                                        &second_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &forwarded,
                                                  &result) ==
           PROTO_ERR_NO_SPACE);
    assert(result == APP_MESH_CH9_ACK_QUEUE_TABLE_FULL);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, TRANSMITTER_ID));
    assert(!app_mesh_ch9_ack_table_pending_for_peer(&table,
                                                     SECOND_RELAY_ID));
}

static void test_ack_send_failure_keeps_exact_peer_custody_until_random_backoff(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound forwarded = ack_outbound(RELAY_ID, MSG_GATEWAY_ACK);
    struct mesh_outbound other =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry other_entry =
        ack_batch_entry(UINT32_C(0x2200), UINT16_C(0x33), UINT32_C(44));
    struct mesh_outbound built_before;
    struct mesh_outbound built_after;
    uint32_t first_delay_ms = 0u;
    uint32_t second_delay_ms = 0u;
    const uint32_t now_ms = UINT32_C(1000);

    forwarded.payload[0] = UINT8_C(0x5a);
    forwarded.payload[1] = UINT8_C(0xa5);
    forwarded.payload_len = 2u;
    forwarded.packet.payload_len = 2u;

    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &forwarded,
                                                  NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &other,
                                        &other_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             RELAY_ID,
                                             &built_before) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table, RELAY_ID, now_ms, UINT32_C(0),
               &first_delay_ms) == PROTO_OK);
    assert(first_delay_ms == APP_MESH_CH9_ACK_RETRY_BASE_MS / 2u);
    assert(!app_mesh_ch9_ack_table_retry_ready(&table, RELAY_ID, now_ms));
    assert(app_mesh_ch9_ack_table_retry_ready(&table,
                                               TRANSMITTER_ID,
                                               now_ms));
    assert(app_mesh_ch9_ack_table_retry_wait_ms(&table,
                                                 RELAY_ID,
                                                 now_ms) == first_delay_ms);
    assert(app_mesh_ch9_ack_table_build_peer(&table,
                                             RELAY_ID,
                                             &built_after) == PROTO_OK);
    assert(built_after.packet.msg_type == built_before.packet.msg_type);
    assert(built_after.packet.src_id == built_before.packet.src_id);
    assert(built_after.packet.dst_id == built_before.packet.dst_id);
    assert(built_after.packet.session_id == built_before.packet.session_id);
    assert(built_after.packet.seq == built_before.packet.seq);
    assert(built_after.next_hop_id == built_before.next_hop_id);
    assert(built_after.payload_len == built_before.payload_len);
    assert(memcmp(built_after.payload,
                  built_before.payload,
                  built_before.payload_len) == 0);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));

    assert(!app_mesh_ch9_ack_table_retry_ready(
        &table, RELAY_ID, now_ms + first_delay_ms - 1u));
    assert(app_mesh_ch9_ack_table_retry_ready(
        &table, RELAY_ID, now_ms + first_delay_ms));
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table, RELAY_ID, now_ms + first_delay_ms,
               UINT32_C(100), &second_delay_ms) == PROTO_OK);
    assert(second_delay_ms >= APP_MESH_CH9_ACK_RETRY_BASE_MS);
    assert(second_delay_ms <= APP_MESH_CH9_ACK_RETRY_BASE_MS * 3u);
    assert(second_delay_ms != first_delay_ms);
    assert(app_mesh_ch9_ack_table_pending_for_peer(&table, RELAY_ID));
}

static void test_assignment_hop_ack_cleanup_classification_survives_send_failure(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound assignment_ack =
        ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound ordinary_ack =
        ack_outbound(TRANSMITTER_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound forwarded_ack =
        ack_outbound(SECOND_RELAY_ID, MSG_GATEWAY_ACK);
    struct app_mesh_ch9_ack_batch_entry assignment_entry =
        ack_batch_entry(UINT32_C(0x3300), UINT16_C(0x44), UINT32_C(55));
    struct app_mesh_ch9_ack_batch_entry ordinary_entry =
        ack_batch_entry(UINT32_C(0x3301), UINT16_C(0x45), UINT32_C(56));
    const struct app_mesh_ch9_ack_batch *batch;
    uint32_t retry_delay_ms = 0u;

    assignment_entry.assignment_turn_action =
        APP_MESH_CH9_ASSIGNMENT_TURN_RETIRE;
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &assignment_ack,
                                        &assignment_entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ordinary_ack,
                                        &ordinary_entry,
                                        NULL) == PROTO_OK);

    batch = app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID);
    assert(batch != NULL);
    assert(app_mesh_ch9_ack_batch_consumes_next_peer_turn(batch));
    assert(app_mesh_ch9_ack_batch_retires_peer_timing(batch));
    batch = app_mesh_ch9_ack_table_get_peer(&table, TRANSMITTER_ID);
    assert(batch != NULL);
    assert(!app_mesh_ch9_ack_batch_consumes_next_peer_turn(batch));
    assert(!app_mesh_ch9_ack_batch_retires_peer_timing(batch));

    /* A failed physical send keeps both ACK custody and the deferred one-turn
     * marker. The later successful send remains the only boundary allowed to
     * consume the next reciprocal turn; the reusable timing itself remains. */
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table,
               RELAY_ID,
               UINT32_C(4000),
               UINT32_C(17),
               &retry_delay_ms) == PROTO_OK);
    assert(retry_delay_ms > 0u);
    batch = app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID);
    assert(batch != NULL && batch->retry_deferred);
    assert(app_mesh_ch9_ack_batch_consumes_next_peer_turn(batch));
    assert(app_mesh_ch9_ack_batch_retires_peer_timing(batch));

    forwarded_ack.packet.dst_id = TRANSMITTER_ID;
    forwarded_ack.payload[0] = UINT8_C(0xa5);
    forwarded_ack.payload_len = 1u;
    forwarded_ack.packet.payload_len = 1u;
    assert(app_mesh_ch9_ack_table_clear_peer(&table, TRANSMITTER_ID));
    assert(app_mesh_ch9_ack_table_queue_forwarded(&table,
                                                  &forwarded_ack,
                                                  NULL) == PROTO_OK);
    batch = app_mesh_ch9_ack_table_get_peer(&table, SECOND_RELAY_ID);
    assert(batch != NULL && batch->preserve_payload);
    assert(!app_mesh_ch9_ack_batch_consumes_next_peer_turn(batch));
    assert(!app_mesh_ch9_ack_batch_retires_peer_timing(batch));

    assert(app_mesh_ch9_ack_table_clear_peer(&table, RELAY_ID));
    assert(!app_mesh_ch9_ack_batch_consumes_next_peer_turn(
        app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)));
    assert(!app_mesh_ch9_ack_batch_retires_peer_timing(
        app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)));

    /* CLAIM consumes the now-empty reciprocal turn but deliberately retains
     * the assignment cadence for the later TABLE response. */
    assignment_entry.session_id++;
    assignment_entry.seq++;
    assignment_entry.packet_id++;
    assignment_entry.assignment_turn_action =
        APP_MESH_CH9_ASSIGNMENT_TURN_CONSUME;
    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &assignment_ack,
                                        &assignment_entry,
                                        NULL) == PROTO_OK);
    batch = app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID);
    assert(batch != NULL);
    assert(app_mesh_ch9_ack_batch_consumes_next_peer_turn(batch));
    assert(!app_mesh_ch9_ack_batch_retires_peer_timing(batch));
}

static void test_duplicate_sender_retry_does_not_reset_ack_retry_round(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct app_mesh_ch9_ack_batch_entry entry =
        ack_batch_entry(UINT32_C(0x4411), UINT16_C(0x55), UINT32_C(66));
    enum app_mesh_ch9_ack_queue_result result;
    uint32_t delay_ms = 0u;
    uint32_t retry_not_before_ms;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &entry,
                                        NULL) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_note_send_failure(
               &table, RELAY_ID, UINT32_C(2000), UINT32_C(17),
               &delay_ms) == PROTO_OK);
    retry_not_before_ms = UINT32_C(2000) + delay_ms;

    assert(app_mesh_ch9_ack_table_queue(&table,
                                        &ack,
                                        &entry,
                                        &result) == PROTO_OK);
    assert(result == APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
    assert(app_mesh_ch9_ack_table_get_peer(&table, RELAY_ID)->retry_round == 1u);
    assert(app_mesh_ch9_ack_table_get_peer(
               &table, RELAY_ID)->retry_not_before_ms == retry_not_before_ms);
    assert(!app_mesh_ch9_ack_table_retry_ready(
        &table, RELAY_ID, retry_not_before_ms - 1u));
    assert(app_mesh_ch9_ack_table_retry_ready(
        &table, RELAY_ID, retry_not_before_ms));
}

static void test_c5_flow_ack_batch_preserves_exact_identities(void)
{
    struct app_mesh_ch9_ack_table table = {0};
    struct mesh_outbound ack = ack_outbound(RELAY_ID, MSG_MESH_HOP_ACK);
    struct mesh_outbound built;
    const uint32_t sessions[] = {0x101u, 0x102u, 0x103u};
    const uint16_t seqs[] = {11u, 12u, 13u};
    const uint32_t packet_ids[] = {101u, 102u, 103u};
    struct mesh_ack_semantic_identity identities[3];
    struct mesh_ack_flow_control flow = {
        .depth = 1u, .credit = 3u, .depth_valid = true, .credit_valid = true,
    };

    ack.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    for (size_t i = 0u; i < 3u; i++) {
        struct app_mesh_ch9_ack_batch_entry entry =
            ack_batch_entry(sessions[i], seqs[i], packet_ids[i]);
        bind_ack_to_batch_entry(&ack, &entry);
        assert(mesh_ack_semantic_identity_at(ack.payload, ack.payload_len,
                                             0u, &identities[i]) == PROTO_OK);
        size_t len = ack.payload_len;
        assert(mesh_append_ack_flow_control(ack.payload, sizeof(ack.payload),
                                             &len, &flow) == PROTO_OK);
        ack.payload_len = (uint16_t)len;
        ack.packet.payload_len = (uint16_t)len;
        assert(queue_raw_ack(&table, &ack, &entry, NULL) == PROTO_OK);
    }
    assert(app_mesh_ch9_ack_table_build_peer(&table, RELAY_ID, &built) == PROTO_OK);
    assert(built.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert_built_ack_entries(&built, sessions, seqs, packet_ids, 3u);
    for (size_t i = 0u; i < 3u; i++) {
        struct mesh_ack_semantic_identity actual;
        assert(mesh_ack_semantic_identity_at(built.payload, built.payload_len,
                                             (uint8_t)i, &actual) == PROTO_OK);
        assert(memcmp(actual.digest, identities[i].digest,
                      sizeof(actual.digest)) == 0);
    }
    struct mesh_ack_flow_control parsed;
    assert(mesh_ack_flow_control_parse(&built.packet, built.payload,
                                       built.payload_len, &parsed) == PROTO_OK);
    assert(parsed.depth_valid && parsed.depth == flow.depth);
    assert(parsed.credit_valid && parsed.credit == flow.credit);
    assert(parsed.identity_count == 3u);
}

int main(void)
{
    test_c5_flow_ack_batch_preserves_exact_identities();
    test_ack_table_interleaves_two_peers();
    test_ack_table_timeout_clear_is_peer_scoped();
    test_ack_table_duplicate_is_scoped_by_session_and_sequence();
    test_ack_table_rejects_same_id_different_semantic_packet();
    test_ack_table_pressure_rejects_without_eviction_and_reuses_slot();
    test_ack_send_failure_attempt_budget_releases_peer_slot();
    test_ack_send_failure_lifetime_releases_peer_slot();
    test_ack_table_flush_retains_failure_and_clears_only_sent_peer();
    test_forwarded_gateway_ack_does_not_replace_other_peer();
    test_overlapping_forwarded_gateway_acks_preserve_first_peer_custody();
    test_forwarded_gateway_ack_owner_class_tracks_real_custody();
    test_forwarded_gateway_ack_table_full_preserves_existing_peers();
    test_ack_send_failure_keeps_exact_peer_custody_until_random_backoff();
    test_assignment_hop_ack_cleanup_classification_survives_send_failure();
    test_duplicate_sender_retry_does_not_reset_ack_retry_round();
    test_ack_complete_keeps_idle_route_test_timing_open();
    test_ack_complete_closes_idle_cadence_parent();
    test_ack_complete_does_not_close_gateway_peer();
    test_ack_complete_does_not_close_when_work_remains();
    test_ack_complete_does_not_close_with_source_delivery_pending();
    test_ack_complete_policy_is_disabled_outside_route_test();
    test_direct_gateway_ack_matches_transit_original_source();
    test_direct_gateway_ack_rejects_relay_address_for_transit();
    test_direct_gateway_ack_matches_local_source();
    test_direct_gateway_ack_matches_batched_session_list();
    test_direct_gateway_ack_rejects_wrong_header_session();
    test_direct_gateway_ack_rejects_reused_id_different_payload();
    test_gateway_ack_relay_path_uses_single_core_tracked_packet();
    test_direct_local_gateway_ack_uses_single_core_owner();
    test_gateway_ack_required_worst_case_is_always_single();
    test_retry_waits_for_next_local_tx_slot();
    test_wait_plan_retries_at_slot_prepare_boundary();
    test_core_retry_backoff_keeps_channel9_rx_available();
    test_relay_path_stays_out_of_final_ack_batch_tracker();
    test_retry_backoff_keeps_coordinator_rx_visible();
    test_only_exact_forwarded_ack_route_repair_may_use_c5();
    test_durable_gateway_result_stays_in_core_tracker();
    test_anchor_tracks_transit_direct_gateway_send();
    test_forced_hop_transit_retries_without_local_pressure();
    test_local_priority_pressure_preserves_transit_timeout_owner();
    test_downstream_pressure_defers_nonpriority_local_timeout();
    test_downstream_pressure_allows_local_click_preemption();
    test_timeout_pressure_is_inactive_without_downstream();
    test_local_direct_gateway_send_tracks_ack();
    test_local_destination_does_not_track_ack();
    test_non_ch9_send_does_not_track_ack();
    test_zero_configured_in_flight_stays_zero();
    test_non_ack_transit_keeps_configured_in_flight_limit();
    test_direct_gateway_timeout_counts_route_failure();
    test_selected_relay_timeout_counts_route_failure();
    test_other_peer_timeout_cannot_charge_this_route();
    test_non_gateway_ack_timeout_does_not_count_route_failure();
    test_unacked_retry_retains_ownership_until_queue_admits();
    return 0;
}
