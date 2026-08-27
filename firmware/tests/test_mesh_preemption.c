#include "gateway_command.h"
#include "mesh.h"
#include "mesh_preemption.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "route.h"

#include <assert.h>
#include <string.h>

#define ANCHOR_A 0xA001u
#define RELAY_ANCHOR 0xA002u
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

static struct route_candidate relay_gateway_route(uint32_t route_epoch)
{
    struct route_candidate route = direct_gateway_route(route_epoch);

    route.next_hop_id = RELAY_ANCHOR;
    route.hop_count = 2u;
    route.route_cost = 210u;
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
    assert(plan.defer_active_tx);
    assert(plan.schedule_timeout);
    assert(!plan.transfer_local_click);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
    assert(mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x11111111)));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
}

static void test_click_preemption_ignores_disposable_heartbeat(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(7u);
    struct proto_packet packet = {
        .msg_type = MSG_ANCHOR_HEARTBEAT,
        .flags = 0u,
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
    assert(!plan.defer_active_tx);
    assert(!plan.schedule_timeout);
    assert(!plan.transfer_local_click);
    assert(!mesh_relay_tx_active(&relay));
    assert(!mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x22222222)));
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
        .message_age_ms = 42u,
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
    assert(plan.transfer_local_click);
    assert(plan.click_report.packet.msg_type == MSG_CLICK_REPORT);
    assert(plan.click_report.packet.src_id == ANCHOR_A);
    assert(plan.click_report.payload_len == sizeof(payload));
    assert(memcmp(plan.click_report.payload, payload, sizeof(payload)) == 0);
    assert(plan.click_report.radio_channel == relay.pending.radio_channel);
    assert(plan.click_report.next_hop_id == relay.pending.next_hop_id);
    assert(plan.click_report.packet.message_age_ms == 142u);
    assert(plan.click_report.queued_at_ms == 5100u);
    assert(plan.click_report.earliest_tx_ms == 5100u);
    assert(plan.click_report.queued_at_valid);
    assert(plan.click_report.earliest_tx_valid);
    assert(!plan.defer_active_tx);
    assert(!plan.schedule_timeout);
    assert(mesh_relay_tx_active(&relay));
}

static void test_click_preemption_retains_local_multihop_hop_ack_owner(void)
{
    struct mesh_relay relay;
    struct route_candidate route = relay_gateway_route(7u);
    struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 23u,
        .seq = 24u,
        .ttl = MESH_GATEWAY_ACK_TTL,
        .payload_len = 4u,
    };
    struct mesh_outbound tx;
    struct mesh_click_preempt_plan plan;
    uint8_t payload[] = {0xc1u, 0x1cu, 0xacu, 0x4bu};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               sizeof(payload),
                               5000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == RELAY_ANCHOR);
    tx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    relay.pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    mesh_relay_note_tx_sent(&relay, &tx, 5001u);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) ==
           PROTO_OK);
    assert(!plan.transfer_local_click);
    assert(!plan.defer_active_tx);
    assert(!plan.schedule_timeout);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.packet.session_id == packet.session_id);
    assert(relay.pending.packet.seq == packet.seq);
    assert(relay.pending.next_hop_id == RELAY_ANCHOR);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    /* Keeping the active owner preserves its exact RX predicate while the
     * click coordinator temporarily suppresses radio work. */
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
    assert(plan.defer_active_tx);
    assert(plan.schedule_timeout);
    assert(!plan.transfer_local_click);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.msg_type == MSG_CLICK_REPORT);
    assert(relay.pending.packet.src_id == TRANSMITTER);
    assert(relay.pending.payload_len == sizeof(payload));
    assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);

    /* Click preemption pauses this same owner instead of copying it away. */
    assert(mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x33333333)));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
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

static void start_pending_click(struct mesh_relay *relay,
                                uint64_t source_id,
                                uint16_t sequence,
                                uint32_t now_ms)
{
    const struct route_candidate route = direct_gateway_route(7u);
    const struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = source_id,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x77000000) + sequence,
        .seq = sequence,
        .ttl = MESH_GATEWAY_ACK_TTL,
        .payload_len = 4u,
    };
    const uint8_t payload[] = {0x41u, 0x42u, 0x43u, 0x44u};
    struct mesh_outbound tx;

    assert(relay != NULL);
    mesh_relay_init(relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(route_upsert_candidate(&relay->upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(relay,
                               &packet,
                               payload,
                               sizeof(payload),
                               now_ms,
                               &tx) == PROTO_OK);
}

static void test_click_preemption_preserves_forbidden_local_custody(void)
{
    struct mesh_relay relay;
    struct mesh_click_preempt_plan plan;

    start_pending_click(&relay, ANCHOR_A, 40u, 5000u);
    relay.pending.gateway_ack_confirm_pending = true;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) ==
           PROTO_OK);
    assert(!plan.transfer_local_click);
    assert(!plan.defer_active_tx);
    assert(relay.pending.gateway_ack_confirm_pending);

    start_pending_click(&relay, ANCHOR_A, 41u, 5000u);
    relay.pending.gateway_ack_recovery_flags = 1u;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) ==
           PROTO_OK);
    assert(!plan.transfer_local_click);
    assert(!plan.defer_active_tx);
    assert(relay.pending.gateway_ack_recovery_flags == 1u);

    start_pending_click(&relay, ANCHOR_A, 42u, 5000u);
    relay.pending.gateway_ack_forward_pending = true;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) ==
           PROTO_OK);
    assert(!plan.transfer_local_click);
    assert(!plan.defer_active_tx);
    assert(relay.pending.gateway_ack_forward_pending);
}

