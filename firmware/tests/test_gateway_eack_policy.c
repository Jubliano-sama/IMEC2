#include "app_gateway_eack_policy.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#define HOP_A 0x1111222233334444ull
#define HOP_B 0x2222333344445555ull

struct policy_test_ctx {
    uint64_t planned_hops[8];
    uint32_t planned_starts[8];
    int plan_returns[8];
    int prepare_returns[8];
    int send_channel9_returns[8];
    int c5_return;
    struct mesh_outbound sent_channel9;
    struct mesh_outbound sent_c5;
    uint64_t noted_channel9_hop;
    uint32_t noted_channel9_start;
    int plan_count;
    int prepare_count;
    int send_channel9_count;
    int send_c5_count;
    int note_tx_count;
    int note_channel9_count;
};

static int test_plan_channel9(uint64_t next_hop_id,
                              struct mesh_event_plan *plan,
                              void *ctx)
{
    struct policy_test_ctx *test = ctx;
    int index;

    assert(test != NULL);
    assert(plan != NULL);
    index = test->plan_count;
    assert(index >= 0 &&
           index < (int)(sizeof(test->planned_hops) / sizeof(test->planned_hops[0])));
    test->planned_hops[index] = next_hop_id;
    test->planned_starts[index] = 1000u + (uint32_t)index * 100u;
    plan->action = MESH_EVENT_PLAN_START;
    plan->start_ms = test->planned_starts[index];
    plan->end_ms = plan->start_ms + 20u;
    plan->window_ms = 20u;
    test->plan_count++;
    return test->plan_returns[index];
}

static int test_prepare_channel9(struct mesh_outbound *out,
                                 const struct mesh_event_plan *plan,
                                 void *ctx)
{
    struct policy_test_ctx *test = ctx;
    int index;

    assert(test != NULL);
    assert(out != NULL);
    assert(plan != NULL);
    index = test->prepare_count;
    assert(index >= 0 &&
           index < (int)(sizeof(test->prepare_returns) / sizeof(test->prepare_returns[0])));
    assert(out->radio_channel == MESH_EVENT_CHANNEL);
    out->earliest_tx_ms = plan->start_ms + 3u;
    test->prepare_count++;
    return test->prepare_returns[index];
}

static int test_send_channel9(const struct mesh_outbound *out, void *ctx)
{
    struct policy_test_ctx *test = ctx;
    int index;

    assert(test != NULL);
    assert(out != NULL);
    index = test->send_channel9_count;
    assert(index >= 0 &&
           index < (int)(sizeof(test->send_channel9_returns) /
                         sizeof(test->send_channel9_returns[0])));
    test->sent_channel9 = *out;
    test->send_channel9_count++;
    return test->send_channel9_returns[index];
}

static int test_send_c5_flood(const struct mesh_outbound *out, void *ctx)
{
    struct policy_test_ctx *test = ctx;

    assert(test != NULL);
    assert(out != NULL);
    test->sent_c5 = *out;
    test->send_c5_count++;
    return test->c5_return;
}

static void test_note_tx_sent(const struct mesh_outbound *out, void *ctx)
{
    struct policy_test_ctx *test = ctx;

    assert(test != NULL);
    assert(out != NULL);
    test->note_tx_count++;
}

static void test_note_channel9_tx(uint64_t next_hop_id,
                                  uint32_t event_start_ms,
                                  void *ctx)
{
    struct policy_test_ctx *test = ctx;

    assert(test != NULL);
    test->noted_channel9_hop = next_hop_id;
    test->noted_channel9_start = event_start_ms;
    test->note_channel9_count++;
}

static struct app_gateway_eack_policy_ops test_ops(struct policy_test_ctx *ctx)
{
    const struct app_gateway_eack_policy_ops ops = {
        .plan_channel9 = test_plan_channel9,
        .prepare_channel9 = test_prepare_channel9,
        .send_channel9 = test_send_channel9,
        .send_c5_flood = test_send_c5_flood,
        .note_tx_sent = test_note_tx_sent,
        .note_channel9_tx = test_note_channel9_tx,
        .ctx = ctx,
    };

    return ops;
}

static void init_eack(struct mesh_outbound *eack)
{
    memset(eack, 0, sizeof(*eack));
    eack->packet.msg_type = MSG_GATEWAY_COLLECTION_EACK;
    eack->packet.src_id = 0x9999888877776666ull;
    eack->packet.dst_id = MESH_BROADCAST_ID;
    eack->packet.session_id = 1001u;
    eack->packet.seq = 1u;
    eack->packet.ttl = MESH_DEFAULT_TTL;
}

