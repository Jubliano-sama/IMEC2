#include "app_mesh_flood.h"

#include "mesh_relay.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TEST_ANCHOR = 0x2301u,
    TEST_GATEWAY = 0x6601u,
};

struct flood_test_ctx {
    uint32_t now_ms;
    uint16_t delays[32];
    uint8_t send_count;
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
    (void)sniff_ms;
    (void)ctx;
    return true;
}

static int test_send(const struct mesh_outbound *out, void *ctx)
{
    struct flood_test_ctx *test = ctx;
    uint16_t delay_ms = 0u;

    assert(test->send_count < sizeof(test->delays) / sizeof(test->delays[0]));
    assert(mesh_route_request_reply_rx_delay_ms(out, &delay_ms));
    test->delays[test->send_count++] = delay_ms;
    return 0;
}

static void test_route_request_eta_counts_down_per_repeat(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct app_mesh_flood_result result;
    struct flood_test_ctx ctx = {
        .now_ms = 900u,
    };
    const struct app_mesh_flood_ops ops = {
        .now_ms = test_now_ms,
        .sleep_until_ms = test_sleep_until_ms,
        .defer_active = test_defer_active,
        .c5_quiet = test_c5_quiet,
        .send = test_send,
        .ctx = &ctx,
    };
    uint16_t original_delay_ms = 0u;
    uint8_t repeat_limit = app_mesh_flood_repeat_limit();

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR,
                    TEST_GATEWAY,
                    1u);
    assert(mesh_relay_build_route_request_with_timing_flags(
               &relay,
               TEST_GATEWAY,
               NULL,
               0u,
               0u,
               620u,
               &route_req,
               1000u) == PROTO_OK);

    assert(app_mesh_flood_send_bounded(&route_req, &ops, &result) == 0);
    assert(result.sent_count == repeat_limit);
    assert(result.busy_skip_count == 0u);
    assert(result.deferred_count == 0u);
    assert(result.first_due_ms == 1000u);
    assert(result.last_due_ms == 1000u +
           ((uint32_t)(repeat_limit - 1u) * FLOOD_RELAY_REPEAT_MS));
    assert(ctx.send_count == repeat_limit);
    assert(ctx.delays[0] == 620u);
    assert(ctx.delays[1] == 620u - FLOOD_RELAY_REPEAT_MS);
    assert(ctx.delays[repeat_limit - 1u] ==
           620u - ((uint16_t)(repeat_limit - 1u) * FLOOD_RELAY_REPEAT_MS));
    assert(mesh_route_request_reply_rx_delay_ms(&route_req, &original_delay_ms));
    assert(original_delay_ms == 620u);
}

int main(void)
{
    test_route_request_eta_counts_down_per_repeat();
    return 0;
}
