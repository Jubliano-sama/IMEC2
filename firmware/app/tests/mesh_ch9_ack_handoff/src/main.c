#include "app_mesh_ch9_ack.h"
#include "mesh.h"
#include "protocol.h"

#include <zephyr/ztest.h>

struct fake_retry_queue {
    struct mesh_outbound entries[4];
    uint8_t count;
    uint8_t drop_notes;
};

static struct proto_packet gateway_ack_packet(void)
{
    return (struct proto_packet) {
        .msg_type = MSG_GATEWAY_ACK,
        .flags = FLAG_GATEWAY_ACK,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x1111222233334444ull,
        .session_id = 7001u,
        .seq = 91u,
        .ttl = MESH_DEFAULT_TTL,
    };
}

static void append_ack_lists(uint8_t *payload,
                             size_t payload_cap,
                             size_t *payload_len,
                             const uint32_t *sessions,
                             const uint16_t *seqs,
                             uint8_t count)
{
    uint8_t session_list[16];
    uint8_t seq_list[8];

    zassert_true(count <= 4u);
    for (uint8_t i = 0u; i < count; i++) {
        proto_put_u32_le(&session_list[i * sizeof(uint32_t)], sessions[i]);
        proto_put_u16_le(&seq_list[i * sizeof(uint16_t)], seqs[i]);
    }
    zassert_ok(tlv_append_bytes(payload,
                                payload_cap,
                                payload_len,
                                TLV_MESH_ACK_SESSION_LIST,
                                session_list,
                                (uint8_t)(count * sizeof(uint32_t))));
    zassert_ok(tlv_append_bytes(payload,
                                payload_cap,
                                payload_len,
                                TLV_MESH_ACK_SEQ_LIST,
                                seq_list,
                                (uint8_t)(count * sizeof(uint16_t))));
}

static void append_ack_packet_ids(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *payload_len,
                                  const uint32_t *packet_ids,
                                  uint8_t count)
{
    uint8_t packet_id_list[16];

    zassert_true(count <= 4u);
    for (uint8_t i = 0u; i < count; i++) {
        proto_put_u32_le(&packet_id_list[i * sizeof(uint32_t)], packet_ids[i]);
    }
    zassert_ok(tlv_append_bytes(payload,
                                payload_cap,
                                payload_len,
                                TLV_MESH_ACK_PACKET_ID_LIST,
                                packet_id_list,
                                (uint8_t)(count * sizeof(uint32_t))));
}

static int fake_retry_put(const struct mesh_outbound *outbound, void *ctx)
{
    struct fake_retry_queue *queue = ctx;

    if (queue->count >= ARRAY_SIZE(queue->entries)) {
        return -ENOMEM;
    }
    queue->entries[queue->count++] = *outbound;
    return 0;
}

static int fake_retry_get(struct mesh_outbound *outbound, void *ctx)
{
    struct fake_retry_queue *queue = ctx;

    if (queue->count == 0u) {
        return -ENOMSG;
    }
    *outbound = queue->entries[0];
    for (uint8_t i = 1u; i < queue->count; i++) {
        queue->entries[i - 1u] = queue->entries[i];
    }
    queue->count--;
    return 0;
}

static uint8_t fake_retry_used(void *ctx)
{
    const struct fake_retry_queue *queue = ctx;

    return queue->count;
}

static void fake_retry_note_drop(void *ctx)
{
    struct fake_retry_queue *queue = ctx;

    queue->drop_notes++;
}

static struct mesh_outbound fake_outbound(uint16_t seq, uint32_t queued_at_ms)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = MSG_COMMAND_RESULT,
            .seq = seq,
            .session_id = 1000u + seq,
            .src_id = 0x1111222233334444ull,
            .dst_id = 0x9999888877776666ull,
            .ttl = MESH_DEFAULT_TTL,
        },
        .queued_at_ms = queued_at_ms,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
    };
}

