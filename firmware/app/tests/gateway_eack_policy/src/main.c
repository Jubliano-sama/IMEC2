#include "app_gateway_eack_policy.h"
#include "mesh.h"
#include "protocol.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/ztest.h>

struct test_ctx {
    int plan_ret;
    int prepare_ret;
    int send_ch9_ret;
    int send_c5_ret;
    int prepare_ret_by_call[4];
    int send_ch9_ret_by_call[4];
    uint64_t planned_next_hop;
    uint64_t planned_next_hops[4];
    uint64_t prepared_next_hops[4];
    uint64_t sent_ch9_next_hops[4];
    uint64_t noted_ch9_peer;
    uint32_t noted_ch9_start_ms;
    uint32_t prepared_earliest_tx_ms;
    uint8_t plan_calls;
    uint8_t prepare_calls;
    uint8_t send_ch9_calls;
    uint8_t send_c5_calls;
    uint8_t note_tx_calls;
    uint8_t note_ch9_calls;
    bool mutate_failed_prepare;
};

static struct mesh_outbound make_eack(void)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = MSG_GATEWAY_COLLECTION_EACK,
            .src_id = 0x9999888877776666ull,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 1001u,
            .seq = 3u,
            .ttl = MESH_DEFAULT_TTL,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
}

static int test_plan_channel9(uint64_t next_hop_id,
                              struct mesh_event_plan *plan,
                              void *ctx)
{
    struct test_ctx *test = ctx;
    const uint8_t call = test->plan_calls;

    test->plan_calls++;
    test->planned_next_hop = next_hop_id;
    if (call < ARRAY_SIZE(test->planned_next_hops)) {
        test->planned_next_hops[call] = next_hop_id;
    }
    if (plan != NULL) {
        plan->start_ms = 1234u;
        plan->end_ms = 1334u;
        plan->window_ms = 100u;
    }
    return test->plan_ret;
}

static int test_prepare_channel9(struct mesh_outbound *out,
                                 const struct mesh_event_plan *plan,
                                 void *ctx)
{
    struct test_ctx *test = ctx;
    const uint8_t call = test->prepare_calls;
    int ret = test->prepare_ret;

    zassert_not_null(out);
    zassert_not_null(plan);
    zassert_equal(out->radio_channel, MESH_EVENT_CHANNEL);
    test->prepare_calls++;
    if (call < ARRAY_SIZE(test->prepare_ret_by_call) &&
        test->prepare_ret_by_call[call] != 0) {
        ret = test->prepare_ret_by_call[call];
    }
    if (call < ARRAY_SIZE(test->prepared_next_hops)) {
        test->prepared_next_hops[call] = out->next_hop_id;
    }
    if (ret == 0) {
        out->earliest_tx_ms = plan->start_ms + 11u;
        test->prepared_earliest_tx_ms = out->earliest_tx_ms;
    } else if (test->mutate_failed_prepare) {
        out->packet.session_id = 0xdeadbeefu;
        out->packet.seq = 0xbeefu;
        out->payload[0] = 0xeeu;
        out->payload[1] = 0xddu;
        out->payload_len = 2u;
        out->queued_at_ms = 0x12345678u;
        out->earliest_tx_ms = 0x87654321u;
    }
    return ret;
}

static int test_send_channel9(const struct mesh_outbound *out, void *ctx)
{
    struct test_ctx *test = ctx;
    const uint8_t call = test->send_ch9_calls;
    int ret = test->send_ch9_ret;

    zassert_not_null(out);
    zassert_equal(out->radio_channel, MESH_EVENT_CHANNEL);
    zassert_equal(out->earliest_tx_ms, test->prepared_earliest_tx_ms);
    test->send_ch9_calls++;
    if (call < ARRAY_SIZE(test->send_ch9_ret_by_call) &&
        test->send_ch9_ret_by_call[call] != 0) {
        ret = test->send_ch9_ret_by_call[call];
    }
    if (call < ARRAY_SIZE(test->sent_ch9_next_hops)) {
        test->sent_ch9_next_hops[call] = out->next_hop_id;
    }
    return ret;
}

