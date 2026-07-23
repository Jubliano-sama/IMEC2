#include "gateway_command.h"
#include "mesh.h"
#include "mesh_preemption.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "route.h"

#include <assert.h>
#include <string.h>

#define ANCHOR_A 0xA001u
#define TRANSMITTER 0xB001u
#define GATEWAY 0x9000u

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

static void test_click_preemption_defers_collection_result(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet;
    struct mesh_outbound tx;
    struct mesh_click_preempt_plan plan;
    uint8_t payload[96];
    size_t payload_len;

    build_collection_payload(payload, sizeof(payload), &payload_len);
    assert(mesh_init_command_result(&packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    0x10203040u,
                                    3u,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               payload_len,
                               5000u,
                               &tx) == PROTO_OK);

    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) == PROTO_OK);
    assert(plan.save_outbox);
    assert(plan.schedule_timeout);
    assert(!plan.clear_outbox);
    assert(!plan.cancel_timeout);
    assert(!plan.requeue_click_report);
    assert(plan.cancel_active_tx);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
}

static void test_click_preemption_retains_non_deferrable_tx(void)
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
    struct mesh_click_preempt_plan plan;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay, &packet, NULL, 0u, 5000u, &tx) == PROTO_OK);

    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) == PROTO_OK);
    assert(!plan.save_outbox);
    assert(!plan.schedule_timeout);
    assert(!plan.clear_outbox);
    assert(!plan.cancel_timeout);
    assert(!plan.cancel_active_tx);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.packet.msg_type == MSG_ANCHOR_HEARTBEAT);
    assert(relay.pending.packet.src_id == ANCHOR_A);
    assert(relay.pending.payload_len == 0u);
}

static void test_click_preemption_requeues_local_click_report(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 21u,
        .seq = 22u,
        .ttl = MESH_GATEWAY_ACK_TTL,
        .payload_len = 4u,
    };
    struct mesh_outbound tx;
    struct mesh_click_preempt_plan plan;
    uint8_t payload[] = {0xdeu, 0xadu, 0xbeu, 0xefu};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               sizeof(payload),
                               5000u,
                               &tx) == PROTO_OK);

    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) == PROTO_OK);
    assert(plan.requeue_click_report);
    assert(plan.click_report.packet.msg_type == MSG_CLICK_REPORT);
    assert(plan.click_report.packet.src_id == ANCHOR_A);
    assert(plan.click_report.payload_len == sizeof(payload));
    assert(memcmp(plan.click_report.payload, payload, sizeof(payload)) == 0);
    assert(plan.clear_outbox);
    assert(plan.cancel_timeout);
    assert(plan.cancel_active_tx);
    assert(mesh_relay_tx_active(&relay));
}

static void test_click_preemption_retains_transit_click_report(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = TRANSMITTER,
        .dst_id = GATEWAY,
        .session_id = 31u,
        .seq = 32u,
        .ttl = MESH_GATEWAY_ACK_TTL,
        .payload_len = 4u,
    };
    struct mesh_outbound tx;
    struct mesh_click_preempt_plan plan;
    struct mesh_relay_outbox_snapshot snapshot;
    struct mesh_relay_result result;
    uint8_t payload[] = {0xc0u, 0xffu, 0xeeu, 0x00u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               sizeof(payload),
                               5000u,
                               &tx) == PROTO_OK);
    relay.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) == PROTO_OK);
    assert(plan.save_outbox);
    assert(plan.schedule_timeout);
    assert(!plan.requeue_click_report);
    assert(!plan.clear_outbox);
    assert(!plan.cancel_timeout);
    assert(plan.cancel_active_tx);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.msg_type == MSG_CLICK_REPORT);
    assert(relay.pending.packet.src_id == TRANSMITTER);
    assert(relay.pending.payload_len == sizeof(payload));
    assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);

    /* Model the app's atomic save-before-cancel preemption commit. */
    assert(mesh_relay_export_outbox_snapshot(&relay, 5100u, &snapshot) ==
           PROTO_OK);
    mesh_relay_cancel_tx(&relay);
    assert(!mesh_relay_tx_active(&relay));
    assert(mesh_relay_restore_outbox_snapshot(&relay,
                                              &snapshot,
                                              5200u) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.packet.msg_type == MSG_CLICK_REPORT);
    assert(relay.pending.packet.src_id == TRANSMITTER);
    assert(relay.pending.packet.session_id == packet.session_id);
    assert(relay.pending.packet.seq == packet.seq);
    assert(relay.pending.payload_len == sizeof(payload));
    assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);

    assert(mesh_relay_tick(&relay,
                           relay.pending.retry_after_ms,
                           &result) == PROTO_OK);
    assert((result.actions & MESH_RELAY_ACTION_RETRANSMIT) != 0u);
    assert(result.retransmit.packet.src_id == TRANSMITTER);
    assert(result.retransmit.packet.session_id == packet.session_id);
    assert(result.retransmit.packet.seq == packet.seq);
    assert(result.retransmit.payload_len == sizeof(payload));
    assert(memcmp(result.retransmit.payload, payload, sizeof(payload)) == 0);
}

int main(void)
{
    test_click_preemption_defers_collection_result();
    test_click_preemption_retains_non_deferrable_tx();
    test_click_preemption_requeues_local_click_report();
    test_click_preemption_retains_transit_click_report();
    return 0;
}