static struct mesh_outbound fake_route_test_outbound(uint16_t seq,
                                                     uint32_t session_id,
                                                     uint32_t packet_id,
                                                     uint32_t queued_at_ms)
{
    struct mesh_outbound outbound = {
        .packet = {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            .seq = seq,
            .session_id = session_id,
            .src_id = 0x1111222233334444ull,
            .dst_id = 0x9999888877776666ull,
            .ttl = MESH_DEFAULT_TTL,
        },
        .queued_at_ms = queued_at_ms,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = 0x9999888877776666ull,
    };
    size_t payload_len = 0u;

    zassert_ok(tlv_append_u32(outbound.payload,
                              sizeof(outbound.payload),
                              &payload_len,
                              TLV_MESH_TEST_PACKET_ID,
                              packet_id));
    zassert_ok(tlv_append_u32(outbound.payload,
                              sizeof(outbound.payload),
                              &payload_len,
                              TLV_EVENT_SEQ,
                              packet_id));
    zassert_ok(tlv_append_u8(outbound.payload,
                             sizeof(outbound.payload),
                             &payload_len,
                             TLV_MESH_CHANNEL,
                             UWB_CHANNEL_MESH_PAYLOAD));
    outbound.payload_len = (uint16_t)payload_len;
    outbound.packet.payload_len = (uint16_t)payload_len;
    return outbound;
}

static struct mesh_outbound fake_collection_result_outbound(uint16_t seq,
                                                            uint32_t session_id,
                                                            uint64_t node_id,
                                                            uint32_t boot_counter,
                                                            uint32_t retry_spread_ms,
                                                            uint8_t retry_round,
                                                            uint32_t queued_at_ms)
{
    const struct command_result_id result_id = {
        .gateway_id = 0x9999888877776666ull,
        .gateway_epoch = 7u,
        .command_seq = 0x51000000u + session_id,
        .node_id = node_id,
        .node_boot_counter = boot_counter,
        .result_seq = seq,
    };
    struct mesh_outbound outbound = {
        .packet = {
            .msg_type = MSG_COMMAND_RESULT,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .seq = seq,
            .session_id = session_id,
            .src_id = node_id,
            .dst_id = result_id.gateway_id,
            .ttl = MESH_DEFAULT_TTL,
        },
        .queued_at_ms = queued_at_ms,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = result_id.gateway_id,
    };
    size_t payload_len = 0u;

    zassert_ok(command_result_id_append_tlvs(outbound.payload,
                                             sizeof(outbound.payload),
                                             &payload_len,
                                             &result_id));
    zassert_ok(tlv_append_u32(outbound.payload,
                              sizeof(outbound.payload),
                              &payload_len,
                              TLV_COLLECTION_EPOCH_ID,
                              0x00C011ECu));
    zassert_ok(tlv_append_u8(outbound.payload,
                             sizeof(outbound.payload),
                             &payload_len,
                             TLV_RETRY_ROUND,
                             retry_round));
    zassert_ok(tlv_append_u32(outbound.payload,
                              sizeof(outbound.payload),
                              &payload_len,
                              TLV_NEXT_RETRY_SPREAD_MS,
                              retry_spread_ms));
    outbound.payload_len = (uint16_t)payload_len;
    outbound.packet.payload_len = (uint16_t)payload_len;
    return outbound;
}

static bool payload_find_u32(const uint8_t *payload,
                             size_t payload_len,
                             uint8_t type,
                             uint32_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &encoded, &encoded_len);
    if (ret != PROTO_OK || encoded_len != sizeof(uint32_t)) {
        return false;
    }
    if (value != NULL) {
        *value = proto_get_u32_le(encoded);
    }
    return true;
}

ZTEST(mesh_ch9_ack_handoff, test_partial_ack_marks_only_matching_entry)
{
    const uint32_t sessions[] = { 1001u };
    const uint16_t seqs[] = { 11u };
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entries[] = {
        {
            .session_id = 1001u,
            .seq = 11u,
        },
        {
            .session_id = 1002u,
            .seq = 12u,
        },
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;

    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     sessions,
                     seqs,
                     ARRAY_SIZE(seqs));

    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         entries,
                                         ARRAY_SIZE(entries),
                                         &result));

    zassert_true(result.any_match);
    zassert_false(result.all_acked);
    zassert_equal(result.acked_now, 1u);
    zassert_equal(result.unacked_count, 1u);
    zassert_true(entries[0].acked);
    zassert_false(entries[1].acked);
}

