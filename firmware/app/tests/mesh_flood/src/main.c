#include "app_mesh_flood.h"

#include <errno.h>

#include <zephyr/ztest.h>

struct test_ctx {
    uint32_t now_ms;
    uint32_t slept_until[32];
    uint32_t send_at[32];
    bool quiet[32];
    uint8_t quiet_count;
    uint8_t sleep_count;
    uint8_t send_count;
    uint8_t quiet_calls;
    uint8_t defer_after_calls;
    uint8_t defer_calls;
};

static struct mesh_outbound make_flood(uint32_t earliest_tx_ms)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = 0x9999888877776666ull,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 1001u,
            .seq = 3u,
            .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = earliest_tx_ms,
        .earliest_tx_valid = true,
    };
}

static uint32_t test_now_ms(void *ctx)
{
    return ((struct test_ctx *)ctx)->now_ms;
}

static void test_sleep_until_ms(uint32_t due_ms, void *ctx)
{
    struct test_ctx *test = ctx;

    zassert_true(test->sleep_count < ARRAY_SIZE(test->slept_until));
    test->slept_until[test->sleep_count++] = due_ms;
    test->now_ms = due_ms;
}

static bool test_defer_active(void *ctx)
{
    struct test_ctx *test = ctx;

    test->defer_calls++;
    return test->defer_after_calls != 0u &&
           test->defer_calls >= test->defer_after_calls;
}

static bool test_c5_quiet(uint32_t sniff_ms, void *ctx)
{
    struct test_ctx *test = ctx;
    uint8_t call = test->quiet_calls++;

    zassert_equal(sniff_ms, C5_POLITE_SNIFF_MS);
    if (call < test->quiet_count) {
        return test->quiet[call];
    }
    return true;
}

static uint32_t test_random_u32(void *ctx)
{
    ARG_UNUSED(ctx);
    return 0u;
}

static int test_send(const struct mesh_outbound *out, void *ctx)
{
    struct test_ctx *test = ctx;

    zassert_not_null(out);
    zassert_equal(out->packet.dst_id, MESH_BROADCAST_ID);
    zassert_equal(out->next_hop_id, MESH_BROADCAST_ID);
    zassert_true(test->send_count < ARRAY_SIZE(test->send_at));
    test->send_at[test->send_count++] = test->now_ms;
    return 0;
}

static struct app_mesh_flood_ops make_ops(struct test_ctx *ctx)
{
    return (struct app_mesh_flood_ops) {
        .now_ms = test_now_ms,
        .sleep_until_ms = test_sleep_until_ms,
        .defer_active = test_defer_active,
        .c5_quiet = test_c5_quiet,
        .random_u32 = test_random_u32,
        .send = test_send,
        .ctx = ctx,
    };
}

ZTEST(mesh_flood, test_waits_for_forward_due_and_repeats_on_schedule)
{
    struct mesh_outbound flood = make_flood(1100u);
    struct test_ctx ctx = {
        .now_ms = 1000u,
    };
    struct app_mesh_flood_ops ops = make_ops(&ctx);
    struct app_mesh_flood_result result;

    zassert_ok(app_mesh_flood_send_bounded(&flood, &ops, &result));
    zassert_equal(result.sent_count, app_mesh_flood_repeat_limit());
    zassert_equal(result.busy_skip_count, 0u);
    zassert_equal(result.first_due_ms, 1100u);
    zassert_equal(ctx.slept_until[0], 1100u);
    zassert_equal(ctx.send_at[0], 1100u);
    zassert_equal(ctx.send_at[1], 1100u + FLOOD_RELAY_REPEAT_MS);
    zassert_equal(result.last_due_ms,
                  1100u + ((uint32_t)(app_mesh_flood_repeat_limit() - 1u) *
                           FLOOD_RELAY_REPEAT_MS));
}

ZTEST(mesh_flood, test_busy_c5_preserves_all_real_opportunities)
{
    struct mesh_outbound flood = make_flood(2000u);
    struct test_ctx ctx = {
        .now_ms = 2000u,
        .quiet = { false, false, true },
        .quiet_count = 3u,
    };
    struct app_mesh_flood_ops ops = make_ops(&ctx);
    struct app_mesh_flood_progress progress = {0};
    struct app_mesh_flood_result result;

    zassert_equal(app_mesh_flood_send_bounded_resume(
                      &flood, &ops, &progress, &result), -EAGAIN);
    zassert_equal(progress.next_opportunity, 0u);
    zassert_equal(app_mesh_flood_send_bounded_resume(
                      &flood, &ops, &progress, &result), -EAGAIN);
    zassert_equal(progress.next_opportunity, 0u);
    zassert_ok(app_mesh_flood_send_bounded_resume(
                   &flood, &ops, &progress, &result));
    zassert_equal(result.sent_count, app_mesh_flood_repeat_limit());
    zassert_equal(result.busy_skip_count, 2u);
    zassert_equal(ctx.send_at[0], 2060u);
    zassert_equal(result.last_due_ms,
                  2060u + ((uint32_t)(app_mesh_flood_repeat_limit() - 1u) *
                           FLOOD_RELAY_REPEAT_MS));
}

ZTEST(mesh_flood, test_defers_before_click_service_without_sending)
{
    struct mesh_outbound flood = make_flood(3000u);
    struct test_ctx ctx = {
        .now_ms = 3000u,
        .defer_after_calls = 1u,
    };
    struct app_mesh_flood_ops ops = make_ops(&ctx);
    struct app_mesh_flood_result result;

    zassert_equal(app_mesh_flood_send_bounded(&flood, &ops, &result), -EAGAIN);
    zassert_equal(result.sent_count, 0u);
    zassert_equal(result.deferred_count, 1u);
    zassert_equal(ctx.send_count, 0u);
    zassert_equal(ctx.quiet_calls, 0u);
}

ZTEST(mesh_flood, test_stops_after_bounded_repeat_count)
{
    struct mesh_outbound flood = make_flood(4000u);
    struct test_ctx ctx = {
        .now_ms = 4000u,
    };
    struct app_mesh_flood_ops ops = make_ops(&ctx);
    struct app_mesh_flood_result result;

    zassert_equal(C5_POLITE_SNIFF_MS, 20u);
    zassert_equal(app_mesh_flood_repeat_limit(), 4u);
    zassert_ok(app_mesh_flood_send_bounded(&flood, &ops, &result));
    zassert_equal(ctx.send_count, app_mesh_flood_repeat_limit());
    zassert_equal(ctx.quiet_calls, app_mesh_flood_repeat_limit());
    zassert_equal(result.sent_count, app_mesh_flood_repeat_limit());
}

ZTEST_SUITE(mesh_flood, NULL, NULL, NULL, NULL, NULL);
