#include "app_gateway_eack_policy.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
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
    uint32_t now_ms;
    int plan_count;
    int prepare_count;
    int send_channel9_count;
    int send_c5_count;
    int note_tx_count;
    int note_channel9_count;
    int now_count;
    bool mutate_failed_prepare;
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
    if (test->prepare_returns[index] != 0 && test->mutate_failed_prepare) {
        out->packet.session_id = 0xdeadbeefu;
        out->packet.seq = 0xbeefu;
        out->payload[0] = 0xeeu;
        out->payload[1] = 0xddu;
        out->payload_len = 2u;
        out->queued_at_ms = 0x12345678u;
        out->earliest_tx_ms = 0x87654321u;
    }
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

static uint32_t test_now_ms(void *ctx)
{
    struct policy_test_ctx *test = ctx;

    assert(test != NULL);
    test->now_count++;
    return test->now_ms;
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
        .now_ms = test_now_ms,
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

static void test_current_channel9_success_preempts_later_candidates(void)
{
    struct policy_test_ctx ctx = {
        .now_ms = 4321u,
    };
    struct mesh_outbound eack;
    struct app_gateway_eack_policy_result result;
    const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);
    const uint64_t candidates[] = {HOP_B};
    const struct mesh_event_plan current_plan = {
        .action = MESH_EVENT_PLAN_START,
        .start_ms = 900u,
        .end_ms = 920u,
        .window_ms = 20u,
    };

    init_eack(&eack);
    eack.packet.session_id = 0x01020304u;
    eack.packet.seq = 0x1122u;
    eack.payload[0] = 0xa1u;
    eack.payload[1] = 0xb2u;
    eack.payload_len = 2u;

    assert(app_gateway_eack_send_to_candidates_with_current_channel9(
               &eack,
               HOP_A,
               &current_plan,
               candidates,
               sizeof(candidates) / sizeof(candidates[0]),
               &ops,
               &result) == 0);
    assert(ctx.plan_count == 0);
    assert(ctx.prepare_count == 1);
    assert(ctx.send_channel9_count == 1);
    assert(ctx.send_c5_count == 0);
    assert(ctx.note_tx_count == 1);
    assert(ctx.note_channel9_count == 1);
    assert(ctx.now_count == 0);
    assert(ctx.sent_channel9.next_hop_id == HOP_A);
    assert(ctx.sent_channel9.radio_channel == MESH_EVENT_CHANNEL);
    assert(ctx.sent_channel9.earliest_tx_ms == current_plan.start_ms + 3u);
    assert(ctx.sent_channel9.packet.session_id == 0x01020304u);
    assert(ctx.sent_channel9.packet.seq == 0x1122u);
    assert(ctx.sent_channel9.payload_len == 2u);
    assert(ctx.sent_channel9.payload[0] == 0xa1u);
    assert(ctx.sent_channel9.payload[1] == 0xb2u);
    assert(result.mode == APP_GATEWAY_EACK_SEND_CURRENT_CHANNEL9);
    assert(result.channel9_candidate_count == 0u);
    assert(result.channel9_attempt_count == 1u);
    assert(result.channel9_send_ret == 0);
    assert(result.channel9_next_hop_id == HOP_A);
    assert(ctx.noted_channel9_hop == HOP_A);
    assert(ctx.noted_channel9_start == current_plan.start_ms);
    assert(eack.next_hop_id == HOP_A);
    assert(eack.radio_channel == MESH_EVENT_CHANNEL);
    assert(eack.earliest_tx_ms == current_plan.start_ms + 3u);
}

static void test_current_channel9_failure_stays_on_negotiated_lane(void)
{
    struct policy_test_ctx ctx = {
        .send_channel9_returns = {-EIO},
        .prepare_returns = {0, -EBUSY},
        .mutate_failed_prepare = true,
    };
    struct mesh_outbound eack;
    struct app_gateway_eack_policy_result result;
    const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);
    const uint64_t candidates[] = {HOP_B};
    const struct mesh_event_plan current_plan = {
        .action = MESH_EVENT_PLAN_START,
        .start_ms = 900u,
        .end_ms = 920u,
        .window_ms = 20u,
    };

    init_eack(&eack);
    eack.packet.session_id = 0x01020304u;
    eack.packet.seq = 0x1122u;
    eack.packet.ttl = 3u;
    eack.payload[0] = 0xa1u;
    eack.payload[1] = 0xb2u;
    eack.payload[2] = 0xc3u;
    eack.payload[3] = 0xd4u;
    eack.payload_len = 4u;
    eack.queued_at_ms = 0x55667788u;

    assert(app_gateway_eack_send_to_candidates_with_current_channel9(
               &eack,
               HOP_A,
               &current_plan,
               candidates,
               sizeof(candidates) / sizeof(candidates[0]),
               &ops,
               &result) == -EIO);
    assert(ctx.plan_count == 0);
    assert(ctx.prepare_count == 1);
    assert(ctx.send_channel9_count == 1);
    assert(ctx.send_c5_count == 0);
    assert(ctx.note_tx_count == 0);
    assert(ctx.note_channel9_count == 0);
    assert(ctx.now_count == 0);
    assert(ctx.sent_channel9.next_hop_id == HOP_A);
    assert(ctx.sent_channel9.radio_channel == MESH_EVENT_CHANNEL);
    assert(eack.packet.msg_type == MSG_GATEWAY_COLLECTION_EACK);
    assert(eack.packet.session_id == 0x01020304u);
    assert(eack.packet.seq == 0x1122u);
    assert(eack.packet.ttl == 3u);
    assert(eack.payload_len == 4u);
    assert(eack.payload[0] == 0xa1u);
    assert(eack.payload[1] == 0xb2u);
    assert(eack.payload[2] == 0xc3u);
    assert(eack.payload[3] == 0xd4u);
    assert(eack.queued_at_ms == 0x55667788u);
    assert(eack.next_hop_id == 0u);
    assert(eack.radio_channel == 0u);
    assert(eack.earliest_tx_ms == 0u);
    assert(result.mode == APP_GATEWAY_EACK_SEND_NONE);
    assert(result.channel9_candidate_count == 0u);
    assert(result.channel9_attempt_count == 1u);
    assert(result.channel9_send_ret == -EIO);
    assert(result.channel9_prepare_ret == 0);
    assert(result.c5_send_ret == 0);
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
    assert(result.channel9_attempt_count == 1u);
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
    assert(result.channel9_attempt_count == 1u);
    assert(result.channel9_prepare_ret == 0);
    assert(result.channel9_next_hop_id == HOP_B);
}

