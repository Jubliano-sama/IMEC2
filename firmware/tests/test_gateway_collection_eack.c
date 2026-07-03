#include "app_gateway_collection_eack.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define GATEWAY_ID_TEST 0x9999888877776666ull
#define HOP_CURRENT 0x1111222233334444ull
#define HOP_STORED 0x2222333344445555ull
#define NODE_A 0xaaaabbbbcccc0001ull
#define NODE_B 0xaaaabbbbcccc0002ull
#define NODE_C 0xaaaabbbbcccc0003ull
#define COMMAND_SEQ_TEST 1001u
#define COLLECTION_EPOCH_TEST 3003u
#define GATEWAY_EPOCH_TEST 9u
#define MEMBERSHIP_EPOCH_TEST 4u

struct orchestration_test_ctx {
    struct mesh_outbound sent_channel9;
    struct mesh_outbound sent_c5;
    uint64_t noted_channel9_hop;
    uint32_t noted_channel9_start;
    uint32_t now_ms;
    int plan_count;
    int prepare_count;
    int send_channel9_count;
    int send_c5_count;
    int note_tx_count;
    int note_channel9_count;
    int now_count;
};

static int test_plan_channel9(uint64_t next_hop_id,
                              struct mesh_event_plan *plan,
                              void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    assert(plan != NULL);
    (void)next_hop_id;
    test->plan_count++;
    plan->start_ms = 1234u;
    plan->end_ms = 1334u;
    plan->window_ms = 100u;
    return 0;
}

static int test_prepare_channel9(struct mesh_outbound *out,
                                 const struct mesh_event_plan *plan,
                                 void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    assert(out != NULL);
    assert(plan != NULL);
    test->prepare_count++;
    out->earliest_tx_ms = plan->start_ms + 5u;
    return 0;
}

static int test_send_channel9(const struct mesh_outbound *out, void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    assert(out != NULL);
    test->sent_channel9 = *out;
    test->send_channel9_count++;
    return 0;
}

static int test_send_c5_flood(const struct mesh_outbound *out, void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    assert(out != NULL);
    test->sent_c5 = *out;
    test->send_c5_count++;
    return 0;
}

static void test_note_tx_sent(const struct mesh_outbound *out, void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    assert(out != NULL);
    test->note_tx_count++;
}

static void test_note_channel9_tx(uint64_t next_hop_id,
                                  uint32_t event_start_ms,
                                  void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    test->noted_channel9_hop = next_hop_id;
    test->noted_channel9_start = event_start_ms;
    test->note_channel9_count++;
}

static uint32_t test_now_ms(void *ctx)
{
    struct orchestration_test_ctx *test = ctx;

    assert(test != NULL);
    test->now_count++;
    return test->now_ms;
}

static struct app_gateway_eack_policy_ops test_ops(struct orchestration_test_ctx *ctx)
{
    const struct app_gateway_eack_policy_ops ops = {
        .plan_channel9 = test_plan_channel9,
        .prepare_channel9 = test_prepare_channel9,
        .send_channel9 = test_send_channel9,
        .send_c5_flood = test_send_c5_flood,
        .note_tx_sent = test_note_tx_sent,
        .note_channel9_tx = test_note_channel9_tx,
        .now_ms = test_now_ms,
        .ctx = ctx,
    };

    return ops;
}

static size_t build_collection_result_payload(uint64_t node_id,
                                              uint32_t result_seq,
                                              uint8_t *payload,
                                              size_t payload_cap)
{
    struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = GATEWAY_EPOCH_TEST,
        .command_seq = COMMAND_SEQ_TEST,
        .node_id = node_id,
        .node_boot_counter = 1u,
        .result_seq = result_seq,
    };
    size_t payload_len = 0u;

    assert(gateway_command_append_collection_result_identity(
               payload,
               payload_cap,
               &payload_len,
               &id,
               COLLECTION_EPOCH_TEST) == PROTO_OK);
    return payload_len;
}