ZTEST(mesh_ch9_ack_handoff, test_complete_ack_uses_session_list_to_disambiguate)
{
    const uint32_t sessions[] = { 1002u };
    const uint16_t seqs[] = { 11u };
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entries[] = {
        {
            .session_id = 1001u,
            .seq = 11u,
        },
        {
            .session_id = 1002u,
            .seq = 11u,
        },
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;

    entries[0].acked = true;
    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     sessions,
                     seqs,
                     ARRAY_SIZE(seqs));

    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         entries,
                                         ARRAY_SIZE(entries),
                                         &result));

    zassert_true(result.any_match);
    zassert_true(result.all_acked);
    zassert_equal(result.acked_now, 1u);
    zassert_equal(result.unacked_count, 0u);
    zassert_true(entries[0].acked);
    zassert_true(entries[1].acked);
}

ZTEST(mesh_ch9_ack_handoff, test_legacy_requested_seq_requires_ack_session)
{
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entry = {
        .session_id = ack.session_id,
        .seq = 33u,
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[16];
    size_t payload_len = 0u;

    zassert_ok(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_REQUESTED_MSG_SEQ,
                              entry.seq));
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         &entry,
                                         1u,
                                         &result));
    zassert_true(result.all_acked);
    zassert_true(entry.acked);

    entry.acked = false;
    entry.session_id = ack.session_id + 1u;
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         &entry,
                                         1u,
                                         &result));
    zassert_false(result.any_match);
    zassert_false(entry.acked);
}

ZTEST(mesh_ch9_ack_handoff, test_malformed_session_list_is_rejected)
{
    const uint8_t bad_session_list[] = { 0x01u, 0x02u };
    const uint16_t seq = 22u;
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entry = {
        .session_id = 1001u,
        .seq = seq,
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;
    uint8_t seq_list[sizeof(uint16_t)];

    proto_put_u16_le(seq_list, seq);
    zassert_ok(tlv_append_bytes(payload,
                                sizeof(payload),
                                &payload_len,
                                TLV_MESH_ACK_SESSION_LIST,
                                bad_session_list,
                                sizeof(bad_session_list)));
    zassert_ok(tlv_append_bytes(payload,
                                sizeof(payload),
                                &payload_len,
                                TLV_MESH_ACK_SEQ_LIST,
                                seq_list,
                                sizeof(seq_list)));

    zassert_equal(app_mesh_ch9_tx_ack_apply(&ack,
                                            payload,
                                            payload_len,
                                            &entry,
                                            1u,
                                            &result),
                  PROTO_ERR_MALFORMED);
    zassert_false(entry.acked);
}

ZTEST(mesh_ch9_ack_handoff, test_partial_ack_requeues_only_unacked_before_existing_queue)
{
    struct fake_retry_queue queue = {
        .entries = {
            fake_outbound(80u, 10u),
        },
        .count = 1u,
    };
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = fake_retry_put,
        .get = fake_retry_get,
        .queue_used = fake_retry_used,
        .note_drop = fake_retry_note_drop,
        .ctx = &queue,
    };
    struct app_mesh_ch9_tx_retry_entry entries[] = {
        {
            .outbound = fake_outbound(11u, 20u),
            .acked = true,
        },
        {
            .outbound = fake_outbound(12u, 30u),
        },
    };
    struct app_mesh_ch9_tx_retry_result result;

    zassert_ok(app_mesh_ch9_tx_requeue_unacked(entries,
                                               ARRAY_SIZE(entries),
                                               1234u,
                                               &ops,
                                               &result));

    zassert_equal(result.requeued, 1u);
    zassert_equal(result.dropped, 0u);
    zassert_equal(result.queued_before, 1u);
    zassert_equal(result.queued_after, 2u);
    zassert_equal(queue.count, 2u);
    zassert_equal(queue.drop_notes, 0u);
    zassert_equal(queue.entries[0].packet.seq, 12u);
    zassert_equal(queue.entries[0].queued_at_ms, 1234u);
    zassert_equal(queue.entries[1].packet.seq, 80u);
    zassert_equal(queue.entries[1].queued_at_ms, 10u);
}

