#include "app_mesh_result_handoff.h"

#include "protocol.h"

#include <errno.h>
#include <zephyr/ztest.h>

enum event_step {
    EVENT_SAVE = 1,
    EVENT_SEND_GRANT = 2,
    EVENT_NOTE_TX_SENT = 3,
    EVENT_NOTE_BUNDLE = 4,
};

struct test_ctx {
    enum event_step events[8];
    uint8_t event_count;
    int save_ret;
    int send_ret;
    const struct mesh_outbound *sent_grant;
    const struct mesh_outbound *noted_tx;
    const struct mesh_outbound *noted_bundle;
};

static void record_event(struct test_ctx *ctx, enum event_step event)
{
    zassert_true(ctx->event_count < ARRAY_SIZE(ctx->events));
    ctx->events[ctx->event_count++] = event;
}

static int save_child_custody(void *opaque)
{
    struct test_ctx *ctx = opaque;

    record_event(ctx, EVENT_SAVE);
    return ctx->save_ret;
}

static int send_result_grant(const struct mesh_outbound *out, void *opaque)
{
    struct test_ctx *ctx = opaque;

    record_event(ctx, EVENT_SEND_GRANT);
    ctx->sent_grant = out;
    return ctx->send_ret;
}

static void note_tx_sent(const struct mesh_outbound *out, void *opaque)
{
    struct test_ctx *ctx = opaque;

    record_event(ctx, EVENT_NOTE_TX_SENT);
    ctx->noted_tx = out;
}

static void note_result_bundle_forwarded(const struct mesh_outbound *out,
                                         void *opaque)
{
    struct test_ctx *ctx = opaque;

    record_event(ctx, EVENT_NOTE_BUNDLE);
    ctx->noted_bundle = out;
}

static struct app_mesh_result_handoff_ops make_ops(struct test_ctx *ctx)
{
    return (struct app_mesh_result_handoff_ops) {
        .save_child_custody = save_child_custody,
        .note_result_bundle_forwarded = note_result_bundle_forwarded,
        .send_result_grant = send_result_grant,
        .note_tx_sent = note_tx_sent,
        .ctx = ctx,
    };
}

static void init_ctx(struct test_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static struct mesh_relay_result make_result_grant_action(void)
{
    return (struct mesh_relay_result) {
        .actions = MESH_RELAY_ACTION_SEND_RESULT_GRANT,
        .result_grant = {
            .packet = {
                .msg_type = MSG_RESULT_GRANT,
                .src_id = 0x1111222233334444ull,
                .dst_id = 0x5555666677778888ull,
                .session_id = 41u,
                .seq = 7u,
                .ttl = 1u,
            },
            .next_hop_id = 0x5555666677778888ull,
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        },
    };
}

ZTEST(mesh_result_handoff,
      test_result_grant_saves_child_custody_before_accepted_c5_send)
{
    struct mesh_relay_result result = make_result_grant_action();
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = 0;
    ctx.send_ret = 0;
    app_mesh_result_handoff_result_grant(&result, true, &ops, &status);

    zassert_true(status.child_custody_ready);
    zassert_true(status.child_custody_saved);
    zassert_true(status.result_grant_sent);
    zassert_false(status.result_grant_suppressed);
    zassert_equal(ctx.event_count, 3u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
    zassert_equal(ctx.events[1], EVENT_SEND_GRANT);
    zassert_equal(ctx.events[2], EVENT_NOTE_TX_SENT);
    zassert_true(ctx.sent_grant == &result.result_grant);
    zassert_true(ctx.noted_tx == &result.result_grant);
}

ZTEST(mesh_result_handoff,
      test_result_grant_save_failure_suppresses_grant_send)
{
    struct mesh_relay_result result = make_result_grant_action();
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = -EIO;
    ctx.send_ret = 0;
    app_mesh_result_handoff_result_grant(&result, true, &ops, &status);

    zassert_false(status.child_custody_ready);
    zassert_true(status.child_custody_save_failed);
    zassert_true(status.result_grant_suppressed);
    zassert_false(status.result_grant_sent);
    zassert_equal(status.save_ret, -EIO);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
    zassert_is_null(ctx.sent_grant);
    zassert_is_null(ctx.noted_tx);
}

ZTEST(mesh_result_handoff,
      test_result_grant_send_failure_keeps_saved_custody_without_tx_note)
{
    struct mesh_relay_result result = make_result_grant_action();
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = 0;
    ctx.send_ret = -EIO;
    app_mesh_result_handoff_result_grant(&result, true, &ops, &status);

    zassert_true(status.child_custody_ready);
    zassert_true(status.child_custody_saved);
    zassert_false(status.result_grant_sent);
    zassert_false(status.result_grant_suppressed);
    zassert_equal(status.save_ret, 0);
    zassert_equal(status.send_ret, -EIO);
    zassert_equal(ctx.event_count, 2u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
    zassert_equal(ctx.events[1], EVENT_SEND_GRANT);
    zassert_true(ctx.sent_grant == &result.result_grant);
    zassert_is_null(ctx.noted_tx);
}

ZTEST(mesh_result_handoff,
      test_forwarded_child_result_updates_child_custody_after_handoff)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_FORWARD,
        .forward = {
            .packet = {
                .msg_type = MSG_COMMAND_RESULT,
                .src_id = 0x5555666677778888ull,
                .dst_id = 0x9999888877776666ull,
                .session_id = 41u,
                .seq = 8u,
                .ttl = MESH_DEFAULT_TTL,
            },
            .next_hop_id = 0x9999888877776666ull,
            .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        },
    };
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = 0;
    app_mesh_result_handoff_after_forward(&result, true, true, &ops, &status);

    zassert_true(status.child_custody_ready);
    zassert_true(status.child_custody_saved);
    zassert_false(status.result_bundle_forward_noted);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
    zassert_is_null(ctx.noted_bundle);
}

ZTEST(mesh_result_handoff,
      test_forwarded_result_bundle_notes_bundle_then_saves_child_custody)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_FORWARD,
        .forward = {
            .packet = {
                .msg_type = MSG_RESULT_BUNDLE,
                .src_id = 0x5555666677778888ull,
                .dst_id = 0x9999888877776666ull,
                .session_id = 41u,
                .seq = 9u,
                .ttl = MESH_DEFAULT_TTL,
            },
            .next_hop_id = 0x9999888877776666ull,
            .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        },
    };
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = 0;
    app_mesh_result_handoff_after_forward(&result, true, true, &ops, &status);

    zassert_true(status.child_custody_ready);
    zassert_true(status.child_custody_saved);
    zassert_true(status.result_bundle_forward_noted);
    zassert_equal(ctx.event_count, 2u);
    zassert_equal(ctx.events[0], EVENT_NOTE_BUNDLE);
    zassert_equal(ctx.events[1], EVENT_SAVE);
    zassert_true(ctx.noted_bundle == &result.forward);
}