static void record_collection_result(struct gateway_collection_state *collection,
                                     uint64_t node_id,
                                     uint64_t previous_hop_id,
                                     uint32_t result_seq)
{
    uint8_t payload[64];
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = node_id,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = COMMAND_SEQ_TEST,
        .seq = (uint16_t)result_seq,
        .ttl = 1u,
    };
    bool duplicate = true;
    size_t payload_len;

    payload_len = build_collection_result_payload(node_id,
                                                  result_seq,
                                                  payload,
                                                  sizeof(payload));
    packet.payload_len = (uint16_t)payload_len;
    assert(gateway_collection_record_result_from_hop(collection,
                                                     &packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
}

static void test_strict_roster_missing_list_uses_current_channel9_first(void)
{
    const uint64_t expected_node_ids[] = {NODE_A, NODE_B, NODE_C};
    struct gateway_collection_state collection;
    struct mesh_outbound outbound;
    struct gateway_collection_eack decoded;
    struct orchestration_test_ctx ctx = {
        .now_ms = 4567u,
    };
    const struct mesh_event_plan current_plan = {
        .action = MESH_EVENT_PLAN_START,
        .start_ms = 3456u,
        .end_ms = 3556u,
        .window_ms = 100u,
    };
    struct app_gateway_eack_policy_ops ops = test_ops(&ctx);
    struct app_gateway_collection_eack_input input = {
        .collection = &collection,
        .expected_node_ids = expected_node_ids,
        .expected_node_id_count = sizeof(expected_node_ids) / sizeof(expected_node_ids[0]),
        .previous_hop_id = HOP_CURRENT,
        .received_radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .current_channel9_plan = &current_plan,
        .self_id = GATEWAY_ID_TEST,
    };
    struct app_gateway_collection_eack_result result;
    bool listed = true;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    GATEWAY_EPOCH_TEST,
                                    COMMAND_SEQ_TEST,
                                    COLLECTION_EPOCH_TEST,
                                    MEMBERSHIP_EPOCH_TEST,
                                    (uint16_t)(sizeof(expected_node_ids) /
                                               sizeof(expected_node_ids[0])),
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    record_collection_result(&collection, NODE_A, HOP_STORED, 1u);

    assert(app_gateway_collection_eack_send(&outbound,
                                            &input,
                                            &ops,
                                            &result) == 0);
    assert(result.current_channel9_next_hop_id == HOP_CURRENT);
    assert(result.return_target_count == 1u);
    assert(result.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST);
    assert(result.missing_count == 2u);
    assert(result.command_seq == COMMAND_SEQ_TEST);
    assert(result.expected_count == 3u);
    assert(result.received_count == 1u);
    assert(result.collection_open);
    assert(result.policy.mode == APP_GATEWAY_EACK_SEND_CURRENT_CHANNEL9);
    assert(result.policy.channel9_next_hop_id == HOP_CURRENT);
    assert(result.policy.channel9_attempt_count == 1u);
    assert(result.policy.channel9_candidate_count == 0u);
    assert(ctx.plan_count == 0);
    assert(ctx.prepare_count == 1);
    assert(ctx.send_channel9_count == 1);
    assert(ctx.send_c5_count == 0);
    assert(ctx.note_tx_count == 1);
    assert(ctx.note_channel9_count == 1);
    assert(ctx.now_count == 0);
    assert(ctx.noted_channel9_hop == HOP_CURRENT);
    assert(ctx.noted_channel9_start == current_plan.start_ms);

    assert(ctx.sent_channel9.next_hop_id == HOP_CURRENT);
    assert(ctx.sent_channel9.radio_channel == UWB_CHANNEL_MESH_PAYLOAD);
    assert(ctx.sent_channel9.earliest_tx_ms == current_plan.start_ms + 5u);
    assert(ctx.sent_channel9.packet.msg_type == MSG_GATEWAY_COLLECTION_EACK);
    assert(ctx.sent_channel9.packet.src_id == GATEWAY_ID_TEST);
    assert(ctx.sent_channel9.packet.dst_id == MESH_BROADCAST_ID);
    assert(ctx.sent_channel9.packet.session_id == COMMAND_SEQ_TEST);
    assert(ctx.sent_channel9.packet.seq == 1u);
    assert(ctx.sent_channel9.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(ctx.sent_channel9.packet.payload_len == ctx.sent_channel9.payload_len);

    assert(gateway_collection_eack_from_tlvs(ctx.sent_channel9.payload,
                                             ctx.sent_channel9.payload_len,
                                             &decoded) == PROTO_OK);
    assert(decoded.gateway_id == GATEWAY_ID_TEST);
    assert(decoded.gateway_epoch == GATEWAY_EPOCH_TEST);
    assert(decoded.command_seq == COMMAND_SEQ_TEST);
    assert(decoded.collection_epoch_id == COLLECTION_EPOCH_TEST);
    assert(decoded.membership_epoch == MEMBERSHIP_EPOCH_TEST);
    assert(decoded.expected_count == 3u);
    assert(decoded.received_count == 1u);
    assert(decoded.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST);
    assert(decoded.retry_round == 0u);
    assert(decoded.next_retry_spread_ms == COLLECTION_RETRY_ROUND_0_MS);
    assert(decoded.collection_open);

    assert(gateway_collection_eack_contains_node_id(ctx.sent_channel9.payload,
                                                    ctx.sent_channel9.payload_len,
                                                    NODE_A,
                                                    &listed) == PROTO_OK);
    assert(!listed);
    assert(gateway_collection_eack_contains_node_id(ctx.sent_channel9.payload,
                                                    ctx.sent_channel9.payload_len,
                                                    NODE_B,
                                                    &listed) == PROTO_OK);
    assert(listed);
    assert(gateway_collection_eack_contains_node_id(ctx.sent_channel9.payload,
                                                    ctx.sent_channel9.payload_len,
                                                    NODE_C,
                                                    &listed) == PROTO_OK);
    assert(listed);
}

static void test_current_channel9_hop_filters_invalid_sources(void)
{
    const struct mesh_event_plan current_plan = {
        .action = MESH_EVENT_PLAN_START,
        .start_ms = 3456u,
        .end_ms = 3556u,
        .window_ms = 100u,
    };

    assert(app_gateway_collection_eack_current_channel9_return_hop(
               HOP_CURRENT,
               UWB_CHANNEL_WAKE_CONTACT,
               NULL,
               GATEWAY_ID_TEST) == 0u);
    assert(app_gateway_collection_eack_current_channel9_return_hop(
               0u,
               UWB_CHANNEL_MESH_PAYLOAD,
               NULL,
               GATEWAY_ID_TEST) == 0u);
    assert(app_gateway_collection_eack_current_channel9_return_hop(
               MESH_BROADCAST_ID,
               UWB_CHANNEL_MESH_PAYLOAD,
               NULL,
               GATEWAY_ID_TEST) == 0u);
    assert(app_gateway_collection_eack_current_channel9_return_hop(
               GATEWAY_ID_TEST,
               UWB_CHANNEL_MESH_PAYLOAD,
               NULL,
               GATEWAY_ID_TEST) == 0u);
    assert(app_gateway_collection_eack_current_channel9_return_hop(
               HOP_CURRENT,
               UWB_CHANNEL_MESH_PAYLOAD,
               NULL,
               GATEWAY_ID_TEST) == 0u);
    assert(app_gateway_collection_eack_current_channel9_return_hop(
               HOP_CURRENT,
               UWB_CHANNEL_MESH_PAYLOAD,
               &current_plan,
               GATEWAY_ID_TEST) == HOP_CURRENT);
}

int main(void)
{
    test_strict_roster_missing_list_uses_current_channel9_first();
    test_current_channel9_hop_filters_invalid_sources();
    return 0;
}
