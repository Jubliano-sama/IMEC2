#include "app_mesh_c5_priority.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_route_request_policy.h"
#include "gateway_command.h"
#include "mesh_sim.h"

#include "mesh_relay.h"
#include "protocol.h"
#include "survey.h"
#include "uwb.h"

#include <errno.h>
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
#define FORCEDHOP_BURST_COUNT 4u
#define FORCEDHOP_PAYLOAD_LEN 900u
#define FORCEDHOP_BATCH_ID UINT32_C(0x54f09ced)
#define TARGETED_SURVEY_ID UINT32_C(0x07130071)
#define FORCEDHOP_BATCH_FLAG_FINAL 0x01u
#define DIRECT_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_PAYLOAD_SERVICE_US UINT64_C(50000)
#define DIRECT_ACK_SERVICE_US UINT64_C(40000)
#define LOSS_SWEEP_CASES 16u

_Static_assert(FORCEDHOP_PAYLOAD_LEN <= UWB_MESH_MAX_PAYLOAD_LEN,
               "forced-hop stress payload must fit one extended frame");

#define CHECK(expression) do {                                               \
    if (!(expression)) {                                                     \
        fprintf(stderr, "forcedhop phase=%s line=%d assertion=%s\n",       \
                phase, __LINE__, #expression);                               \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char *phase = "setup";

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

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

static size_t count_matching_receptions(const struct mesh_sim_world *world,
                                         uint64_t receiver_id,
                                         uint8_t msg_type,
                                         uint32_t session_id,
                                         uint16_t seq)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->receiver_id == receiver_id &&
            reception->outcome == MESH_SIM_RX_DECODED &&
            reception->packet.msg_type == msg_type &&
            reception->packet.session_id == session_id &&
            reception->packet.seq == seq) {
            count++;
        }
    }
    return count;
}

static const struct mesh_sim_queued_tx *queued_packet_for_peer(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint16_t seq)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &node->tx_queue[i];

        if (queued->valid && queued->outbound.next_hop_id == peer_id &&
            queued->outbound.packet.msg_type == msg_type &&
            queued->outbound.packet.session_id == session_id &&
            queued->outbound.packet.seq == seq) {
            return queued;
        }
    }
    return NULL;
}

static const struct mesh_sim_queued_tx *queued_gateway_ack_for_sender(
    const struct mesh_sim_role_instance *gateway,
    uint64_t sender_id,
    uint64_t original_source_id,
    uint32_t session_id)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &gateway->tx_queue[i];

        if (queued->valid && queued->outbound.next_hop_id == sender_id &&
            queued->outbound.packet.msg_type == MSG_GATEWAY_ACK &&
            queued->outbound.packet.dst_id == original_source_id &&
            queued->outbound.packet.session_id == session_id) {
            return queued;
        }
    }
    return NULL;
}

