#include "app_mesh_flood.h"

#include "mesh_relay.h"
#include "survey.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    TEST_GATEWAY = 0x6601u,
};

struct flood_test_ctx {
    uint32_t now_ms;
    uint16_t delays[32];
    uint32_t message_ages_ms[32];
    uint32_t flood_tlv_ages_ms[32];
    uint32_t queued_at_ms[32];
    uint8_t send_count;
    uint8_t quiet_count;
    uint8_t quiet_busy_remaining;
    uint8_t send_busy_remaining;
    uint8_t quiet_busy_at_count;
    bool defer_active;
    bool defer_after_quiet;
    bool quiet_busy_once;
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
    return ((struct flood_test_ctx *)ctx)->defer_active;
}

static bool test_c5_quiet(uint32_t sniff_ms, void *ctx)
{
    struct flood_test_ctx *test = ctx;

    assert(sniff_ms == 20u);
    test->quiet_count++;
    if (test->quiet_busy_once &&
        test->quiet_count == test->quiet_busy_at_count) {
        test->quiet_busy_once = false;
        return false;
    }
    if (test->defer_after_quiet) {
        test->defer_active = true;
    }
    if (test->quiet_busy_remaining > 0u) {
        test->quiet_busy_remaining--;
        return false;
    }
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
    const uint8_t *age_value = NULL;
    uint8_t age_len = 0u;

    if (test->send_busy_remaining > 0u) {
        test->send_busy_remaining--;
        return -EBUSY;
    }
    assert(test->send_count < sizeof(test->delays) / sizeof(test->delays[0]));
    assert(out->packet.msg_type == MSG_SURVEY_DISCOVERY_START ||
           out->packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    test->delays[test->send_count] = (uint16_t)test->now_ms;
    test->message_ages_ms[test->send_count] = out->packet.message_age_ms;
    if (out->packet.msg_type == MSG_SURVEY_DISCOVERY_START) {
        assert(tlv_find(out->payload, out->payload_len,
                        TLV_FLOOD_PACKET_AGE_MS,
                        &age_value, &age_len) == PROTO_OK);
        assert(age_len == sizeof(uint32_t));
        test->flood_tlv_ages_ms[test->send_count] =
            proto_get_u32_le(age_value);
    }
    test->queued_at_ms[test->send_count] = out->queued_at_ms;
    test->send_count++;
    return 0;
}

static void test_survey_start_repeats_age_from_one_origin(void)
{
    struct mesh_outbound gateway_adv = {
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    const struct survey_discovery_config config = {
        .survey_id = UINT32_C(0x50665006),
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
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
    size_t payload_len = 0u;
    uint8_t repeat_limit = app_mesh_flood_repeat_limit();

    assert(survey_append_discovery_start_tlvs(
               gateway_adv.payload, sizeof(gateway_adv.payload),
               &payload_len, &config) == PROTO_OK);
    assert(tlv_append_u32(gateway_adv.payload, sizeof(gateway_adv.payload),
                          &payload_len, TLV_FLOOD_PACKET_AGE_MS, 0u) == PROTO_OK);
    assert(survey_init_discovery_start_packet(
               &gateway_adv.packet, TEST_GATEWAY, &config, 1u,
               (uint8_t)payload_len) == PROTO_OK);
    gateway_adv.payload_len = (uint8_t)payload_len;
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
    for (uint8_t i = 0u; i < repeat_limit; i++) {
        assert(ctx.message_ages_ms[i] ==
               (uint32_t)i * FLOOD_RELAY_REPEAT_MS);
        assert(ctx.flood_tlv_ages_ms[i] == ctx.message_ages_ms[i]);
        assert(ctx.queued_at_ms[i] == ctx.delays[i]);
        if (i > 0u) {
            assert(ctx.message_ages_ms[i] > ctx.message_ages_ms[i - 1u]);
        }
    }

    assert(app_mesh_flood_backoff_ms(0u, 0u) == 20u);
    assert(app_mesh_flood_backoff_ms(0u, 19u) == 39u);
    assert(app_mesh_flood_backoff_ms(1u, 0u) == 40u);
    assert(app_mesh_flood_backoff_ms(1u, 39u) == 79u);
    assert(app_mesh_flood_backoff_ms(2u, 79u) == 159u);
    assert(app_mesh_flood_backoff_ms(UINT8_MAX, 0u) == C5_POLITE_BACKOFF_MAX_MS);
}

static void test_malformed_flood_age_fails_closed(void)
{
    struct mesh_outbound malformed = {
        .packet = {
            .msg_type = MSG_SURVEY_DISCOVERY_START,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .payload_len = 3u,
        },
        .payload = {TLV_FLOOD_PACKET_AGE_MS, 1u, 0u},
        .payload_len = 3u,
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    struct app_mesh_flood_result result;
    struct flood_test_ctx ctx = {.now_ms = 900u};
    const struct app_mesh_flood_ops ops = {
        .now_ms = test_now_ms,
        .sleep_until_ms = test_sleep_until_ms,
        .defer_active = test_defer_active,
        .c5_quiet = test_c5_quiet,
        .random_u32 = test_random_u32,
        .send = test_send,
        .ctx = &ctx,
    };

    assert(app_mesh_flood_send_bounded(&malformed, &ops, &result) == -EBADMSG);
    assert(result.sent_count == 0u);
    assert(ctx.send_count == 0u);
}

static void test_resumable_deferral_saturates_and_rollover_rebases(void)
{
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 1u,
            .seq = 1u,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    struct app_mesh_flood_progress progress = {0};
    struct app_mesh_flood_result result = {0};
    struct flood_test_ctx ctx = {
        .now_ms = 900u,
        .defer_active = true,
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

    for (uint16_t i = 0u; i < 300u; i++) {
        assert(app_mesh_flood_send_bounded_resume(
                   &gateway_adv, &ops, &progress, &result) == -EAGAIN);
    }
    assert(result.deferred_count == UINT8_MAX);
    assert(progress.next_opportunity == 0u);

    memset(&progress, 0, sizeof(progress));
    progress.initialized = true;
    progress.due_ms = UINT32_MAX - 10u;
    app_mesh_flood_progress_rebase(&progress, 20u);
    assert(progress.due_ms == 9u);
}

static void test_pause_after_quiet_check_prevents_send(void)
{
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 2u,
            .seq = 2u,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    struct app_mesh_flood_progress progress = {0};
    struct app_mesh_flood_result result = {0};
    struct flood_test_ctx ctx = {
        .now_ms = 1000u,
        .defer_after_quiet = true,
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

    assert(app_mesh_flood_send_bounded_resume(
               &gateway_adv, &ops, &progress, &result) == -EAGAIN);
    assert(ctx.quiet_count == 1u);
    assert(ctx.send_count == 0u);
    assert(progress.next_opportunity == 0u);
    assert(result.deferred_count == 1u);
}

static void test_pre_rf_blocks_preserve_four_real_opportunities(void)
{
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 3u,
            .seq = 3u,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    struct app_mesh_flood_progress progress = {0};
    struct app_mesh_flood_result result = {0};
    struct flood_test_ctx ctx = {
        .now_ms = 1000u,
        .quiet_busy_remaining = 5u,
        .send_busy_remaining = 3u,
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

    for (uint8_t blocked = 0u; blocked < 8u; blocked++) {
        assert(app_mesh_flood_send_bounded_resume(
                   &gateway_adv, &ops, &progress, &result) == -EAGAIN);
        assert(progress.next_opportunity == 0u);
        assert(result.sent_count == 0u);
        assert(!progress.complete);
    }
    assert(result.busy_skip_count == 8u);
    assert(app_mesh_flood_send_bounded_resume(
               &gateway_adv, &ops, &progress, &result) == 0);
    assert(progress.complete);
    assert(progress.next_opportunity == app_mesh_flood_repeat_limit());
    assert(result.sent_count == app_mesh_flood_repeat_limit());
    assert(ctx.send_count == app_mesh_flood_repeat_limit());
    assert(ctx.quiet_count == 8u + app_mesh_flood_repeat_limit());
}

static void test_partial_success_never_completes_a_four_frame_burst(void)
{
    for (uint8_t sent_before_busy = 1u;
         sent_before_busy < app_mesh_flood_repeat_limit();
         sent_before_busy++) {
        struct mesh_outbound gateway_adv = {
            .packet = {
                .msg_type = MSG_GATEWAY_ROUTE_ADV,
                .src_id = TEST_GATEWAY,
                .dst_id = MESH_BROADCAST_ID,
                .session_id = 30u + sent_before_busy,
                .seq = 30u + sent_before_busy,
            },
            .next_hop_id = MESH_BROADCAST_ID,
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
            .earliest_tx_ms = 1000u,
        };
        struct app_mesh_flood_progress progress = {0};
        struct app_mesh_flood_result result = {0};
        struct flood_test_ctx ctx = {
            .now_ms = 1000u,
            .quiet_busy_at_count = sent_before_busy + 1u,
            .quiet_busy_once = true,
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

        assert(app_mesh_flood_send_bounded_resume(
                   &gateway_adv, &ops, &progress, &result) == -EAGAIN);
        assert(!progress.complete);
        assert(progress.next_opportunity == sent_before_busy);
        assert(result.sent_count == sent_before_busy);
        assert(ctx.send_count == sent_before_busy);

        assert(app_mesh_flood_send_bounded_resume(
                   &gateway_adv, &ops, &progress, &result) == 0);
        assert(progress.complete);
        assert(progress.next_opportunity == app_mesh_flood_repeat_limit());
        assert(result.sent_count == app_mesh_flood_repeat_limit());
        assert(ctx.send_count == app_mesh_flood_repeat_limit());
    }
}

static void test_one_shot_continuous_busy_returns_boundedly(void)
{
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 4u,
            .seq = 4u,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = UINT32_MAX - 10u,
    };
    struct app_mesh_flood_result result = {0};
    struct flood_test_ctx ctx = {
        .now_ms = UINT32_MAX - 10u,
        .quiet_busy_remaining = UINT8_MAX,
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

    assert(app_mesh_flood_send_bounded(&gateway_adv, &ops, &result) ==
           -EAGAIN);
    assert(result.sent_count == 0u);
    assert(result.busy_skip_count == 1u);
    assert(ctx.quiet_count == 1u);
    assert(ctx.send_count == 0u);
}

static void test_scheduler_owned_opportunity_sends_exactly_once(void)
{
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 6u,
            .seq = 6u,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = 1000u,
    };
    struct app_mesh_flood_result result = {0};
    struct flood_test_ctx ctx = {
        .now_ms = 900u,
        .defer_active = true,
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

    assert(app_mesh_flood_send_opportunity(
               &gateway_adv, &ops, &result) == -EAGAIN);
    assert(result.sent_count == 0u);
    assert(ctx.quiet_count == 0u);
    assert(ctx.send_count == 0u);

    ctx.defer_active = false;
    ctx.quiet_busy_remaining = 1u;
    assert(app_mesh_flood_send_opportunity(
               &gateway_adv, &ops, &result) == -EAGAIN);
    assert(result.sent_count == 0u);
    assert(result.busy_skip_count == 1u);
    assert(ctx.send_count == 0u);

    for (uint8_t opportunity = 0u;
         opportunity < app_mesh_flood_repeat_limit();
         opportunity++) {
        memset(&result, 0, sizeof(result));
        assert(app_mesh_flood_send_opportunity(
                   &gateway_adv, &ops, &result) == 0);
        assert(result.sent_count == 1u);
    }
    assert(ctx.send_count == app_mesh_flood_repeat_limit());
}

static void test_resumable_continuous_busy_times_out_across_rollover(void)
{
    const uint32_t start_ms = UINT32_MAX - 30u;
    const uint32_t deadline_ms = start_ms + 50u;
    struct mesh_outbound gateway_adv = {
        .packet = {
            .msg_type = MSG_GATEWAY_ROUTE_ADV,
            .src_id = TEST_GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 5u,
            .seq = 5u,
        },
        .next_hop_id = MESH_BROADCAST_ID,
        .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        .earliest_tx_ms = start_ms,
    };
    struct app_mesh_flood_progress progress = {0};
    struct app_mesh_flood_result result = {0};
    struct flood_test_ctx ctx = {
        .now_ms = start_ms,
        .quiet_busy_remaining = UINT8_MAX,
    };
    const struct app_mesh_flood_ops ops = {
        .now_ms = test_now_ms,
        .sleep_until_ms = test_sleep_until_ms,
        .defer_active = test_defer_active,
        .c5_quiet = test_c5_quiet,
        .random_u32 = test_random_u32,
        .send = test_send,
        .absolute_deadline_ms = deadline_ms,
        .absolute_deadline_valid = true,
        .ctx = &ctx,
    };

    assert(app_mesh_flood_send_bounded_resume(
               &gateway_adv, &ops, &progress, &result) == -EAGAIN);
    assert(app_mesh_flood_send_bounded_resume(
               &gateway_adv, &ops, &progress, &result) == -EAGAIN);
    assert(progress.due_ms == deadline_ms);
    assert(app_mesh_flood_send_bounded_resume(
               &gateway_adv, &ops, &progress, &result) == -ETIMEDOUT);
    assert(ctx.now_ms == deadline_ms);
    assert(progress.next_opportunity == 0u);
    assert(result.sent_count == 0u);
    assert(ctx.send_count == 0u);
    assert(ctx.quiet_count == 2u);
}

int main(void)
{
    test_survey_start_repeats_age_from_one_origin();
    test_malformed_flood_age_fails_closed();
    test_resumable_deferral_saturates_and_rollover_rebases();
    test_pause_after_quiet_check_prevents_send();
    test_pre_rf_blocks_preserve_four_real_opportunities();
    test_partial_success_never_completes_a_four_frame_burst();
    test_one_shot_continuous_busy_returns_boundedly();
    test_scheduler_owned_opportunity_sends_exactly_once();
    test_resumable_continuous_busy_times_out_across_rollover();
    return 0;
}