ZTEST(mesh_ch9_ack_handoff, test_partial_ack_requeue_reports_drop_when_queue_full)
{
    struct fake_retry_queue queue = {
        .entries = {
            fake_outbound(80u, 10u),
            fake_outbound(81u, 11u),
            fake_outbound(82u, 12u),
            fake_outbound(83u, 13u),
        },
        .count = 4u,
    };
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = fake_retry_put,
        .get = fake_retry_get,
        .queue_used = fake_retry_used,
        .note_drop = fake_retry_note_drop,
        .ctx = &queue,
    };
    struct app_mesh_ch9_tx_retry_entry entry = {
        .outbound = fake_outbound(12u, 30u),
    };
    struct app_mesh_ch9_tx_retry_result result;

    zassert_ok(app_mesh_ch9_tx_requeue_unacked(&entry,
                                               1u,
                                               1234u,
                                               &ops,
                                               &result));

    zassert_equal(result.requeued, 0u);
    zassert_equal(result.dropped, 1u);
    zassert_equal(result.queued_before, 4u);
    zassert_equal(result.queued_after, 4u);
    zassert_equal(queue.drop_notes, 1u);
}

ZTEST(mesh_ch9_ack_handoff,
      test_route_test_partial_ack_requeues_unacked_ahead_of_later_work)
{
    const uint32_t ack_sessions[] = { 2101u, 2103u };
    const uint16_t ack_seqs[] = { 41u, 43u };
    const uint32_t ack_packet_ids[] = { 501u, 503u };
    struct proto_packet ack = gateway_ack_packet();
    struct fake_retry_queue queue = {
        .entries = {
            fake_route_test_outbound(99u, 2199u, 599u, 10u),
        },
        .count = 1u,
    };
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = fake_retry_put,
        .get = fake_retry_get,
        .queue_used = fake_retry_used,
        .note_drop = fake_retry_note_drop,
        .ctx = &queue,
    };
    struct app_mesh_ch9_tx_retry_entry retry_entries[] = {
        {
            .outbound = fake_route_test_outbound(41u, 2101u, 501u, 20u),
        },
        {
            .outbound = fake_route_test_outbound(42u, 2102u, 502u, 30u),
        },
        {
            .outbound = fake_route_test_outbound(43u, 2103u, 503u, 40u),
        },
    };
    struct app_mesh_ch9_tx_ack_entry ack_entries[] = {
        {
            .session_id = retry_entries[0].outbound.packet.session_id,
            .seq = retry_entries[0].outbound.packet.seq,
        },
        {
            .session_id = retry_entries[1].outbound.packet.session_id,
            .seq = retry_entries[1].outbound.packet.seq,
        },
        {
            .session_id = retry_entries[2].outbound.packet.session_id,
            .seq = retry_entries[2].outbound.packet.seq,
        },
    };
    struct app_mesh_ch9_tx_ack_result ack_result;
    struct app_mesh_ch9_tx_retry_result retry_result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    uint32_t packet_id = 0u;

    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     ack_sessions,
                     ack_seqs,
                     ARRAY_SIZE(ack_seqs));
    append_ack_packet_ids(payload,
                          sizeof(payload),
                          &payload_len,
                          ack_packet_ids,
                          ARRAY_SIZE(ack_packet_ids));

    zassert_true(app_mesh_ch9_tx_should_track_ack(
        &retry_entries[0].outbound.packet, true));
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         ack_entries,
                                         ARRAY_SIZE(ack_entries),
                                         &ack_result));
    zassert_true(ack_result.any_match);
    zassert_false(ack_result.all_acked);
    zassert_equal(ack_result.acked_now, 2u);
    zassert_equal(ack_result.unacked_count, 1u);

    for (uint8_t i = 0u; i < ARRAY_SIZE(retry_entries); i++) {
        retry_entries[i].acked = ack_entries[i].acked;
    }
    zassert_true(retry_entries[0].acked);
    zassert_false(retry_entries[1].acked);
    zassert_true(retry_entries[2].acked);

    zassert_ok(app_mesh_ch9_tx_requeue_unacked(retry_entries,
                                               ARRAY_SIZE(retry_entries),
                                               6000u,
                                               &ops,
                                               &retry_result));

    zassert_equal(retry_result.requeued, 1u);
    zassert_equal(retry_result.dropped, 0u);
    zassert_equal(retry_result.queued_before, 1u);
    zassert_equal(retry_result.queued_after, 2u);
    zassert_equal(queue.drop_notes, 0u);
    zassert_equal(queue.count, 2u);

    zassert_equal(queue.entries[0].packet.msg_type, MSG_MESH_DATA);
    zassert_equal(queue.entries[0].packet.flags,
                  FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC);
    zassert_equal(queue.entries[0].packet.seq, 42u);
    zassert_equal(queue.entries[0].packet.session_id, 2102u);
    zassert_equal(queue.entries[0].queued_at_ms, 6000u);
    zassert_equal(queue.entries[0].radio_channel, UWB_CHANNEL_MESH_PAYLOAD);
    zassert_true(payload_find_u32(queue.entries[0].payload,
                                  queue.entries[0].payload_len,
                                  TLV_MESH_TEST_PACKET_ID,
                                  &packet_id));
    zassert_equal(packet_id, 502u);

    zassert_equal(queue.entries[1].packet.seq, 99u);
    zassert_equal(queue.entries[1].queued_at_ms, 10u);
    zassert_true(payload_find_u32(queue.entries[1].payload,
                                  queue.entries[1].payload_len,
                                  TLV_MESH_TEST_PACKET_ID,
                                  &packet_id));
    zassert_equal(packet_id, 599u);
}

