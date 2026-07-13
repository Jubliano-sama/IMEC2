#include "app_mesh_c5_priority.h"
#include "app_mesh_route_request_policy.h"
#include "mesh_sim.h"

#include "mesh_relay.h"
#include "protocol.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TRANSMITTER_ID UINT64_C(0x3333333333333301)
#define ANCHOR_ID UINT64_C(0x3333333333333302)
#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define ROUTE_EPOCH UINT32_C(41)
#define RX_GUARD_US UINT64_C(100)
#define DIRECT_PROBE_START_US UINT64_C(10000)
#define ROUTE_REQUEST_START_MS UINT32_C(40)
#define ROUTE_WAKE_START_US UINT64_C(20000)
#define DATA_COUNT 3u

#define CHECK(expression) do {                                               \
    if (!(expression)) {                                                     \
        fprintf(stderr, "forcedhop phase=%s line=%d assertion=%s\n",       \
                phase, __LINE__, #expression);                               \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char *phase = "setup";

static uint64_t max_u64(uint64_t first, uint64_t second)
{
    return first > second ? first : second;
}

static uint64_t transmission_evaluation_us(const struct mesh_sim_world *world,
                                           uint16_t transmission_index)
{
    const struct mesh_sim_transmission *tx =
        &world->transmissions[transmission_index];
    uint16_t propagation_us = 0u;

    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->reachable[tx->node_index][i] &&
            world->propagation_us[tx->node_index][i] > propagation_us) {
            propagation_us = world->propagation_us[tx->node_index][i];
        }
    }
    return tx->end_us + propagation_us;
}

static int receive_transmission(struct mesh_sim_world *world,
                                uint16_t transmission_index,
                                uint8_t receiver)
{
    const struct mesh_sim_transmission *transmission;
    enum mesh_sim_phy phy;
    uint64_t arrival_start_us;
    uint64_t arrival_end_us;
    uint16_t rx_window;
    uint8_t channel;
    int ret;

    transmission = &world->transmissions[transmission_index];
    ret = mesh_sim_outbound_radio(&transmission->outbound, &channel, &phy);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    arrival_start_us = transmission->start_us +
                       world->propagation_us[transmission->node_index][receiver];
    arrival_end_us = transmission->end_us +
                     world->propagation_us[transmission->node_index][receiver];
    ret = mesh_sim_schedule_rx(world, receiver,
                               arrival_start_us - RX_GUARD_US,
                               arrival_end_us + RX_GUARD_US,
                               channel, phy, &rx_window);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_run_until(world,
                              transmission_evaluation_us(world,
                                                         transmission_index));
}

static int transmit_outbound(struct mesh_sim_world *world,
                             uint8_t sender,
                             uint8_t receiver,
                             const struct mesh_outbound *outbound,
                             uint64_t requested_start_us,
                             uint16_t *transmission_index)
{
    uint64_t start_us;
    int ret;

    start_us = max_u64(requested_start_us, world->now_us + RX_GUARD_US + 1u);
    start_us = max_u64(start_us, (uint64_t)outbound->earliest_tx_ms * 1000u);
    ret = mesh_sim_schedule_outbound_tx(world, sender, start_us, outbound,
                                        transmission_index);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr,
                "transmit schedule ret=%d last=%d msg=0x%02x sender=%u receiver=%u now=%llu start=%llu\n",
                ret, world->last_error, outbound->packet.msg_type,
                sender, receiver, (unsigned long long)world->now_us,
                (unsigned long long)start_us);
        return ret;
    }
    return receive_transmission(world, *transmission_index, receiver);
}

static void make_direct_probe(struct mesh_outbound *probe, uint64_t source_id)
{
    memset(probe, 0, sizeof(*probe));
    probe->packet.msg_type = MSG_GATEWAY_ROUTE_REQ;
    probe->packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    probe->packet.src_id = source_id;
    probe->packet.dst_id = GATEWAY_ID;
    probe->packet.session_id = 1u;
    probe->packet.seq = 1u;
    probe->packet.ttl = MESH_DEFAULT_TTL;
    probe->next_hop_id = GATEWAY_ID;
    probe->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
}

