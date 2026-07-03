#include "app_mesh_collection_deferral.h"
#include "gateway_command.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "route.h"

#include <assert.h>
#include <string.h>

#define ANCHOR_A 0xA001u
#define ANCHOR_B 0xA002u
#define GATEWAY 0x9000u

struct deferral_test_ctx {
    struct mesh_relay *saved_relay;
    uint32_t saved_now_ms;
    int save_ret;
    int schedule_ret;
    int save_count;
    int schedule_count;
    int save_order;
    int schedule_order;
    int next_order;
};

static int save_outbox(struct mesh_relay *relay, uint32_t now_ms, void *ctx)
{
    struct deferral_test_ctx *test = ctx;

    assert(test != NULL);
    test->saved_relay = relay;
    test->saved_now_ms = now_ms;
    test->save_count++;
    test->save_order = ++test->next_order;
    return test->save_ret;
}

static int schedule_retry(void *ctx)
{
    struct deferral_test_ctx *test = ctx;

    assert(test != NULL);
    test->schedule_count++;
    test->schedule_order = ++test->next_order;
    return test->schedule_ret;
}

static struct app_mesh_collection_deferral_ops deferral_ops(
    struct deferral_test_ctx *ctx)
{
    const struct app_mesh_collection_deferral_ops ops = {
        .save_outbox = save_outbox,
        .schedule_retry = schedule_retry,
        .ctx = ctx,
    };

    return ops;
}

static struct route_candidate direct_gateway_route(uint32_t route_epoch)
{
    struct route_candidate route = {
        .next_hop_id = GATEWAY,
        .gateway_id = GATEWAY,
        .route_epoch = route_epoch,
        .hop_count = 1u,
        .link_quality = 90u,
        .route_cost = 110u,
        .last_seen_ms = 1000u,
        .last_success_ms = 1000u,
        .valid = true,
    };

    return route;
}

static void build_collection_payload(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 7u,
        .command_seq = 0x10203040u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 2u,
        .result_seq = 3u,
    };

    assert(payload != NULL);
    assert(payload_len != NULL);
    *payload_len = 0u;
    assert(gateway_command_append_collection_result_identity(payload,
                                                            payload_cap,
                                                            payload_len,
                                                            &result_id,
                                                            0x55667788u) == PROTO_OK);
}