ZTEST(mesh_ch9_ack_handoff,
      test_collection_source_retry_survives_nonmatching_and_partial_ack)
{
    const uint64_t source_node_id = 0x1111222233334444ull;
    const uint32_t source_boot_counter = 0x00B00751u;
    const uint32_t retry_spread_ms = 45000u;
    const uint8_t retry_round = 2u;
    const uint32_t miss_sessions[] = { 9999u };
    const uint16_t miss_seqs[] = { 99u };
    const uint32_t ack_sessions[] = { 3202u };
    const uint16_t ack_seqs[] = { 52u };
    struct proto_packet ack = gateway_ack_packet();
    struct fake_retry_queue queue = {
        .entries = {
            fake_route_test_outbound(90u, 3290u, 590u, 10u),
        },
        .count = 1u,
    };
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = fake_retry_put,
        .get = fake_retry_get,
        .queue_used = fake_retry_used,
        .note_drop = fake_retry_note_drop,
        .ctx = &queue,
    };
    struct app_mesh_ch9_tx_retry_entry retry_entries[] = {
        {
            .outbound = fake_collection_result_outbound(51u,
                                                        3201u,
                                                        source_node_id,
                                                        source_boot_counter,
                                                        retry_spread_ms,
                                                        retry_round,
                                                        20u),
        },
        {
            .outbound = fake_route_test_outbound(52u, 3202u, 592u, 30u),
        },
    };
    struct app_mesh_ch9_tx_ack_entry ack_entries[] = {
        {
            .session_id = retry_entries[0].outbound.packet.session_id,
            .seq = retry_entries[0].outbound.packet.seq,
        },
        {
            .session_id = retry_entries[1].outbound.packet.session_id,
            .seq = retry_entries[1].outbound.packet.seq,
        },
    };
    struct app_mesh_ch9_tx_ack_result ack_result;
    struct app_mesh_ch9_tx_retry_result retry_result;
    struct command_result_id decoded_id;
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;
    uint32_t value = 0u;
    uint8_t payload[96];
    size_t payload_len = 0u;

    zassert_true(app_mesh_ch9_tx_should_track_ack(
        &retry_entries[0].outbound.packet, false));
    zassert_false(app_mesh_ch9_tx_should_track_ack(
        &retry_entries[0].outbound.packet, true));

    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     miss_sessions,
                     miss_seqs,
                     ARRAY_SIZE(miss_seqs));
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         ack_entries,
                                         ARRAY_SIZE(ack_entries),
                                         &ack_result));
    zassert_false(ack_result.any_match);
    zassert_false(ack_entries[0].acked);
    zassert_false(ack_entries[1].acked);

    payload_len = 0u;
    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     ack_sessions,
                     ack_seqs,
                     ARRAY_SIZE(ack_seqs));
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         ack_entries,
                                         ARRAY_SIZE(ack_entries),
                                         &ack_result));
    zassert_true(ack_result.any_match);
    zassert_false(ack_result.all_acked);
    zassert_equal(ack_result.acked_now, 1u);
    zassert_equal(ack_result.unacked_count, 1u);

    for (uint8_t i = 0u; i < ARRAY_SIZE(retry_entries); i++) {
        retry_entries[i].acked = ack_entries[i].acked;
    }
    zassert_false(retry_entries[0].acked);
    zassert_true(retry_entries[1].acked);

    zassert_ok(app_mesh_ch9_tx_requeue_unacked(retry_entries,
                                               ARRAY_SIZE(retry_entries),
                                               7000u,
                                               &ops,
                                               &retry_result));
    zassert_equal(retry_result.requeued, 1u);
    zassert_equal(retry_result.dropped, 0u);
    zassert_equal(retry_result.queued_before, 1u);
    zassert_equal(retry_result.queued_after, 2u);
    zassert_equal(queue.drop_notes, 0u);
    zassert_equal(queue.count, 2u);

    zassert_equal(queue.entries[0].packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(queue.entries[0].packet.seq, 51u);
    zassert_equal(queue.entries[0].packet.session_id, 3201u);
    zassert_equal(queue.entries[0].packet.src_id, source_node_id);
    zassert_equal(queue.entries[0].queued_at_ms, 7000u);
    zassert_equal(queue.entries[0].radio_channel, UWB_CHANNEL_MESH_PAYLOAD);
    zassert_equal(queue.entries[0].next_hop_id, 0x9999888877776666ull);
    zassert_ok(command_result_id_from_tlvs(queue.entries[0].payload,
                                           queue.entries[0].payload_len,
                                           &decoded_id));
    zassert_equal(decoded_id.gateway_id, 0x9999888877776666ull);
    zassert_equal(decoded_id.command_seq, 0x51000000u + 3201u);
    zassert_equal(decoded_id.node_id, source_node_id);
    zassert_equal(decoded_id.node_boot_counter, source_boot_counter);
    zassert_equal(decoded_id.result_seq, 51u);
    zassert_true(payload_find_u32(queue.entries[0].payload,
                                  queue.entries[0].payload_len,
                                  TLV_COLLECTION_EPOCH_ID,
                                  &value));
    zassert_equal(value, 0x00C011ECu);
    zassert_equal(tlv_find(queue.entries[0].payload,
                           queue.entries[0].payload_len,
                           TLV_RETRY_ROUND,
                           &encoded,
                           &encoded_len),
                  PROTO_OK);
    zassert_equal(encoded_len, sizeof(uint8_t));
    zassert_equal(encoded[0], retry_round);
    zassert_true(payload_find_u32(queue.entries[0].payload,
                                  queue.entries[0].payload_len,
                                  TLV_NEXT_RETRY_SPREAD_MS,
                                  &value));
    zassert_equal(value, retry_spread_ms);

    zassert_equal(queue.entries[1].packet.seq, 90u);
    zassert_equal(queue.entries[1].queued_at_ms, 10u);
}

ZTEST(mesh_ch9_ack_handoff,
      test_collection_result_stays_relay_owned_instead_of_ack_handoff)
{
    struct proto_packet collection_result = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
    };
    struct proto_packet click_report = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
    };

    zassert_false(app_mesh_ch9_tx_should_track_ack(&collection_result, true));
    zassert_true(app_mesh_ch9_tx_should_track_ack(&collection_result, false));
    zassert_true(app_mesh_ch9_tx_should_track_ack(&click_report, true));
    zassert_false(app_mesh_ch9_tx_should_track_ack(NULL, true));
}

ZTEST_SUITE(mesh_ch9_ack_handoff, NULL, NULL, NULL, NULL, NULL);