static void test_failed_send_never_falls_through_to_another_rf_lane(void)
{
    const uint64_t candidates[] = {
        HOP_A,
        HOP_B,
        UINT64_C(0x3333444455556666),
        UINT64_C(0x4444555566667777),
        UINT64_C(0x5555666677778888),
        UINT64_C(0x6666777788889999),
        UINT64_C(0x777788889999aaaa),
        UINT64_C(0x88889999aaaabbbb),
    };
    const int failures[] = {-EIO, -ETIMEDOUT, -EBUSY};

    for (size_t failure_index = 0u;
         failure_index < sizeof(failures) / sizeof(failures[0]);
         failure_index++) {
        for (size_t candidate_count = 1u;
             candidate_count <= sizeof(candidates) / sizeof(candidates[0]);
             candidate_count++) {
            struct policy_test_ctx ctx = {0};
            struct mesh_outbound eack;
            struct mesh_outbound original_eack;
            struct app_gateway_eack_policy_result result;
            const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);

            ctx.send_channel9_returns[0] = failures[failure_index];
            init_eack(&eack);
            eack.earliest_tx_ms = 4242u;
            eack.payload[0] = 0xa5u;
            eack.payload_len = 1u;
            original_eack = eack;

            assert(app_gateway_eack_send_to_candidates(&eack,
                                                       candidates,
                                                       candidate_count,
                                                       &ops,
                                                       &result) ==
                   failures[failure_index]);
            assert(ctx.plan_count == 1);
            assert(ctx.prepare_count == 1);
            assert(ctx.send_channel9_count == 1);
            assert(ctx.send_c5_count == 0);
            assert(ctx.note_tx_count == 0);
            assert(ctx.note_channel9_count == 0);
            assert(memcmp(&eack, &original_eack, sizeof(eack)) == 0);
            assert(result.mode == APP_GATEWAY_EACK_SEND_NONE);
            assert(result.channel9_candidate_count == 1u);
            assert(result.channel9_attempt_count == 1u);
            assert(result.channel9_next_hop_id == HOP_A);
            assert(result.channel9_send_ret == failures[failure_index]);
            assert(result.c5_send_ret == 0);
        }
    }
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

static void test_c5_pre_rf_deferrals_never_complete_a_round(void)
{
    const int deferrals[] = {
        -EAGAIN,
        -EBUSY,
        -ENOSPC,
        -ETIMEDOUT,
    };

    for (size_t i = 0u; i < sizeof(deferrals) / sizeof(deferrals[0]); i++) {
        struct policy_test_ctx ctx = {
            .c5_return = deferrals[i],
        };
        struct mesh_outbound eack;
        struct mesh_outbound original;
        struct app_gateway_eack_policy_result result;
        const struct app_gateway_eack_policy_ops ops = test_ops(&ctx);

        init_eack(&eack);
        eack.payload[0] = (uint8_t)i;
        eack.payload_len = 1u;
        original = eack;

        assert(app_gateway_eack_send_to_candidates(&eack,
                                                   NULL,
                                                   0u,
                                                   &ops,
                                                   &result) == deferrals[i]);
        assert(ctx.send_c5_count == 1);
        assert(ctx.note_tx_count == 0);
        assert(ctx.note_channel9_count == 0);
        assert(result.mode == APP_GATEWAY_EACK_SEND_NONE);
        assert(result.c5_send_ret == deferrals[i]);
        assert(eack.packet.session_id == original.packet.session_id);
        assert(eack.packet.seq == original.packet.seq);
        assert(eack.payload_len == original.payload_len);
        assert(eack.payload[0] == original.payload[0]);
    }
}

int main(void)
{
    test_current_channel9_success_preempts_later_candidates();
    test_current_channel9_failure_stays_on_negotiated_lane();
    test_channel9_uses_second_candidate_without_c5_flood();
    test_duplicate_and_invalid_candidates_are_not_planned();
    test_failed_send_never_falls_through_to_another_rf_lane();
    test_no_return_candidates_uses_bounded_c5_recovery();
    test_c5_pre_rf_deferrals_never_complete_a_round();
    return 0;
}