static int test_send_c5(const struct mesh_outbound *out, void *ctx)
{
    struct test_ctx *test = ctx;

    zassert_not_null(out);
    zassert_equal(out->radio_channel, UWB_CHANNEL_WAKE_CONTACT);
    zassert_equal(out->next_hop_id, MESH_BROADCAST_ID);
    test->send_c5_calls++;
    return test->send_c5_ret;
}

static void test_note_tx(const struct mesh_outbound *out, void *ctx)
{
    struct test_ctx *test = ctx;

    zassert_not_null(out);
    test->note_tx_calls++;
}

static void test_note_ch9(uint64_t next_hop_id,
                          uint32_t event_start_ms,
                          void *ctx)
{
    struct test_ctx *test = ctx;

    test->note_ch9_calls++;
    test->noted_ch9_peer = next_hop_id;
    test->noted_ch9_start_ms = event_start_ms;
}

static struct app_gateway_eack_policy_ops make_ops(struct test_ctx *ctx)
{
    return (struct app_gateway_eack_policy_ops) {
        .plan_channel9 = test_plan_channel9,
        .prepare_channel9 = test_prepare_channel9,
        .send_channel9 = test_send_channel9,
        .send_c5_flood = test_send_c5,
        .note_tx_sent = test_note_tx,
        .note_channel9_tx = test_note_ch9,
        .ctx = ctx,
    };
}

ZTEST(gateway_eack_policy, test_prefers_channel9_for_valid_return_target)
{
    const uint64_t return_peer = 0x1111222233334444ull;
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {0};
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send(&eack, return_peer, &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_CHANNEL9);
    zassert_equal(result.channel9_plan_ret, 0);
    zassert_equal(result.channel9_prepare_ret, 0);
    zassert_equal(result.channel9_send_ret, 0);
    zassert_equal(result.channel9_next_hop_id, return_peer);
    zassert_equal(result.channel9_candidate_count, 1u);
    zassert_equal(result.channel9_attempt_count, 1u);
    zassert_equal(ctx.plan_calls, 1u);
    zassert_equal(ctx.prepare_calls, 1u);
    zassert_equal(ctx.send_ch9_calls, 1u);
    zassert_equal(ctx.send_c5_calls, 0u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 1u);
    zassert_equal(ctx.planned_next_hop, return_peer);
    zassert_equal(ctx.noted_ch9_peer, return_peer);
    zassert_equal(ctx.noted_ch9_start_ms, 1234u);
    zassert_equal(eack.next_hop_id, return_peer);
    zassert_equal(eack.radio_channel, MESH_EVENT_CHANNEL);
    zassert_equal(eack.earliest_tx_ms, 1245u);
}

ZTEST(gateway_eack_policy, test_falls_back_to_c5_without_return_target)
{
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {0};
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send(&eack, 0u, &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_C5_FLOOD);
    zassert_equal(result.channel9_next_hop_id, 0u);
    zassert_equal(result.channel9_candidate_count, 0u);
    zassert_equal(result.channel9_attempt_count, 0u);
    zassert_equal(ctx.plan_calls, 0u);
    zassert_equal(ctx.prepare_calls, 0u);
    zassert_equal(ctx.send_ch9_calls, 0u);
    zassert_equal(ctx.send_c5_calls, 1u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 0u);
    zassert_equal(eack.next_hop_id, MESH_BROADCAST_ID);
    zassert_equal(eack.radio_channel, UWB_CHANNEL_WAKE_CONTACT);
    zassert_equal(eack.earliest_tx_ms, 0u);
}

ZTEST(gateway_eack_policy, test_falls_back_to_c5_when_channel9_plan_fails)
{
    const uint64_t return_peer = 0x1111222233334444ull;
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {
        .plan_ret = -EAGAIN,
    };
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send(&eack, return_peer, &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_C5_FLOOD);
    zassert_equal(result.channel9_plan_ret, -EAGAIN);
    zassert_equal(result.channel9_next_hop_id, 0u);
    zassert_equal(result.channel9_candidate_count, 1u);
    zassert_equal(result.channel9_attempt_count, 1u);
    zassert_equal(ctx.plan_calls, 1u);
    zassert_equal(ctx.prepare_calls, 0u);
    zassert_equal(ctx.send_ch9_calls, 0u);
    zassert_equal(ctx.send_c5_calls, 1u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 0u);
    zassert_equal(eack.next_hop_id, MESH_BROADCAST_ID);
    zassert_equal(eack.radio_channel, UWB_CHANNEL_WAKE_CONTACT);
    zassert_equal(eack.earliest_tx_ms, 0u);
}

