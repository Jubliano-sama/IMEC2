#include "app_mesh_flood.h"

#include "mesh_relay.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TEST_GATEWAY = 0x6601u,
};

struct flood_test_ctx {
    uint32_t now_ms;
    uint16_t delays[32];
    uint8_t send_count;
    uint8_t quiet_count;
};

static uint32_t test_now_ms(void *ctx)
{
    return ((struct flood_test_ctx *)ctx)->now_ms;
}

static void test_sleep_until_ms(uint32_t due_ms, void *ctx)
{
    ((struct flood_test_ctx *)ctx)->now_ms = due_ms;
}

static bool test_defer_active(void *ctx)
{
    (void)ctx;
    return false;
}

static bool test_c5_quiet(uint32_t sniff_ms, void *ctx)
{
    struct flood_test_ctx *test = ctx;

    assert(sniff_ms == 20u);
    test->quiet_count++;
    return true;
}

static uint32_t test_random_u32(void *ctx)
{
    (void)ctx;
    return 0u;
}

static int test_send(const struct mesh_outbound *out, void *ctx)
{
    struct flood_test_ctx *test = ctx;

    assert(test->send_count < sizeof(test->delays) / sizeof(test->delays[0]));
    assert(out->packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    test->delays[test->send_count++] = (uint16_t)test->now_ms;
    return 0;
}

static void test_gateway_flood_repeats_at_burst_interval(void)
{
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    struct app_mesh_flood_result result;
    struct flood_test_ctx ctx = {
        .now_ms = 900u,
    };
    const struct app_mesh_flood_ops ops = {
        .now_ms = test_now_ms,
        .sleep_until_ms = test_sleep_until_ms,
        .defer_active = test_defer_active,
        .c5_quiet = test_c5_quiet,
        .random_u32 = test_random_u32,
        .send = test_send,
        .ctx = &ctx,
    };
    uint8_t repeat_limit = app_mesh_flood_repeat_limit();

    assert(repeat_limit == 4u);
    assert(app_mesh_flood_send_bounded(&gateway_adv, &ops, &result) == 0);
    assert(result.sent_count == repeat_limit);
    assert(result.busy_skip_count == 0u);
    assert(result.deferred_count == 0u);
    assert(result.first_due_ms == 1000u);
    assert(result.last_due_ms == 1000u +
           ((uint32_t)(repeat_limit - 1u) * FLOOD_RELAY_REPEAT_MS));
    assert(ctx.send_count == repeat_limit);
    assert(ctx.quiet_count == repeat_limit);
    assert(ctx.delays[0] == 1000u);
    assert(ctx.delays[1] == 1000u + FLOOD_RELAY_REPEAT_MS);
    assert(ctx.delays[repeat_limit - 1u] ==
           1000u + ((uint16_t)(repeat_limit - 1u) * FLOOD_RELAY_REPEAT_MS));

    assert(app_mesh_flood_backoff_ms(0u, 0u) == 20u);
    assert(app_mesh_flood_backoff_ms(0u, 19u) == 39u);
    assert(app_mesh_flood_backoff_ms(1u, 0u) == 40u);
    assert(app_mesh_flood_backoff_ms(1u, 39u) == 79u);
    assert(app_mesh_flood_backoff_ms(2u, 79u) == 159u);
    assert(app_mesh_flood_backoff_ms(UINT8_MAX, 0u) == C5_POLITE_BACKOFF_MAX_MS);
}

int main(void)
{
    test_gateway_flood_repeats_at_burst_interval();
    return 0;
}