static int transmit_route_wake_contact(struct mesh_sim_world *world,
                                       uint8_t transmitter,
                                       uint8_t anchor)
{
    const struct uwb_wake_claim_frame claim = {
        .network_id = UINT32_C(0x10203040),
        .clicker_id = TRANSMITTER_ID,
        .click_event_id = 1u,
        .attempt_index = 1u,
        .priority_id = TRANSMITTER_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 5u,
        .discovery_starts_in_ms = 5u,
        .claimed_duration_ms = 10u,
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .nonce = UINT64_C(0x0102030405060708),
        .flags = FLAG_ROUTE_SETUP | FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY,
    };
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    uint64_t end_us;
    uint16_t tx_index;
    size_t frame_len = 0u;
    int ret;

    ret = uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_sim_schedule_raw_tx(world, transmitter, ROUTE_WAKE_START_US,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   frame, frame_len, false, &tx_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    end_us = world->transmissions[tx_index].end_us +
             world->propagation_us[transmitter][anchor];
    ret = mesh_sim_schedule_rx(world, anchor, ROUTE_WAKE_START_US - RX_GUARD_US,
                               end_us + RX_GUARD_US,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE, NULL);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_run_until(world, end_us);
}

static bool has_timing_for_peer(const struct mesh_relay *relay, uint64_t peer_id)
{
    for (size_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static bool take_queued_message(struct mesh_sim_role_instance *node,
                                uint8_t msg_type,
                                struct mesh_outbound *outbound)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            node->tx_queue[i].outbound.packet.msg_type == msg_type) {
            *outbound = node->tx_queue[i].outbound;
            memset(&node->tx_queue[i], 0, sizeof(node->tx_queue[i]));
            node->tx_queue_count--;
            return true;
        }
    }
    return false;
}

static size_t count_decoded_receptions(const struct mesh_sim_world *world,
                                       uint64_t receiver_id,
                                       uint8_t msg_type)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        if (world->receptions[i].receiver_id == receiver_id &&
            world->receptions[i].outcome == MESH_SIM_RX_DECODED &&
            world->receptions[i].packet.msg_type == msg_type) {
            count++;
        }
    }
    return count;
}

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    const struct mesh_event_params params = {
        .first_event_time_ms = first_event_ms,
        .event_interval_ms = 250u,
        .event_window_ms = 120u,
        .guard_ms = 30u,
        .supervision_timeout_ms = 5000u,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = 3u,
    };

    return params;
}

static int run_connection_action(struct mesh_sim_world *world,
                                 uint16_t connection,
                                 bool receiver_preempted)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world, connection, &action);

    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "connection next ret=%d last=%d now=%llu\n",
                ret, world->last_error,
                (unsigned long long)world->now_us);
        return ret;
    }
    if (!action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(world, connection,
                                                      receiver_preempted);
        if (ret != MESH_SIM_OK) {
            fprintf(stderr,
                    "connection schedule ret=%d last=%d kind=%d now=%llu start=%llu end=%llu skipped=%u\n",
                    ret, world->last_error, action.kind,
                    (unsigned long long)world->now_us,
                    (unsigned long long)action.start_us,
                    (unsigned long long)action.end_us,
                    action.skipped_events);
            return ret;
        }
    }
    ret = mesh_sim_run_until(world, action.end_us);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "connection run ret=%d last=%d kind=%d end=%llu\n",
                ret, world->last_error, action.kind,
                (unsigned long long)action.end_us);
    }
    return ret;
}