static void test_click_preemption_refuses_expired_relay_custody(void)
{
    struct mesh_relay relay;
    struct mesh_click_preempt_plan plan;
    uint32_t expiry_ms;

    start_pending_click(&relay, ANCHOR_A, 43u, 5000u);
    expiry_ms = mesh_relay_outbox_expiry_s_for_packet(
        &relay.pending.packet, relay.pending.payload, relay.pending.payload_len) *
        1000u;
    relay.pending.packet.message_age_ms = expiry_ms;
    relay.pending.queued_at_ms = 5100u;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) ==
           PROTO_OK);
    assert(!plan.transfer_local_click);
    assert(!plan.defer_active_tx);
    assert(!mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x44444444)));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.seq == 43u);

    start_pending_click(&relay, TRANSMITTER, 44u, 5000u);
    expiry_ms = mesh_relay_outbox_expiry_s_for_packet(
        &relay.pending.packet, relay.pending.payload, relay.pending.payload_len) *
        1000u;
    relay.pending.packet.message_age_ms = expiry_ms;
    relay.pending.queued_at_ms = 5100u;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_A, 5100u, &plan) ==
           PROTO_OK);
    assert(!plan.transfer_local_click);
    assert(!plan.defer_active_tx);
    assert(!mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x55555555)));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.seq == 44u);
}

static void test_relay_deferral_refuses_terminal_and_forward_custody(void)
{
    struct mesh_relay relay;
    uint16_t original_sequence;

    start_pending_click(&relay, TRANSMITTER, 45u, 5000u);
    relay.pending.state = MESH_RELAY_TX_WAIT_TERMINAL_COMMIT;
    original_sequence = relay.pending.packet.seq;
    assert(!mesh_relay_can_defer_tx(&relay));
    assert(!mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x66666666)));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(relay.pending.packet.seq == original_sequence);

    start_pending_click(&relay, TRANSMITTER, 46u, 5000u);
    relay.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    relay.pending.gateway_ack_forward_pending = true;
    original_sequence = relay.pending.packet.seq;
    assert(!mesh_relay_can_defer_tx(&relay));
    assert(!mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x77777777)));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD);
    assert(relay.pending.gateway_ack_forward_pending);
    assert(relay.pending.packet.seq == original_sequence);
}

static void test_relay_deferral_preserves_existing_backoff(void)
{
    struct mesh_relay relay;
    uint32_t retry_after_ms;
    uint32_t message_age_ms;
    uint32_t queued_at_ms;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;

    start_pending_click(&relay, TRANSMITTER, 47u, 5000u);
    relay.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay.pending.retry_after_ms = 9000u;
    relay.pending.packet.message_age_ms = 55u;
    relay.pending.queued_at_ms = 5050u;
    retry_after_ms = relay.pending.retry_after_ms;
    message_age_ms = relay.pending.packet.message_age_ms;
    queued_at_ms = relay.pending.queued_at_ms;
    payload_len = relay.pending.payload_len;
    memcpy(payload, relay.pending.payload, payload_len);

    assert(mesh_relay_can_defer_tx(&relay));
    assert(mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x88888888)));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == retry_after_ms);
    assert(relay.pending.packet.message_age_ms == message_age_ms);
    assert(relay.pending.queued_at_ms == queued_at_ms);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
}

int main(void)
{
    test_click_preemption_defers_collection_result();
    test_click_preemption_ignores_disposable_heartbeat();
    test_click_preemption_requeues_local_click_report();
    test_click_preemption_retains_local_multihop_hop_ack_owner();
    test_click_preemption_retains_transit_click_report();
    test_click_preemption_preserves_forbidden_local_custody();
    test_click_preemption_refuses_expired_relay_custody();
    test_relay_deferral_refuses_terminal_and_forward_custody();
    test_relay_deferral_preserves_existing_backoff();
    return 0;
}