static void start_collection_result_tx(struct mesh_relay *relay,
                                       struct mesh_outbound *tx,
                                       uint8_t *payload,
                                       size_t payload_cap,
                                       size_t *payload_len)
{
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet;

    build_collection_payload(payload, payload_cap, payload_len);
    assert(mesh_init_command_result(&packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    0x10203040u,
                                    3u,
                                    (uint8_t)*payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay->upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(relay,
                               &packet,
                               payload,
                               *payload_len,
                               5000u,
                               tx) == PROTO_OK);
    assert(mesh_relay_tx_active_local_collection_result(relay));
}

static void test_collection_result_defers_and_runs_hooks_in_order(void)
{
    struct mesh_relay relay;
    struct mesh_outbound tx;
    uint8_t payload[96];
    size_t payload_len;
    struct deferral_test_ctx ctx = {0};
    const struct app_mesh_collection_deferral_ops ops = deferral_ops(&ctx);
    struct app_mesh_collection_deferral_result result;

    start_collection_result_tx(&relay,
                               &tx,
                               payload,
                               sizeof(payload),
                               &payload_len);

    assert(app_mesh_collection_defer_active_result(&relay,
                                                  5100u,
                                                  &ops,
                                                  &result));
    assert(result.deferred);
    assert(result.outbox_saved);
    assert(result.retry_scheduled);
    assert(result.save_ret == 0);
    assert(result.schedule_ret == 0);
    assert(ctx.saved_relay == &relay);
    assert(ctx.saved_now_ms == 5100u);
    assert(ctx.save_count == 1);
    assert(ctx.schedule_count == 1);
    assert(ctx.save_order == 1);
    assert(ctx.schedule_order == 2);
    assert(mesh_relay_tx_active(&relay));
    assert(mesh_relay_tx_active_local_collection_result(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == 5100u + RELAY_BUSY_RETRY_MIN_MS);
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
}

static void test_collection_result_defers_even_when_snapshot_save_fails(void)
{
    struct mesh_relay relay;
    struct mesh_outbound tx;
    uint8_t payload[96];
    size_t payload_len;
    struct deferral_test_ctx ctx = {
        .save_ret = -5,
    };
    const struct app_mesh_collection_deferral_ops ops = deferral_ops(&ctx);
    struct app_mesh_collection_deferral_result result;

    start_collection_result_tx(&relay,
                               &tx,
                               payload,
                               sizeof(payload),
                               &payload_len);

    assert(app_mesh_collection_defer_active_result(&relay,
                                                  5100u,
                                                  &ops,
                                                  &result));
    assert(result.deferred);
    assert(!result.outbox_saved);
    assert(result.retry_scheduled);
    assert(result.save_ret == -5);
    assert(result.schedule_ret == 0);
    assert(ctx.save_count == 1);
    assert(ctx.schedule_count == 1);
    assert(mesh_relay_tx_active_local_collection_result(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
}

static void test_forwarded_child_result_defers_and_preserves_outbox(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet;
    struct mesh_outbound tx;
    uint8_t payload[96];
    size_t payload_len;
    struct deferral_test_ctx ctx = {0};
    const struct app_mesh_collection_deferral_ops ops = deferral_ops(&ctx);
    struct app_mesh_collection_deferral_result result;

    build_collection_payload(payload, sizeof(payload), &payload_len);
    assert(mesh_init_command_result(&packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    0x10203040u,
                                    4u,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(!mesh_relay_tx_active_local_collection_result(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.src_id == ANCHOR_A);
    assert(relay.pending.packet.dst_id == GATEWAY);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.packet_class == MSG_COMMAND_RESULT);
    assert(relay.outbox_record.payload_len == payload_len);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(app_mesh_collection_defer_active_result(&relay,
                                                  5100u,
                                                  &ops,
                                                  &result));
    assert(result.deferred);
    assert(result.outbox_saved);
    assert(result.retry_scheduled);
    assert(result.save_ret == 0);
    assert(result.schedule_ret == 0);
    assert(ctx.save_count == 1);
    assert(ctx.schedule_count == 1);
    assert(mesh_relay_tx_active(&relay));
    assert(!mesh_relay_tx_active_local_collection_result(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == 5100u + RELAY_BUSY_RETRY_MIN_MS);
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.packet.src_id == ANCHOR_A);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.packet_class == MSG_COMMAND_RESULT);
    assert(relay.outbox_record.payload_len == payload_len);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(!relay.outbox_record.gateway_acked);
}

static void test_non_collection_tx_is_left_for_caller_fallback(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet = {
        .msg_type = MSG_ANCHOR_HEARTBEAT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 11u,
        .seq = 12u,
        .ttl = MESH_GATEWAY_ACK_TTL,
        .payload_len = 0u,
    };
    struct mesh_outbound tx;
    struct deferral_test_ctx ctx = {0};
    const struct app_mesh_collection_deferral_ops ops = deferral_ops(&ctx);
    struct app_mesh_collection_deferral_result result;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay, &packet, NULL, 0u, 5000u, &tx) == PROTO_OK);

    assert(!app_mesh_collection_defer_active_result(&relay,
                                                   5100u,
                                                   &ops,
                                                   &result));
    assert(!result.deferred);
    assert(!result.outbox_saved);
    assert(!result.retry_scheduled);
    assert(ctx.save_count == 0);
    assert(ctx.schedule_count == 0);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.packet.msg_type == MSG_ANCHOR_HEARTBEAT);

    mesh_relay_cancel_tx(&relay);
    assert(!mesh_relay_tx_active(&relay));
}

int main(void)
{
    test_collection_result_defers_and_runs_hooks_in_order();
    test_collection_result_defers_even_when_snapshot_save_fails();
    test_forwarded_child_result_defers_and_preserves_outbox();
    test_non_collection_tx_is_left_for_caller_fallback();
    return 0;
}