static int run_forcedhop_connection_scenario(void)
{
    static struct mesh_sim_world world;
    struct app_mesh_route_request_policy_state policy_state = {
        .relay_required = true,
        .direct_probe_ret = 0,
    };
    struct app_mesh_route_request_policy_decision policy;
    struct mesh_outbound direct_probe;
    struct mesh_outbound gateway_reply;
    struct mesh_outbound route_request;
    struct mesh_event_params params;
    const struct uwb_anchor_config anchor_config = {
        .network_id = UINT32_C(0x10203040),
        .anchor_id = ANCHOR_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct proto_packet packet = {0};
    const struct route_candidate *selected;
    uint64_t next_hop_id = 0u;
    uint16_t probe_tx;
    uint16_t duplicate_reply_tx;
    uint16_t route_request_tx;
    uint16_t route_reply_tx;
    uint16_t connection;
    uint8_t transmitter;
    uint8_t anchor;
    uint8_t gateway;
    size_t transmission_count;
    size_t gateway_ack_receptions;

    mesh_sim_init(&world, UINT32_C(0xf09ced11));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                            TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &transmitter) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ANCHOR_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &anchor) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, transmitter, anchor, 98u, 5u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, anchor, gateway, 98u, 5u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, transmitter, gateway, 98u, 5u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_route_request_flags(
              &world, transmitter, MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_init_anchor_session(&world, anchor, &anchor_config) == PROTO_OK);

    phase = "forced_direct_probe_contact_only";
    app_mesh_route_request_policy_decide(&policy_state, &policy);
    CHECK(!policy.install_direct_route_from_probe);
    CHECK(!policy.direct_probe_satisfies_request);
    CHECK(policy.route_request_flags == MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED);
    make_direct_probe(&direct_probe, TRANSMITTER_ID);
    CHECK(transmit_outbound(&world, transmitter, gateway, &direct_probe,
                            DIRECT_PROBE_START_US, &probe_tx) == MESH_SIM_OK);
    CHECK(world.receptions[world.reception_count - 1u].outcome ==
          MESH_SIM_RX_DECODED);
    CHECK(take_queued_message(&world.roles[gateway], MSG_GATEWAY_ACK,
                              &gateway_reply));
    gateway_reply.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(gateway_reply.packet.src_id == GATEWAY_ID);
    CHECK(gateway_reply.packet.dst_id == TRANSMITTER_ID);
    gateway_ack_receptions = count_decoded_receptions(
        &world, TRANSMITTER_ID, MSG_GATEWAY_ACK);
    CHECK(transmit_outbound(&world, gateway, transmitter, &gateway_reply,
                            world.now_us + 1000u, &duplicate_reply_tx) ==
          MESH_SIM_OK);
    CHECK(count_decoded_receptions(&world, TRANSMITTER_ID, MSG_GATEWAY_ACK) ==
          gateway_ack_receptions + 1u);
    CHECK(mesh_relay_select_next_hop(&world.roles[transmitter].relay,
                                     GATEWAY_ID, &next_hop_id) ==
          PROTO_ERR_NOT_FOUND);
    CHECK(route_selected(&world.roles[transmitter].relay.upstream) == NULL);
    CHECK(!has_timing_for_peer(&world.roles[transmitter].relay, GATEWAY_ID));
    CHECK(world.connection_count == 0u);

    phase = "forced_duplicate_gateway_reply_contact_only";
    gateway_ack_receptions = count_decoded_receptions(
        &world, TRANSMITTER_ID, MSG_GATEWAY_ACK);
    CHECK(transmit_outbound(
              &world, gateway, transmitter, &gateway_reply,
              world.now_us + 1000u, &duplicate_reply_tx) == MESH_SIM_OK);
    CHECK(world.receptions[world.reception_count - 1u].outcome ==
          MESH_SIM_RX_DECODED);
    CHECK(count_decoded_receptions(&world, TRANSMITTER_ID, MSG_GATEWAY_ACK) ==
          gateway_ack_receptions + 1u);
    CHECK(mesh_relay_select_next_hop(&world.roles[transmitter].relay,
                                     GATEWAY_ID, &next_hop_id) ==
          PROTO_ERR_NOT_FOUND);
    CHECK(route_selected(&world.roles[transmitter].relay.upstream) == NULL);
    CHECK(!has_timing_for_peer(&world.roles[transmitter].relay, GATEWAY_ID));
    CHECK(world.connection_count == 0u);

    phase = "anchor_direct_probe_route";
    make_direct_probe(&direct_probe, ANCHOR_ID);
    direct_probe.packet.seq = 2u;
    CHECK(transmit_outbound(&world, anchor, gateway, &direct_probe,
                            world.now_us + 1000u, &probe_tx) == MESH_SIM_OK);
    CHECK(world.receptions[world.reception_count - 1u].outcome ==
          MESH_SIM_RX_DECODED);
    CHECK(mesh_relay_note_direct_gateway_route(&world.roles[anchor].relay,
                                               (uint32_t)(world.now_us / 1000u)) ==
                                               PROTO_OK);

    phase = "route_wake_contact";
    CHECK(transmit_route_wake_contact(&world, transmitter, anchor) == MESH_SIM_OK);
    CHECK(world.receptions[world.reception_count - 1u].outcome ==
          MESH_SIM_RX_DECODED);
    CHECK(world.rx_windows[world.rx_window_count - 1u].wake_claim_handoff);

    phase = "over_air_forced_route_request";
    CHECK(mesh_relay_prepare_route_request_with_timing_flags(
              &world.roles[transmitter].relay, GATEWAY_ID, NULL, 0u,
              policy.route_request_flags, 20u, ROUTE_REQUEST_START_MS, 0u,
              &route_request) == PROTO_OK);
    CHECK(route_request.packet.ttl == 1u);
    CHECK(app_mesh_route_request_defer_delay_ms(
              route_request.earliest_tx_ms + 1u,
              route_request.earliest_tx_ms) == 0u);
    transmission_count = world.transmission_count;
    CHECK(transmit_outbound(&world, transmitter, anchor, &route_request,
                            (uint64_t)ROUTE_REQUEST_START_MS * 1000u,
                            &route_request_tx) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count + 2u);
    route_reply_tx = (uint16_t)(transmission_count + 1u);
    CHECK(world.transmissions[route_reply_tx].has_outbound);
    CHECK(world.transmissions[route_reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[route_reply_tx].outbound.next_hop_id ==
          TRANSMITTER_ID);
    CHECK(receive_transmission(&world, route_reply_tx, transmitter) ==
          MESH_SIM_OK);
    CHECK(mesh_relay_select_next_hop(&world.roles[transmitter].relay,
                                     GATEWAY_ID, &next_hop_id) == PROTO_OK);
    CHECK(next_hop_id == ANCHOR_ID);
    selected = route_selected(&world.roles[transmitter].relay.upstream);
    CHECK(selected != NULL);
    CHECK(selected->next_hop_id == ANCHOR_ID);
    CHECK(selected->next_hop_id != GATEWAY_ID);

    phase = "over_air_propose_accept";
    CHECK(app_mesh_c5_route_capture_requires_post_rx_response(
              MSG_MESH_EVENT_PROPOSE));
    CHECK(app_mesh_c5_route_capture_requires_inline_timing_install(
              MSG_MESH_EVENT_ACCEPT, true));
    params = connection_params((uint32_t)(world.now_us / 1000u) + 5000u);
    CHECK(mesh_sim_add_connection_over_radio(&world, transmitter, anchor,
                                             &params, true, &connection) ==
          MESH_SIM_OK);
    CHECK(run_connection_action(&world, connection, false) == MESH_SIM_OK);
    CHECK(world.connections[connection].repair_propose_decoded);
    CHECK(world.connections[connection].repair_accept_decoded);
    CHECK(!world.connections[connection].repair_pending);
    CHECK(world.connections[connection].completed_repairs == 1u);

    phase = "repeated_channel9_data";
    for (uint16_t seq = 1u; seq <= DATA_COUNT; seq++) {
        packet.msg_type = MSG_MESH_DATA;
        packet.src_id = TRANSMITTER_ID;
        packet.dst_id = GATEWAY_ID;
        packet.session_id = 7u;
        packet.seq = seq;
        packet.ttl = MESH_DEFAULT_TTL;
        CHECK(mesh_sim_queue_originated(&world, transmitter, &packet,
                                        NULL, 0u) == MESH_SIM_OK);
        packet.src_id = ANCHOR_ID;
        packet.dst_id = TRANSMITTER_ID;
        packet.session_id = 8u;
        CHECK(mesh_sim_queue_originated(&world, anchor, &packet,
                                        NULL, 0u) == MESH_SIM_OK);
    }
    for (size_t event = 0u; event < 6u; event++) {
        CHECK(run_connection_action(&world, connection, false) == MESH_SIM_OK);
    }
    CHECK(world.connections[connection].completed_events >= 6u);
    CHECK(world.roles[anchor].decoded_frames >= DATA_COUNT);
    CHECK(world.connections[connection].timing_a.timing_fresh);
    CHECK(world.connections[connection].timing_b.timing_fresh);

    phase = "click_shaped_preemption_recovery";
    CHECK(run_connection_action(&world, connection, true) == MESH_SIM_OK);
    CHECK(world.connections[connection].completed_repairs == 1u);
    CHECK(!world.connections[connection].repair_pending);
    for (uint16_t seq = 10u; seq < 12u; seq++) {
        packet.src_id = TRANSMITTER_ID;
        packet.dst_id = GATEWAY_ID;
        packet.session_id = 9u;
        packet.seq = seq;
        CHECK(mesh_sim_queue_originated(&world, transmitter, &packet,
                                        NULL, 0u) == MESH_SIM_OK);
        packet.src_id = ANCHOR_ID;
        packet.dst_id = TRANSMITTER_ID;
        packet.session_id = 10u;
        CHECK(mesh_sim_queue_originated(&world, anchor, &packet,
                                        NULL, 0u) == MESH_SIM_OK);
    }
    for (size_t event = 0u; event < 4u; event++) {
        CHECK(run_connection_action(&world, connection, false) == MESH_SIM_OK);
    }
    CHECK(world.connections[connection].completed_repairs == 1u);
    CHECK(world.connections[connection].timing_a.timing_fresh);
    CHECK(world.connections[connection].timing_b.timing_fresh);
    CHECK(mesh_relay_select_next_hop(&world.roles[transmitter].relay,
                                     GATEWAY_ID, &next_hop_id) == PROTO_OK);
    CHECK(next_hop_id == ANCHOR_ID);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

int main(void)
{
    if (run_forcedhop_connection_scenario() != 0) {
        return 1;
    }
    printf("forced-hop over-air route and stable connection scenario passed\n");
    return 0;
}