static void test_channel9_uses_second_candidate_without_c5_flood(void)
{
    struct policy_test_ctx ctx = {0};
    struct mesh_outbound eack;
    struct app_gateway_eack_policy_result result;
    const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);
    const uint64_t candidates[] = {HOP_A, HOP_B};

    ctx.plan_returns[0] = -ENOENT;
    init_eack(&eack);
    assert(app_gateway_eack_send_to_candidates(&eack,
                                               candidates,
                                               sizeof(candidates) / sizeof(candidates[0]),
                                               &ops,
                                               &result) == 0);
    assert(ctx.plan_count == 2);
    assert(ctx.prepare_count == 1);
    assert(ctx.send_channel9_count == 1);
    assert(ctx.send_c5_count == 0);
    assert(ctx.note_tx_count == 1);
    assert(ctx.note_channel9_count == 1);
    assert(ctx.planned_hops[0] == HOP_A);
    assert(ctx.planned_hops[1] == HOP_B);
    assert(ctx.sent_channel9.next_hop_id == HOP_B);
    assert(ctx.sent_channel9.radio_channel == MESH_EVENT_CHANNEL);
    assert(result.mode == APP_GATEWAY_EACK_SEND_CHANNEL9);
    assert(result.channel9_candidate_count == 2u);
    assert(result.channel9_attempt_count == 2u);
    assert(result.channel9_plan_ret == 0);
    assert(result.channel9_prepare_ret == 0);
    assert(result.channel9_send_ret == 0);
    assert(result.channel9_next_hop_id == HOP_B);
    assert(ctx.noted_channel9_hop == HOP_B);
    assert(ctx.noted_channel9_start == ctx.planned_starts[1]);
}

static void test_duplicate_and_invalid_candidates_are_not_planned(void)
{
    struct policy_test_ctx ctx = {0};
    struct mesh_outbound eack;
    struct app_gateway_eack_policy_result result;
    const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);
    const uint64_t candidates[] = {HOP_A, HOP_A, 0u, MESH_BROADCAST_ID, HOP_B};

    ctx.prepare_returns[0] = -EBUSY;
    init_eack(&eack);
    assert(app_gateway_eack_send_to_candidates(&eack,
                                               candidates,
                                               sizeof(candidates) / sizeof(candidates[0]),
                                               &ops,
                                               &result) == 0);
    assert(ctx.plan_count == 2);
    assert(ctx.prepare_count == 2);
    assert(ctx.send_channel9_count == 1);
    assert(ctx.send_c5_count == 0);
    assert(ctx.planned_hops[0] == HOP_A);
    assert(ctx.planned_hops[1] == HOP_B);
    assert(ctx.sent_channel9.next_hop_id == HOP_B);
    assert(result.mode == APP_GATEWAY_EACK_SEND_CHANNEL9);
    assert(result.channel9_candidate_count == 2u);
    assert(result.channel9_attempt_count == 2u);
    assert(result.channel9_prepare_ret == 0);
    assert(result.channel9_next_hop_id == HOP_B);
}

static void test_falls_back_to_c5_only_after_all_channel9_sends_fail(void)
{
    struct policy_test_ctx ctx = {0};
    struct mesh_outbound eack;
    struct app_gateway_eack_policy_result result;
    const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);
    const uint64_t candidates[] = {HOP_A, HOP_B};

    ctx.send_channel9_returns[0] = -EIO;
    ctx.send_channel9_returns[1] = -EIO;
    init_eack(&eack);
    eack.earliest_tx_ms = 4242u;
    assert(app_gateway_eack_send_to_candidates(&eack,
                                               candidates,
                                               sizeof(candidates) / sizeof(candidates[0]),
                                               &ops,
                                               &result) == 0);
    assert(ctx.plan_count == 2);
    assert(ctx.prepare_count == 2);
    assert(ctx.send_channel9_count == 2);
    assert(ctx.send_c5_count == 1);
    assert(ctx.note_tx_count == 1);
    assert(ctx.note_channel9_count == 0);
    assert(ctx.sent_c5.next_hop_id == MESH_BROADCAST_ID);
    assert(ctx.sent_c5.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(ctx.sent_c5.earliest_tx_ms == 0u);
    assert(eack.next_hop_id == MESH_BROADCAST_ID);
    assert(eack.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(eack.earliest_tx_ms == 0u);
    assert(result.mode == APP_GATEWAY_EACK_SEND_C5_FLOOD);
    assert(result.channel9_candidate_count == 2u);
    assert(result.channel9_attempt_count == 2u);
    assert(result.channel9_send_ret == -EIO);
    assert(result.c5_send_ret == 0);
}

static void test_no_return_candidates_uses_bounded_c5_recovery(void)
{
    struct policy_test_ctx ctx = {0};
    struct mesh_outbound eack;
    struct app_gateway_eack_policy_result result;
    const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);

    init_eack(&eack);
    assert(app_gateway_eack_send_to_candidates(&eack,
                                               NULL,
                                               0u,
                                               &ops,
                                               &result) == 0);
    assert(ctx.plan_count == 0);
    assert(ctx.prepare_count == 0);
    assert(ctx.send_channel9_count == 0);
    assert(ctx.send_c5_count == 1);
    assert(result.mode == APP_GATEWAY_EACK_SEND_C5_FLOOD);
    assert(result.channel9_candidate_count == 0u);
    assert(result.channel9_attempt_count == 0u);
    assert(ctx.sent_c5.next_hop_id == MESH_BROADCAST_ID);
    assert(ctx.sent_c5.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
}

int main(void)
{
    test_channel9_uses_second_candidate_without_c5_flood();
    test_duplicate_and_invalid_candidates_are_not_planned();
    test_falls_back_to_c5_only_after_all_channel9_sends_fail();
    test_no_return_candidates_uses_bounded_c5_recovery();
    return 0;
}