ZTEST(gateway_eack_policy, test_falls_back_to_c5_when_channel9_prepare_fails)
{
    const uint64_t return_peer = 0x1111222233334444ull;
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {
        .prepare_ret = -EMSGSIZE,
    };
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send(&eack, return_peer, &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_C5_FLOOD);
    zassert_equal(result.channel9_plan_ret, 0);
    zassert_equal(result.channel9_prepare_ret, -EMSGSIZE);
    zassert_equal(result.channel9_next_hop_id, 0u);
    zassert_equal(result.channel9_candidate_count, 1u);
    zassert_equal(result.channel9_attempt_count, 1u);
    zassert_equal(ctx.plan_calls, 1u);
    zassert_equal(ctx.prepare_calls, 1u);
    zassert_equal(ctx.send_ch9_calls, 0u);
    zassert_equal(ctx.send_c5_calls, 1u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 0u);
    zassert_equal(eack.next_hop_id, MESH_BROADCAST_ID);
    zassert_equal(eack.radio_channel, UWB_CHANNEL_WAKE_CONTACT);
    zassert_equal(eack.earliest_tx_ms, 0u);
}

ZTEST(gateway_eack_policy,
      test_c5_fallback_restores_collection_eack_state_after_prepare_failure)
{
    const uint64_t return_peer = 0x1111222233334444ull;
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {
        .prepare_ret = -EBUSY,
        .mutate_failed_prepare = true,
    };
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    eack.packet.session_id = 0x01020304u;
    eack.packet.seq = 0x1122u;
    eack.packet.ttl = 3u;
    eack.payload[0] = 0xa1u;
    eack.payload[1] = 0xb2u;
    eack.payload[2] = 0xc3u;
    eack.payload[3] = 0xd4u;
    eack.payload_len = 4u;
    eack.queued_at_ms = 0x55667788u;

    zassert_ok(app_gateway_eack_send(&eack, return_peer, &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_C5_FLOOD);
    zassert_equal(result.channel9_prepare_ret, -EBUSY);
    zassert_equal(result.channel9_candidate_count, 1u);
    zassert_equal(result.channel9_attempt_count, 1u);
    zassert_equal(ctx.prepare_calls, 1u);
    zassert_equal(ctx.send_ch9_calls, 0u);
    zassert_equal(ctx.send_c5_calls, 1u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 0u);
    zassert_equal(eack.packet.msg_type, MSG_GATEWAY_COLLECTION_EACK);
    zassert_equal(eack.packet.session_id, 0x01020304u);
    zassert_equal(eack.packet.seq, 0x1122u);
    zassert_equal(eack.packet.ttl, 3u);
    zassert_equal(eack.payload_len, 4u);
    zassert_equal(eack.payload[0], 0xa1u);
    zassert_equal(eack.payload[1], 0xb2u);
    zassert_equal(eack.payload[2], 0xc3u);
    zassert_equal(eack.payload[3], 0xd4u);
    zassert_equal(eack.queued_at_ms, 0x55667788u);
    zassert_equal(eack.next_hop_id, MESH_BROADCAST_ID);
    zassert_equal(eack.radio_channel, UWB_CHANNEL_WAKE_CONTACT);
    zassert_equal(eack.earliest_tx_ms, 0u);
}

ZTEST(gateway_eack_policy, test_tries_second_candidate_when_first_prepare_fails)
{
    const uint64_t first_peer = 0x1111222233334444ull;
    const uint64_t second_peer = 0x5555666677778888ull;
    const uint64_t candidates[] = { first_peer, second_peer };
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {
        .prepare_ret_by_call = { -EMSGSIZE, 0 },
    };
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send_to_candidates(&eack, candidates,
                                                   ARRAY_SIZE(candidates),
                                                   &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_CHANNEL9);
    zassert_equal(result.channel9_plan_ret, 0);
    zassert_equal(result.channel9_prepare_ret, 0);
    zassert_equal(result.channel9_send_ret, 0);
    zassert_equal(result.channel9_next_hop_id, second_peer);
    zassert_equal(result.channel9_candidate_count, 2u);
    zassert_equal(result.channel9_attempt_count, 2u);
    zassert_equal(ctx.plan_calls, 2u);
    zassert_equal(ctx.prepare_calls, 2u);
    zassert_equal(ctx.send_ch9_calls, 1u);
    zassert_equal(ctx.send_c5_calls, 0u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 1u);
    zassert_equal(ctx.planned_next_hops[0], first_peer);
    zassert_equal(ctx.planned_next_hops[1], second_peer);
    zassert_equal(ctx.prepared_next_hops[0], first_peer);
    zassert_equal(ctx.prepared_next_hops[1], second_peer);
    zassert_equal(ctx.sent_ch9_next_hops[0], second_peer);
    zassert_equal(ctx.noted_ch9_peer, second_peer);
    zassert_equal(eack.next_hop_id, second_peer);
    zassert_equal(eack.radio_channel, MESH_EVENT_CHANNEL);
    zassert_equal(eack.earliest_tx_ms, 1245u);
}

ZTEST(gateway_eack_policy, test_tries_second_candidate_when_first_send_fails)
{
    const uint64_t first_peer = 0x1111222233334444ull;
    const uint64_t second_peer = 0x5555666677778888ull;
    const uint64_t candidates[] = { first_peer, second_peer };
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {
        .send_ch9_ret_by_call = { -EIO, 0 },
    };
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send_to_candidates(&eack, candidates,
                                                   ARRAY_SIZE(candidates),
                                                   &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_CHANNEL9);
    zassert_equal(result.channel9_send_ret, 0);
    zassert_equal(result.channel9_next_hop_id, second_peer);
    zassert_equal(result.channel9_candidate_count, 2u);
    zassert_equal(result.channel9_attempt_count, 2u);
    zassert_equal(ctx.plan_calls, 2u);
    zassert_equal(ctx.prepare_calls, 2u);
    zassert_equal(ctx.send_ch9_calls, 2u);
    zassert_equal(ctx.send_c5_calls, 0u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 1u);
    zassert_equal(ctx.sent_ch9_next_hops[0], first_peer);
    zassert_equal(ctx.sent_ch9_next_hops[1], second_peer);
    zassert_equal(ctx.noted_ch9_peer, second_peer);
    zassert_equal(eack.next_hop_id, second_peer);
    zassert_equal(eack.radio_channel, MESH_EVENT_CHANNEL);
    zassert_equal(eack.earliest_tx_ms, 1245u);
}

ZTEST(gateway_eack_policy, test_skips_invalid_and_duplicate_candidates_before_c5_fallback)
{
    const uint64_t return_peer = 0x1111222233334444ull;
    const uint64_t candidates[] = {
        0u,
        MESH_BROADCAST_ID,
        return_peer,
        return_peer,
        0u,
    };
    struct mesh_outbound eack = make_eack();
    struct test_ctx ctx = {
        .plan_ret = -EAGAIN,
    };
    struct app_gateway_eack_policy_ops ops = make_ops(&ctx);
    struct app_gateway_eack_policy_result result;

    zassert_ok(app_gateway_eack_send_to_candidates(&eack, candidates,
                                                   ARRAY_SIZE(candidates),
                                                   &ops, &result));
    zassert_equal(result.mode, APP_GATEWAY_EACK_SEND_C5_FLOOD);
    zassert_equal(result.channel9_plan_ret, -EAGAIN);
    zassert_equal(result.channel9_next_hop_id, 0u);
    zassert_equal(result.channel9_candidate_count, 1u);
    zassert_equal(result.channel9_attempt_count, 1u);
    zassert_equal(ctx.plan_calls, 1u);
    zassert_equal(ctx.prepare_calls, 0u);
    zassert_equal(ctx.send_ch9_calls, 0u);
    zassert_equal(ctx.send_c5_calls, 1u);
    zassert_equal(ctx.note_tx_calls, 1u);
    zassert_equal(ctx.note_ch9_calls, 0u);
    zassert_equal(ctx.planned_next_hop, return_peer);
    zassert_equal(eack.next_hop_id, MESH_BROADCAST_ID);
    zassert_equal(eack.radio_channel, UWB_CHANNEL_WAKE_CONTACT);
    zassert_equal(eack.earliest_tx_ms, 0u);
}

ZTEST_SUITE(gateway_eack_policy, NULL, NULL, NULL, NULL, NULL);