static int keep_connection_alive_until(struct mesh_sim_world *world,
                                       uint16_t connection,
                                       uint64_t target_us)
{
    for (size_t step = 0u; step < 64u; step++) {
        struct mesh_sim_connection_action action;

        if (world->now_us >= target_us) {
            return mesh_sim_run_until(world, world->now_us);
        }
        CHECK(mesh_sim_connection_next_action(world, connection, &action) ==
              MESH_SIM_OK);
        if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE ||
            action.start_us > target_us) {
            return mesh_sim_run_until(world, target_us);
        }
        CHECK(run_connection_action(world, connection, false) == MESH_SIM_OK);
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int schedule_origin_retry(struct mesh_sim_world *world,
                                 uint8_t transmitter,
                                 uint16_t connection)
{
    struct mesh_relay *relay = &world->roles[transmitter].relay;
    uint64_t due_us;

    CHECK(relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    due_us = (uint64_t)relay->pending.gateway_ack_deadline_ms * 1000u;
    if (due_us < world->now_us) {
        due_us = world->now_us;
    }
    CHECK(mesh_sim_schedule_relay_tick(world, transmitter, due_us) ==
          MESH_SIM_OK);
    CHECK(keep_connection_alive_until(world, connection, due_us) == MESH_SIM_OK);
    CHECK(relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);

    due_us = (uint64_t)relay->pending.retry_after_ms * 1000u;
    if (due_us < world->now_us) {
        due_us = world->now_us;
    }
    CHECK(mesh_sim_schedule_relay_tick(world, transmitter, due_us) ==
          MESH_SIM_OK);
    CHECK(keep_connection_alive_until(world, connection, due_us) == MESH_SIM_OK);
    CHECK(world->roles[transmitter].tx_queue_count > 0u);
    return 0;
}

static int drive_until_packet_reaches_relay(struct mesh_sim_world *world,
                                            uint16_t connection,
                                            uint32_t session_id,
                                            uint16_t seq,
                                            size_t expected_receptions)
{
    for (size_t step = 0u; step < 32u; step++) {
        if (count_matching_receptions(world, ANCHOR_ID, MSG_MESH_DATA,
                                      session_id, seq) >=
            expected_receptions) {
            return 0;
        }
        CHECK(run_connection_action(world, connection, false) == MESH_SIM_OK);
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int drive_until_transition_count(struct mesh_sim_world *world,
                                        uint16_t connection,
                                        enum mesh_sim_transition_kind kind,
                                        uint64_t node_id,
                                        size_t expected_count)
{
    for (size_t step = 0u; step < 32u; step++) {
        if (mesh_sim_count_transitions(world, kind, node_id) >= expected_count) {
            return 0;
        }
        CHECK(run_connection_action(world, connection, false) == MESH_SIM_OK);
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int run_relay_to_gateway_payload_attempt(
    struct mesh_sim_world *world,
    uint8_t anchor,
    uint8_t gateway,
    uint32_t session_id,
    uint16_t seq,
    bool lose_on_air)
{
    const struct mesh_sim_queued_tx *queued = queued_packet_for_peer(
        &world->roles[anchor], GATEWAY_ID, MSG_MESH_DATA, session_id, seq);
    uint64_t air_start_us = world->now_us + DIRECT_TX_PREPARE_US;
    uint64_t window_end_us = air_start_us + DIRECT_PAYLOAD_SERVICE_US;
    bool reachable = world->reachable[anchor][gateway];

    CHECK(queued != NULL);
    CHECK(queued->outbound.payload_len == FORCEDHOP_PAYLOAD_LEN);
    CHECK(app_mesh_ch9_timeout_pressure_decide(
              &queued->outbound, true, true, false, ANCHOR_ID) ==
          APP_MESH_CH9_TIMEOUT_RETRY);
    CHECK(mesh_sim_direct_gateway_arm_rx(world, gateway,
                                         air_start_us, window_end_us) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_direct_gateway_start_queued_tx(world, anchor,
                                                  air_start_us, window_end_us,
                                                  NULL) == MESH_SIM_OK);
    if (lose_on_air) {
        world->reachable[anchor][gateway] = false;
    }
    CHECK(mesh_sim_run_until(world, window_end_us) == MESH_SIM_OK);
    world->reachable[anchor][gateway] = reachable;
    if (world->roles[gateway].dwm3000.cpu_busy_until_us > world->now_us) {
        CHECK(mesh_sim_run_until(
                  world, world->roles[gateway].dwm3000.cpu_busy_until_us) ==
              MESH_SIM_OK);
    }
    return 0;
}

static int run_gateway_to_relay_ack_attempt(struct mesh_sim_world *world,
                                            uint8_t anchor,
                                            uint8_t gateway,
                                            bool lose_on_air)
{
    uint64_t air_start_us = world->now_us + DIRECT_TX_PREPARE_US;
    uint64_t window_end_us = air_start_us + DIRECT_ACK_SERVICE_US;
    bool reachable = world->reachable[gateway][anchor];

    CHECK(mesh_sim_direct_gateway_schedule_ack(world, gateway, anchor,
                                                air_start_us, window_end_us,
                                                NULL) == MESH_SIM_OK);
    if (lose_on_air) {
        world->reachable[gateway][anchor] = false;
    }
    CHECK(mesh_sim_run_until(world, window_end_us) == MESH_SIM_OK);
    world->reachable[gateway][anchor] = reachable;
    return 0;
}

static int run_forcedhop_delivery_loss_case(uint8_t payload_loss_mask,
                                           uint8_t ack_loss_mask)
{
    static struct mesh_sim_world world;
    struct mesh_event_params params = connection_params(1000u);
    uint8_t transmitter;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t connection;

    mesh_sim_init(&world,
                  UINT32_C(0xe2e00000) |
                      ((uint32_t)payload_loss_mask << 8) |
                      ack_loss_mask);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                            TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &transmitter) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ANCHOR_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &anchor) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, transmitter, anchor, 98u, 2u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, anchor, gateway, 98u, 2u) == MESH_SIM_OK);
    CHECK(!world.reachable[transmitter][gateway]);
    CHECK(!world.reachable[gateway][transmitter]);
    CHECK(mesh_sim_install_route(&world, transmitter, anchor, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&world, anchor, gateway, 0u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_downlink(&world, anchor, TRANSMITTER_ID,
                                    transmitter, 1u, ROUTE_EPOCH) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_connection(&world, transmitter, anchor, &params, true,
                                  &connection) == MESH_SIM_OK);
    CHECK(world.connection_count == 1u);
    CHECK(world.connections[connection].node_a == transmitter);
    CHECK(world.connections[connection].node_b == anchor);

    for (uint16_t packet_index = 0u;
         packet_index < FORCEDHOP_BURST_COUNT;
         packet_index++) {
        struct proto_packet packet = {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = TRANSMITTER_ID,
            .dst_id = GATEWAY_ID,
            .session_id = UINT32_C(0xe2e10000) |
                          ((uint32_t)payload_loss_mask << 8) |
                          ack_loss_mask,
            .seq = (uint16_t)(packet_index + 1u),
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = FORCEDHOP_PAYLOAD_LEN,
        };
        uint8_t payload[FORCEDHOP_PAYLOAD_LEN];
        bool payload_loss_pending =
            (payload_loss_mask & (uint8_t)(1u << packet_index)) != 0u;
        bool ack_loss_pending =
            (ack_loss_mask & (uint8_t)(1u << packet_index)) != 0u;
        size_t relay_receptions = 0u;
        size_t hop_progress = mesh_sim_count_transitions(
            &world, MESH_SIM_TRANSITION_HOP_PROGRESS, TRANSMITTER_ID);
        size_t gateway_confirms = mesh_sim_count_transitions(
            &world, MESH_SIM_TRANSITION_GATEWAY_ACKED, TRANSMITTER_ID);
        bool delivered = false;

        memset(payload, (int)(0x20u + packet_index), sizeof(payload));
        CHECK(mesh_sim_queue_originated(&world, transmitter, &packet,
                                        payload, sizeof(payload)) ==
              MESH_SIM_OK);
        for (size_t attempt = 0u; attempt < 3u; attempt++) {
            bool lose_payload = payload_loss_pending;
            size_t delivery_count_before = world.roles[gateway].delivery_count;

            relay_receptions++;
            CHECK(drive_until_packet_reaches_relay(
                      &world, connection, packet.session_id, packet.seq,
                      relay_receptions) == 0);
            hop_progress++;
            CHECK(drive_until_transition_count(
                      &world, connection, MESH_SIM_TRANSITION_HOP_PROGRESS,
                      TRANSMITTER_ID, hop_progress) == 0);
            CHECK(run_relay_to_gateway_payload_attempt(
                      &world, anchor, gateway, packet.session_id, packet.seq,
                      lose_payload) == 0);
            if (lose_payload) {
                payload_loss_pending = false;
                CHECK(world.roles[gateway].delivery_count ==
                      delivery_count_before);
                CHECK(queued_gateway_ack_for_sender(
                          &world.roles[gateway], ANCHOR_ID, TRANSMITTER_ID,
                          packet.session_id) == NULL);
                CHECK(schedule_origin_retry(&world, transmitter,
                                            connection) == 0);
                continue;
            }

            CHECK(world.roles[gateway].delivery_count ==
                  (delivered ? delivery_count_before :
                               delivery_count_before + 1u));
            delivered = true;
            CHECK(queued_gateway_ack_for_sender(
                      &world.roles[gateway], ANCHOR_ID, TRANSMITTER_ID,
                      packet.session_id) != NULL);
            CHECK(run_gateway_to_relay_ack_attempt(
                      &world, anchor, gateway, ack_loss_pending) == 0);
            if (ack_loss_pending) {
                ack_loss_pending = false;
                CHECK(schedule_origin_retry(&world, transmitter,
                                            connection) == 0);
                continue;
            }

            gateway_confirms++;
            CHECK(drive_until_transition_count(
                      &world, connection, MESH_SIM_TRANSITION_GATEWAY_ACKED,
                      TRANSMITTER_ID, gateway_confirms) == 0);
            CHECK(world.roles[transmitter].relay.pending.state ==
                  MESH_RELAY_TX_IDLE);
            break;
        }

        CHECK(delivered);
        CHECK(!payload_loss_pending);
        CHECK(!ack_loss_pending);
        CHECK(world.roles[gateway].delivery_count == packet_index + 1u);
        CHECK(world.roles[gateway].deliveries[packet_index].packet.src_id ==
              TRANSMITTER_ID);
        CHECK(world.roles[gateway].deliveries[packet_index].packet.session_id ==
              packet.session_id);
        CHECK(world.roles[gateway].deliveries[packet_index].packet.seq ==
              packet.seq);
        CHECK(world.roles[gateway].deliveries[packet_index].payload_len ==
              FORCEDHOP_PAYLOAD_LEN);
        CHECK(memcmp(world.roles[gateway].deliveries[packet_index].payload,
                     payload, sizeof(payload)) == 0);
    }

    CHECK(world.roles[gateway].delivery_count == FORCEDHOP_BURST_COUNT);
    CHECK(world.connection_count == 1u);
    CHECK(world.roles[gateway].tx_queue_count == 0u);
    CHECK(world.roles[transmitter].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     TRANSMITTER_ID) ==
          FORCEDHOP_BURST_COUNT);
    return 0;
}

static int run_forcedhop_delivery_loss_sweep(void)
{
    for (uint8_t payload_mask = 0u;
         payload_mask < LOSS_SWEEP_CASES;
         payload_mask++) {
        for (uint8_t ack_mask = 0u;
             ack_mask < LOSS_SWEEP_CASES;
             ack_mask++) {
            phase = "unscheduled_gateway_loss_sweep";
            if (run_forcedhop_delivery_loss_case(payload_mask, ack_mask) != 0) {
                fprintf(stderr, " loss_masks payload=0x%x ack=0x%x\n",
                        payload_mask, ack_mask);
                return 1;
            }
        }
    }
    return 0;
}

static int install_control_downlink(struct mesh_relay *relay,
                                    uint8_t index,
                                    uint64_t target_id,
                                    uint64_t next_hop_id,
                                    uint8_t hop_count)
{
    struct mesh_downlink_entry *entry;

    CHECK(relay != NULL);
    CHECK(index < MESH_RELAY_DOWNLINK_ROUTES);
    entry = &relay->downlinks[index];
    CHECK(!entry->valid);
    *entry = (struct mesh_downlink_entry) {
        .target_id = target_id,
        .next_hop_id = next_hop_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = 1000u,
        .hop_count = hop_count,
        .quality = 95u,
        .valid = true,
    };
    return 0;
}

static int build_targeted_survey_control(struct mesh_outbound *out,
                                         uint8_t msg_type,
                                         uint64_t target_id,
                                         uint16_t seq)
{
    const struct survey_pair pair = {
        .survey_id = TARGETED_SURVEY_ID,
        .initiator_id = target_id,
        .responder_id = target_id + UINT64_C(0x1000),
        .sample_count = 3u,
    };
    size_t payload_len = 0u;
    int ret;

    memset(out, 0, sizeof(*out));
    if (msg_type == MSG_COMMAND) {
        ret = mesh_append_command_id(out->payload,
                                     sizeof(out->payload),
                                     &payload_len,
                                     CMD_SURVEY_START_PAIR);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = survey_append_pair_tlvs(out->payload,
                                  sizeof(out->payload),
                                  &payload_len,
                                  &pair);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (msg_type == MSG_COMMAND) {
        ret = mesh_init_command(&out->packet,
                                GATEWAY_ID,
                                target_id,
                                pair.survey_id,
                                seq,
                                (uint8_t)payload_len);
    } else if (msg_type == MSG_SURVEY_PAIR_PREPARE) {
        ret = survey_init_pair_prepare_packet(&out->packet,
                                              &pair,
                                              GATEWAY_ID,
                                              seq,
                                              (uint8_t)payload_len);
    } else {
        return PROTO_ERR_ARG;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = ANCHOR_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static bool control_capture_relevant(const struct proto_packet *packet,
                                     uint64_t previous_hop_id,
                                     const struct mesh_relay *relay)
{
    const struct mesh_downlink_entry *downlink = NULL;
    bool targeted_control_relay = false;

    if ((packet->msg_type == MSG_COMMAND ||
         packet->msg_type == MSG_SURVEY_PAIR_PREPARE) &&
        packet->dst_id != 0u &&
        packet->dst_id != MESH_BROADCAST_ID &&
        packet->dst_id != relay->local_id &&
        packet->ttl > 1u) {
        downlink = mesh_relay_find_downlink(relay, packet->dst_id);
        targeted_control_relay =
            downlink != NULL && downlink->valid &&
            downlink->route_epoch == relay->upstream.current_epoch &&
            downlink->gateway_id == GATEWAY_ID &&
            downlink->next_hop_id != 0u &&
            downlink->next_hop_id != MESH_BROADCAST_ID &&
            downlink->next_hop_id != relay->local_id &&
            downlink->next_hop_id != previous_hop_id;
    }
    const struct app_mesh_c5_route_capture_state state = {
        .msg_type = packet->msg_type,
        .session_id = packet->session_id,
        .src_id = packet->src_id,
        .dst_id = packet->dst_id,
        .previous_hop_id = previous_hop_id,
        .target_id = relay->local_id,
        .local_id = relay->local_id,
        .control_origin_id = GATEWAY_ID,
        .control_followup = true,
        .gateway_control_priority = true,
        .targeted_control_relay = targeted_control_relay,
    };

    return app_mesh_c5_route_capture_relevant(&state);
}

static int run_targeted_gateway_control_multirelay_scenario(void)
{
    const uint64_t first_relay_id = ANCHOR_ID + UINT64_C(0x100);
    const uint64_t second_relay_id = ANCHOR_ID + UINT64_C(0x200);
    const uint64_t target_ids[2] = {
        TRANSMITTER_ID + UINT64_C(0x1000),
        TRANSMITTER_ID + UINT64_C(0x2000),
    };
    const uint8_t control_types[2] = {
        MSG_COMMAND,
        MSG_SURVEY_PAIR_PREPARE,
    };
    struct mesh_relay first_relay;
    struct mesh_relay second_relay;
    struct mesh_relay targets[2];
    uint16_t seq = UINT16_C(0x6200);

    phase = "targeted_gateway_control_multirelay";
    mesh_relay_init(&first_relay, MESH_RELAY_ROLE_ANCHOR,
                    first_relay_id, GATEWAY_ID, ROUTE_EPOCH);
    mesh_relay_init(&second_relay, MESH_RELAY_ROLE_ANCHOR,
                    second_relay_id, GATEWAY_ID, ROUTE_EPOCH);
    for (size_t target = 0u; target < 2u; target++) {
        mesh_relay_init(&targets[target], MESH_RELAY_ROLE_ANCHOR,
                        target_ids[target], GATEWAY_ID, ROUTE_EPOCH);
        CHECK(install_control_downlink(&first_relay, (uint8_t)target,
                                       target_ids[target], second_relay_id,
                                       2u) == 0);
        CHECK(install_control_downlink(&second_relay, (uint8_t)target,
                                       target_ids[target], target_ids[target],
                                       1u) == 0);
    }

    {
        struct mesh_outbound control;
        struct mesh_downlink_entry saved = first_relay.downlinks[0];

        CHECK(build_targeted_survey_control(&control, MSG_COMMAND,
                                             target_ids[0],
                                             UINT16_C(0x61ff)) == PROTO_OK);
        first_relay.downlinks[0].route_epoch = ROUTE_EPOCH - 1u;
        CHECK(!control_capture_relevant(&control.packet, GATEWAY_ID,
                                        &first_relay));
        first_relay.downlinks[0] = saved;
        first_relay.downlinks[0].next_hop_id = first_relay_id;
        CHECK(!control_capture_relevant(&control.packet, GATEWAY_ID,
                                        &first_relay));
        first_relay.downlinks[0] = saved;
    }

    for (size_t type = 0u; type < 2u; type++) {
        for (size_t target = 0u; target < 2u; target++) {
            struct mesh_outbound control;
            struct mesh_relay_result first_result;
            struct mesh_relay_result second_result;
            struct mesh_relay_result target_result;
            struct survey_pair decoded_pair;
            const struct route_candidate *route;
            uint8_t origin_ttl = 0u;
            enum command_id command_id =
                control_types[type] == MSG_COMMAND ?
                    CMD_SURVEY_START_PAIR : CMD_SURVEY_PREPARE_PAIR;

            seq++;
            CHECK(build_targeted_survey_control(&control,
                                                 control_types[type],
                                                 target_ids[target],
                                                 seq) == PROTO_OK);
            CHECK(app_mesh_c5_gateway_control_origin_ttl(
                      control.packet.msg_type,
                      (uint16_t)command_id,
                      &origin_ttl));
            CHECK(control.packet.ttl == origin_ttl);

            CHECK(control_capture_relevant(&control.packet,
                                           GATEWAY_ID,
                                           &first_relay));
            CHECK(mesh_relay_handle_rx(&first_relay,
                                       &control.packet,
                                       control.payload,
                                       control.payload_len,
                                       GATEWAY_ID,
                                       96u,
                                       2000u + seq,
                                       &first_result) == PROTO_OK);
            CHECK(first_result.status == PROTO_OK);
            CHECK(has_action(&first_result, MESH_RELAY_ACTION_FORWARD));
            CHECK(!has_action(&first_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
            CHECK(!has_action(&first_result, MESH_RELAY_ACTION_DROP));
            CHECK(first_result.forward.next_hop_id == second_relay_id);
            CHECK(first_result.forward.packet.src_id == GATEWAY_ID);
            CHECK(first_result.forward.packet.dst_id == target_ids[target]);
            CHECK(first_result.forward.packet.seq == seq);
            CHECK(first_result.forward.packet.ttl == origin_ttl - 1u);
            CHECK(first_result.forward.payload_len == control.payload_len);
            CHECK(memcmp(first_result.forward.payload, control.payload,
                         control.payload_len) == 0);

            CHECK(control_capture_relevant(&first_result.forward.packet,
                                           first_relay_id,
                                           &second_relay));
            CHECK(mesh_relay_handle_rx(&second_relay,
                                       &first_result.forward.packet,
                                       first_result.forward.payload,
                                       first_result.forward.payload_len,
                                       first_relay_id,
                                       94u,
                                       3000u + seq,
                                       &second_result) == PROTO_OK);
            CHECK(second_result.status == PROTO_OK);
            CHECK(has_action(&second_result, MESH_RELAY_ACTION_FORWARD));
            CHECK(!has_action(&second_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
            CHECK(!has_action(&second_result, MESH_RELAY_ACTION_DROP));
            CHECK(second_result.forward.next_hop_id == target_ids[target]);
            CHECK(second_result.forward.packet.src_id == GATEWAY_ID);
            CHECK(second_result.forward.packet.dst_id == target_ids[target]);
            CHECK(second_result.forward.packet.seq == seq);
            CHECK(second_result.forward.packet.ttl == origin_ttl - 2u);
            CHECK(second_result.forward.payload_len == control.payload_len);
            CHECK(memcmp(second_result.forward.payload, control.payload,
                         control.payload_len) == 0);

            CHECK(control_capture_relevant(&second_result.forward.packet,
                                           second_relay_id,
                                           &targets[target]));
            CHECK(mesh_relay_note_gateway_control_reverse_route(
                      &targets[target], &second_result.forward.packet,
                      second_relay_id, 92u, origin_ttl,
                      4000u + seq) == PROTO_OK);
            CHECK(mesh_relay_handle_rx(&targets[target],
                                       &second_result.forward.packet,
                                       second_result.forward.payload,
                                       second_result.forward.payload_len,
                                       second_relay_id,
                                       92u,
                                       4000u + seq,
                                       &target_result) == PROTO_OK);
            CHECK(target_result.status == PROTO_OK);
            CHECK(has_action(&target_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
            CHECK(!has_action(&target_result, MESH_RELAY_ACTION_FORWARD));
            CHECK(!has_action(&target_result, MESH_RELAY_ACTION_DROP));
            CHECK(survey_extract_pair_tlvs(second_result.forward.payload,
                                           second_result.forward.payload_len,
                                           &decoded_pair) == PROTO_OK);
            CHECK(decoded_pair.survey_id == TARGETED_SURVEY_ID);
            CHECK(decoded_pair.initiator_id == target_ids[target]);
            CHECK(decoded_pair.responder_id ==
                  target_ids[target] + UINT64_C(0x1000));

            route = route_selected(&first_relay.upstream);
            CHECK(route == NULL);
            route = route_selected(&second_relay.upstream);
            CHECK(route == NULL);
            route = route_selected(&targets[target].upstream);
            CHECK(route != NULL && route->next_hop_id == second_relay_id);
            CHECK(route->hop_count == 2u);

            for (size_t destination = 0u; destination < 2u; destination++) {
                const struct mesh_downlink_entry *first_downlink =
                    mesh_relay_find_downlink(&first_relay,
                                             target_ids[destination]);
                const struct mesh_downlink_entry *second_downlink =
                    mesh_relay_find_downlink(&second_relay,
                                             target_ids[destination]);

                CHECK(first_downlink != NULL);
                CHECK(first_downlink->next_hop_id == second_relay_id);
                CHECK(second_downlink != NULL);
                CHECK(second_downlink->next_hop_id ==
                      target_ids[destination]);
            }
        }
    }
    return 0;
}

static int run_targeted_gateway_control_bypasses_unrelated_custody_scenario(void)
{
    const uint64_t relay_id = ANCHOR_ID + UINT64_C(0x300);
    const uint64_t target_id = TRANSMITTER_ID + UINT64_C(0x3000);
    const uint64_t custody_origin_id = TRANSMITTER_ID + UINT64_C(0x4000);
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = ROUTE_EPOCH,
        .command_seq = UINT32_C(0x71107110),
        .node_id = custody_origin_id,
        .node_boot_counter = 3u,
        .result_seq = 4u,
    };
    const uint8_t control_types[2] = {
        MSG_COMMAND,
        MSG_SURVEY_PAIR_PREPARE,
    };
    struct mesh_relay relay;
    struct proto_packet custody_packet = {0};
    struct mesh_outbound custody_tx;
    struct mesh_pending_tx pending_before;
    struct persistent_outbox_record outbox_before;
    uint8_t custody_payload[96];
    size_t custody_payload_len = 0u;

    phase = "targeted_gateway_control_busy_relay";
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    relay_id, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_note_direct_gateway_route(&relay, 5000u) == PROTO_OK);
    CHECK(install_control_downlink(&relay, 0u, target_id, target_id, 1u) == 0);

    CHECK(command_result_id_append_tlvs(custody_payload,
                                         sizeof(custody_payload),
                                         &custody_payload_len,
                                         &result_id) == PROTO_OK);
    CHECK(mesh_append_command_result(custody_payload,
                                     sizeof(custody_payload),
                                     &custody_payload_len,
                                     CMD_GET_STATUS,
                                     COMMAND_OK,
                                     0u) == PROTO_OK);
    CHECK(mesh_init_command_result(&custody_packet,
                                   custody_origin_id,
                                   GATEWAY_ID,
                                   UINT32_C(0x71107110),
                                   UINT16_C(0x7110),
                                   (uint8_t)custody_payload_len,
                                   false) == PROTO_OK);
    CHECK(mesh_relay_start_tx(&relay,
                              &custody_packet,
                              custody_payload,
                              custody_payload_len,
                              5010u,
                              &custody_tx) == PROTO_OK);
    CHECK(custody_tx.next_hop_id == GATEWAY_ID);
    CHECK(mesh_relay_tx_active(&relay));
    CHECK(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(relay.pending.packet.src_id == custody_origin_id);
    mesh_relay_note_tx_sent(&relay, &custody_tx, 5010u);
    pending_before = relay.pending;
    outbox_before = relay.outbox_record;

    for (size_t type = 0u; type < 2u; type++) {
        struct mesh_outbound control;
        struct mesh_relay_result result;
        const uint16_t control_seq = (uint16_t)(UINT16_C(0x7200) + type);

        CHECK(build_targeted_survey_control(&control,
                                             control_types[type],
                                             target_id,
                                             control_seq) == PROTO_OK);
        CHECK(control_capture_relevant(&control.packet, GATEWAY_ID, &relay));
        CHECK(mesh_relay_handle_rx(&relay,
                                   &control.packet,
                                   control.payload,
                                   control.payload_len,
                                   GATEWAY_ID,
                                   96u,
                                   5020u + (uint32_t)type,
                                   &result) == PROTO_OK);
        CHECK(result.status == PROTO_OK);
        CHECK(has_action(&result, MESH_RELAY_ACTION_FORWARD));
        CHECK(!has_action(&result, MESH_RELAY_ACTION_DROP));
        CHECK(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
        CHECK(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
        CHECK(result.forward.next_hop_id == target_id);
        CHECK(result.forward.packet.msg_type == control.packet.msg_type);
        CHECK(result.forward.packet.src_id == GATEWAY_ID);
        CHECK(result.forward.packet.dst_id == target_id);
        CHECK(result.forward.packet.session_id == control.packet.session_id);
        CHECK(result.forward.packet.seq == control_seq);
        CHECK(result.forward.packet.ttl == control.packet.ttl - 1u);
        CHECK(result.forward.payload_len == control.payload_len);
        CHECK(memcmp(result.forward.payload, control.payload,
                     control.payload_len) == 0);
        CHECK(memcmp(&relay.pending, &pending_before,
                     sizeof(pending_before)) == 0);
        CHECK(memcmp(&relay.outbox_record, &outbox_before,
                     sizeof(outbox_before)) == 0);
        CHECK(mesh_relay_tx_active(&relay));
    }

    {
        struct proto_packet ordinary = {
            .msg_type = MSG_MESH_DATA,
            .src_id = GATEWAY_ID,
            .dst_id = target_id,
            .session_id = UINT32_C(0x73007300),
            .seq = UINT16_C(0x7300),
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = 1u,
        };
        struct mesh_relay_result result;
        const uint8_t payload[1] = {0x73u};

        CHECK(mesh_relay_handle_rx(&relay,
                                   &ordinary,
                                   payload,
                                   sizeof(payload),
                                   GATEWAY_ID,
                                   96u,
                                   5030u,
                                   &result) == PROTO_OK);
        CHECK(result.status == PROTO_ERR_BUSY);
        CHECK(has_action(&result, MESH_RELAY_ACTION_DROP));
        CHECK(has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
        CHECK(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
        CHECK(memcmp(&relay.pending, &pending_before,
                     sizeof(pending_before)) == 0);
        CHECK(memcmp(&relay.outbox_record, &outbox_before,
                     sizeof(outbox_before)) == 0);
        CHECK(mesh_relay_tx_active(&relay));
    }
    return 0;
}

struct forcedhop_ack_flush_fixture {
    struct mesh_outbound outbound;
    int result;
    uint8_t calls;
};

static int forcedhop_ack_flush(const struct mesh_outbound *outbound, void *ctx)
{
    struct forcedhop_ack_flush_fixture *fixture = ctx;

    fixture->outbound = *outbound;
    fixture->calls++;
    return fixture->result;
}

static int run_forcedhop_batch_ack_scenario(void)
{
    struct app_mesh_ch9_ack_table gateway_table;
    struct app_mesh_ch9_ack_table relay_table;
    struct mesh_outbound frames[FORCEDHOP_BURST_COUNT];
    struct app_mesh_ch9_tx_ack_entry tracked[FORCEDHOP_BURST_COUNT];
    struct forcedhop_ack_flush_fixture gateway_flush = {.result = -EAGAIN};
    struct forcedhop_ack_flush_fixture relay_flush = {.result = -EAGAIN};
    struct app_mesh_ch9_tx_ack_result ack_result;
    struct mesh_outbound ack_template = {
        .packet = {
            .msg_type = MSG_GATEWAY_ACK,
            .flags = FLAG_GATEWAY_ACK,
            .src_id = GATEWAY_ID,
            .dst_id = TRANSMITTER_ID,
            .session_id = UINT32_C(0xbac10001),
            .seq = UINT16_C(0x7101),
            .ttl = MESH_GATEWAY_ACK_TTL,
        },
        .next_hop_id = ANCHOR_ID,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
    };
    uint8_t terminal_count[FORCEDHOP_BURST_COUNT] = {0};

    phase = "four_frame_batch_ack";
    CHECK(APP_MESH_CH9_ACK_BATCH_ENTRY_MAX == FORCEDHOP_BURST_COUNT);
    app_mesh_ch9_ack_table_init(&gateway_table);
    app_mesh_ch9_ack_table_init(&relay_table);

    for (uint8_t i = 0u; i < FORCEDHOP_BURST_COUNT; i++) {
        struct app_mesh_ch9_ack_batch_entry entry = {
            .session_id = UINT32_C(0x70000000) + i,
            .packet_id = FORCEDHOP_BATCH_ID + i,
            .seq = (uint16_t)(UINT16_C(0x4100) + i),
            .has_packet_id = true,
        };
        enum app_mesh_ch9_ack_queue_result queue_result;
        const uint8_t *value = NULL;
        uint8_t encoded[UWB_MESH_MAX_FRAME_LEN];
        uint8_t value_len = 0u;
        size_t encoded_len = 0u;
        size_t metadata_len = 0u;

        memset(&frames[i], 0, sizeof(frames[i]));
        frames[i].packet.msg_type = MSG_MESH_DATA;
        frames[i].packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
        frames[i].packet.src_id = TRANSMITTER_ID;
        frames[i].packet.dst_id = GATEWAY_ID;
        frames[i].packet.session_id = entry.session_id;
        frames[i].packet.seq = entry.seq;
        frames[i].packet.ttl = MESH_DEFAULT_TTL;
        frames[i].packet.payload_len = FORCEDHOP_PAYLOAD_LEN;
        frames[i].payload_len = FORCEDHOP_PAYLOAD_LEN;
        frames[i].next_hop_id = GATEWAY_ID;
        frames[i].radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        CHECK(tlv_append_u32(frames[i].payload,
                             sizeof(frames[i].payload),
                             &metadata_len,
                             TLV_MESH_CH9_BATCH_ID,
                             FORCEDHOP_BATCH_ID) == PROTO_OK);
        CHECK(tlv_append_u8(frames[i].payload,
                            sizeof(frames[i].payload),
                            &metadata_len,
                            TLV_MESH_CH9_BATCH_FLAGS,
                            i + 1u == FORCEDHOP_BURST_COUNT ?
                                FORCEDHOP_BATCH_FLAG_FINAL : 0u) == PROTO_OK);
        memset(&frames[i].payload[metadata_len],
               (int)(0x40u + i),
               FORCEDHOP_PAYLOAD_LEN - metadata_len);
        CHECK(proto_packet_encoded_len(frames[i].payload_len) > 0u);
        CHECK(uwb_mesh_frame_encode(UINT32_C(0x10203040),
                                    ANCHOR_ID,
                                    GATEWAY_ID,
                                    &frames[i].packet,
                                    frames[i].payload,
                                    encoded,
                                    sizeof(encoded),
                                    &encoded_len) == PROTO_OK);
        CHECK(encoded_len <= UWB_PHY_EXTENDED_FRAME_MAX_LEN);
        CHECK(tlv_find(frames[i].payload,
                       frames[i].payload_len,
                       TLV_MESH_CH9_BATCH_ID,
                       &value,
                       &value_len) == PROTO_OK);
        CHECK(value_len == sizeof(uint32_t));
        CHECK(proto_get_u32_le(value) == FORCEDHOP_BATCH_ID);
        CHECK(tlv_find(frames[i].payload,
                       frames[i].payload_len,
                       TLV_MESH_CH9_BATCH_FLAGS,
                       &value,
                       &value_len) == PROTO_OK);
        CHECK(value_len == sizeof(uint8_t));
        CHECK(value[0] == (i + 1u == FORCEDHOP_BURST_COUNT ?
                           FORCEDHOP_BATCH_FLAG_FINAL : 0u));

        tracked[i].session_id = entry.session_id;
        tracked[i].seq = entry.seq;
        tracked[i].acked = false;

        /* The first final frame is lost, so the gateway cannot ACK yet. */
        if (i + 1u == FORCEDHOP_BURST_COUNT) {
            CHECK(app_mesh_ch9_ack_table_get_peer(&gateway_table,
                                                   ANCHOR_ID)->count ==
                  FORCEDHOP_BURST_COUNT - 1u);
            CHECK(gateway_flush.calls == 0u);
        }
        CHECK(app_mesh_ch9_ack_table_queue(&gateway_table,
                                            &ack_template,
                                            &entry,
                                            &queue_result) == PROTO_OK);
        CHECK(queue_result == APP_MESH_CH9_ACK_QUEUE_ADDED);

        /* A duplicate retry must not create a second terminal identity. */
        CHECK(app_mesh_ch9_ack_table_queue(&gateway_table,
                                            &ack_template,
                                            &entry,
                                            &queue_result) == PROTO_OK);
        CHECK(queue_result == APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
    }

    CHECK(app_mesh_ch9_ack_table_get_peer(&gateway_table, ANCHOR_ID)->count ==
          FORCEDHOP_BURST_COUNT);
    CHECK(app_mesh_ch9_ack_table_flush_peer(&gateway_table,
                                             ANCHOR_ID,
                                             forcedhop_ack_flush,
                                             &gateway_flush) == -EAGAIN);
    CHECK(gateway_flush.calls == 1u);
    CHECK(app_mesh_ch9_ack_table_pending_for_peer(&gateway_table, ANCHOR_ID));
    for (uint8_t i = 0u; i < FORCEDHOP_BURST_COUNT; i++) {
        CHECK(app_mesh_direct_gateway_ack_matches(
                  &frames[i],
                  &gateway_flush.outbound.packet,
                  gateway_flush.outbound.payload,
                  gateway_flush.outbound.payload_len,
                  GATEWAY_ID,
                  GATEWAY_ID));
    }

    gateway_flush.result = 0;
    CHECK(app_mesh_ch9_ack_table_flush_peer(&gateway_table,
                                             ANCHOR_ID,
                                             forcedhop_ack_flush,
                                             &gateway_flush) == 0);
    CHECK(gateway_flush.calls == 2u);
    CHECK(!app_mesh_ch9_ack_table_pending_for_peer(&gateway_table, ANCHOR_ID));

    /* The relay preserves the complete gateway ACK while bubbling it down. */
    gateway_flush.outbound.next_hop_id = TRANSMITTER_ID;
    CHECK(app_mesh_ch9_ack_table_queue_forwarded(&relay_table,
                                                  &gateway_flush.outbound,
                                                  NULL) == PROTO_OK);
    CHECK(app_mesh_ch9_ack_table_flush_peer(&relay_table,
                                             TRANSMITTER_ID,
                                             forcedhop_ack_flush,
                                             &relay_flush) == -EAGAIN);
    CHECK(relay_flush.calls == 1u);
    CHECK(app_mesh_ch9_ack_table_pending_for_peer(&relay_table,
                                                   TRANSMITTER_ID));
    relay_flush.result = 0;
    CHECK(app_mesh_ch9_ack_table_flush_peer(&relay_table,
                                             TRANSMITTER_ID,
                                             forcedhop_ack_flush,
                                             &relay_flush) == 0);
    CHECK(relay_flush.calls == 2u);
    CHECK(!app_mesh_ch9_ack_table_pending_for_peer(&relay_table,
                                                    TRANSMITTER_ID));
    CHECK(app_mesh_ch9_tx_ack_apply(&relay_flush.outbound.packet,
                                     relay_flush.outbound.payload,
                                     relay_flush.outbound.payload_len,
                                     tracked,
                                     FORCEDHOP_BURST_COUNT,
                                     &ack_result) == PROTO_OK);
    CHECK(ack_result.acked_now == FORCEDHOP_BURST_COUNT);
    CHECK(ack_result.unacked_count == 0u);
    CHECK(ack_result.all_acked);
    for (uint8_t i = 0u; i < FORCEDHOP_BURST_COUNT; i++) {
        if (tracked[i].acked) {
            terminal_count[i]++;
        }
    }

    /* Replayed batch ACKs are harmless and produce no second completion. */
    CHECK(app_mesh_ch9_tx_ack_apply(&relay_flush.outbound.packet,
                                     relay_flush.outbound.payload,
                                     relay_flush.outbound.payload_len,
                                     tracked,
                                     FORCEDHOP_BURST_COUNT,
                                     &ack_result) == PROTO_OK);
    CHECK(ack_result.acked_now == 0u);
    CHECK(ack_result.unacked_count == 0u);
    for (uint8_t i = 0u; i < FORCEDHOP_BURST_COUNT; i++) {
        CHECK(tracked[i].acked);
        CHECK(terminal_count[i] == 1u);
    }
    return 0;
}

int main(void)
{
    if (run_forcedhop_connection_scenario() != 0) {
        return 1;
    }
    if (run_forcedhop_batch_ack_scenario() != 0) {
        return 1;
    }
    if (run_forcedhop_delivery_loss_sweep() != 0) {
        return 1;
    }
    if (run_targeted_gateway_control_multirelay_scenario() != 0) {
        return 1;
    }
    if (run_targeted_gateway_control_bypasses_unrelated_custody_scenario() !=
        0) {
        return 1;
    }
    printf("forced-hop over-air route and stable connection scenario passed\n");
    return 0;
}
