#include "app_mesh_result_handoff.h"

#include "protocol.h"

#include <errno.h>
#include <zephyr/ztest.h>

enum event_step {
    EVENT_SEND_GRANT = 1,
    EVENT_NOTE_TX_SENT = 2,
    EVENT_NOTE_BUNDLE = 3,
};

struct test_ctx {
    enum event_step events[4];
    uint8_t event_count;
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
        .note_result_bundle_forwarded = note_result_bundle_forwarded,
        .send_result_grant = send_result_grant,
        .note_tx_sent = note_tx_sent,
        .ctx = ctx,
    };
}

static struct mesh_relay_result make_result_grant_action(void)
{
    return (struct mesh_relay_result) {
        .actions = MESH_RELAY_ACTION_SEND_RESULT_GRANT,
        .result_grant = {
            .packet = {
                .msg_type = MSG_RESULT_GRANT,
                .src_id = UINT64_C(0x1111222233334444),
                .dst_id = UINT64_C(0x5555666677778888),
                .session_id = 41u,
                .seq = 7u,
                .ttl = 1u,
            },
            .next_hop_id = UINT64_C(0x5555666677778888),
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        },
    };
}

ZTEST(mesh_result_handoff, test_result_grant_send_then_exact_tx_note)
{
    struct mesh_relay_result result = make_result_grant_action();
    struct test_ctx ctx = {0};
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    app_mesh_result_handoff_result_grant(&result, &ops, &status);

    zassert_true(status.result_grant_sent);
    zassert_equal(status.send_ret, 0);
    zassert_equal(ctx.event_count, 2u);
    zassert_equal(ctx.events[0], EVENT_SEND_GRANT);
    zassert_equal(ctx.events[1], EVENT_NOTE_TX_SENT);
    zassert_true(ctx.sent_grant == &result.result_grant);
    zassert_true(ctx.noted_tx == &result.result_grant);
}

ZTEST(mesh_result_handoff, test_result_grant_send_failure_has_no_tx_note)
{
    struct mesh_relay_result result = make_result_grant_action();
    struct test_ctx ctx = {.send_ret = -EIO};
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    app_mesh_result_handoff_result_grant(&result, &ops, &status);

    zassert_false(status.result_grant_sent);
    zassert_equal(status.send_ret, -EIO);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_SEND_GRANT);
    zassert_is_null(ctx.noted_tx);
}

ZTEST(mesh_result_handoff, test_result_bundle_forward_notes_exact_ram_owner)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_FORWARD,
        .forward.packet.msg_type = MSG_RESULT_BUNDLE,
    };
    struct test_ctx ctx = {0};
    struct app_mesh_result_handoff_ops ops = make_ops(&ctx);
    struct app_mesh_result_handoff_status status;

    app_mesh_result_handoff_after_forward(&result, true, &ops, &status);

    zassert_true(status.result_bundle_forward_noted);
    zassert_equal(ctx.event_count, 1u);
    zassert_equal(ctx.events[0], EVENT_NOTE_BUNDLE);
    zassert_true(ctx.noted_bundle == &result.forward);
}

ZTEST(mesh_result_handoff, test_accepted_ram_custody_allows_hop_ack)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK |
                   MESH_RELAY_ACTION_CUSTODY_ACCEPTED,
    };
    struct app_mesh_result_handoff_status status;

    app_mesh_result_handoff_prepare_hop_ack(
        &result, false, NULL, &status);

    zassert_true(status.hop_ack_allowed);
}

ZTEST(mesh_result_handoff, test_forward_handoff_allows_hop_ack)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK |
                   MESH_RELAY_ACTION_FORWARD,
    };
    struct app_mesh_result_handoff_status status;

    app_mesh_result_handoff_prepare_hop_ack(
        &result, true, NULL, &status);

    zassert_true(status.hop_ack_allowed);
}

ZTEST(mesh_result_handoff, test_unowned_hop_ack_remains_suppressed)
{
    struct mesh_relay_result result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK,
    };
    struct app_mesh_result_handoff_status status;

    app_mesh_result_handoff_prepare_hop_ack(
        &result, false, NULL, &status);

    zassert_false(status.hop_ack_allowed);
}

ZTEST_SUITE(mesh_result_handoff, NULL, NULL, NULL, NULL, NULL);