ZTEST(mesh_result_handoff,
      test_forward_handoff_save_failure_reports_child_custody_not_ready)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_FORWARD,
        .forward = {
            .packet = {
                .msg_type = MSG_COMMAND_RESULT,
                .src_id = 0x5555666677778888ull,
                .dst_id = 0x9999888877776666ull,
                .session_id = 41u,
                .seq = 10u,
                .ttl = MESH_DEFAULT_TTL,
            },
            .next_hop_id = 0x9999888877776666ull,
            .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        },
    };
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = -EIO;
    app_mesh_result_handoff_after_forward(&result, true, true, &ops, &status);

    zassert_false(status.child_custody_ready);
    zassert_false(status.child_custody_saved);
    zassert_true(status.child_custody_save_failed);
    zassert_equal(status.save_ret, -EIO);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
}

ZTEST(mesh_result_handoff,
      test_hop_ack_requires_saved_child_custody_for_accepted_result)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK |
                   MESH_RELAY_ACTION_CUSTODY_ACCEPTED,
    };
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = 0;
    app_mesh_result_handoff_prepare_hop_ack(&result,
                                            false,
                                            true,
                                            &ops,
                                            &status);

    zassert_true(status.child_custody_ready);
    zassert_true(status.child_custody_saved);
    zassert_true(status.hop_ack_allowed);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
}

ZTEST(mesh_result_handoff,
      test_hop_ack_save_failure_suppresses_custody_ack)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK |
                   MESH_RELAY_ACTION_CUSTODY_ACCEPTED,
    };
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    ctx.save_ret = -EIO;
    app_mesh_result_handoff_prepare_hop_ack(&result,
                                            false,
                                            true,
                                            &ops,
                                            &status);

    zassert_false(status.child_custody_ready);
    zassert_true(status.child_custody_save_failed);
    zassert_false(status.hop_ack_allowed);
    zassert_equal(status.save_ret, -EIO);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_SAVE);
}

ZTEST(mesh_result_handoff,
      test_forwarded_packet_hop_ack_allowed_after_forward_send)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK |
                   MESH_RELAY_ACTION_FORWARD,
    };
    struct test_ctx ctx;
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    init_ctx(&ctx);
    app_mesh_result_handoff_prepare_hop_ack(&result,
                                            true,
                                            true,
                                            &ops,
                                            &status);

    zassert_true(status.child_custody_ready);
    zassert_false(status.child_custody_saved);
    zassert_true(status.hop_ack_allowed);
    zassert_equal(ctx.event_count, 0u);
}

ZTEST_SUITE(mesh_result_handoff, NULL, NULL, NULL, NULL, NULL);
