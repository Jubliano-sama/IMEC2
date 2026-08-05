#include "app_mesh_direct_gateway_retry.h"
#include "app_discovery_assignment_policy.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh_sim.h"
#include "mesh_sim_invariants.h"
#include "node_transaction.h"
#include "survey.h"
#include "survey_gateway_transaction.h"
#include "survey_round_control.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xb001000000000001)
#define RELAY_ID UINT64_C(0xb002000000000002)
#define LEAF_ID UINT64_C(0xb003000000000003)
#define ROUTE_EPOCH UINT32_C(31)
#define ASSIGNMENT_EPOCH_1 UINT32_C(0x31000101)
#define ASSIGNMENT_EPOCH_2 UINT32_C(0x31000102)
#define SURVEY_ID_1 UINT32_C(0x31005001)
#define SURVEY_ID_2 UINT32_C(0x31005002)
#define SURVEY_OPERATION_GENERATION_1 UINT64_C(0x0000000131005001)
#define SURVEY_OPERATION_GENERATION_2 UINT64_C(0x0000000131005002)
#define SURVEY_ROUND_ID UINT16_C(1)
#define MAX_DRIVE_STEPS 600u
#define C5_GUARD_US UINT64_C(500)
#define DIRECT_GATEWAY_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_GATEWAY_PAYLOAD_SERVICE_US UINT64_C(50000)
#define DIRECT_GATEWAY_ACK_SERVICE_US UINT64_C(40000)
#define DIRECT_GATEWAY_RX_COMPLETION_GUARD_US UINT64_C(1)

_Static_assert(DIRECT_GATEWAY_ACK_SERVICE_US <=
                   (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u,
               "direct gateway ACK service must fit the production RX bound");

struct fixture {
    struct mesh_sim_world world;
    uint8_t leaf;
    uint8_t relay;
    uint8_t gateway;
    uint16_t connection;
    uint64_t direct_gateway_turns;
    uint64_t direct_gateway_ack_turns;
    uint64_t max_direct_gateway_ack_turnaround_us;
};

static int failures;

#define CHECK(expression, ...) do {                                           \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL line=%d now=%" PRIu64 " error=%d ",         \
                __LINE__, fixture->world.now_us, fixture->world.last_error);  \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                   \
        failures++;                                                           \
        return false;                                                         \
    }                                                                         \
} while (0)

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static struct mesh_event_params connection_params(uint32_t phase_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = 360u,
        .event_window_ms = 25u,
        .first_event_time_ms = phase_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static bool network_idle(const struct mesh_sim_world *world)
{
    for (size_t i = 0u; i < world->role_count; i++) {
        const struct mesh_sim_role_instance *node = &world->roles[i];
        if (node->tx_queue_count != 0u || node->route_waiting_valid ||
            node->relay.pending.state != MESH_RELAY_TX_IDLE) {
            return false;
        }
    }
    return true;
}

static int schedule_relay_deadlines(struct mesh_sim_world *world)
{
    for (size_t i = 0u; i < world->role_count; i++) {
        const struct mesh_pending_tx *pending = &world->roles[i].relay.pending;
        uint32_t deadline_ms = 0u;
        uint64_t deadline_us;
        int ret;
        if (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
            pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD) {
            deadline_ms = pending->gateway_ack_deadline_ms;
        } else if (pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
                   pending->state == MESH_RELAY_TX_WAIT_RESULT_GRANT) {
            deadline_ms = pending->retry_after_ms;
        }
        if (deadline_ms == 0u) {
            continue;
        }
        deadline_us = (uint64_t)deadline_ms * 1000u;
        if (deadline_us < world->now_us) {
            deadline_us = world->now_us;
        }
        ret = mesh_sim_schedule_relay_tick(world, (uint8_t)i, deadline_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return MESH_SIM_OK;
}

static int run_next_connection(struct fixture *state)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(&state->world,
                                              state->connection, &action);

    if (ret == MESH_SIM_OK && !action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(
            &state->world, state->connection, false);
    }
    return ret == MESH_SIM_OK ?
           mesh_sim_run_until(&state->world, action.end_us) : ret;
}

static bool relay_has_gateway_packet(const struct fixture *state)
{
    const struct mesh_sim_role_instance *relay =
        &state->world.roles[state->relay];

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (relay->tx_queue[i].valid &&
            relay->tx_queue[i].outbound.next_hop_id == GATEWAY_ID) {
            return true;
        }
    }
    return false;
}

static bool gateway_ack_queued_for_relay(const struct fixture *state)
{
    const struct mesh_sim_role_instance *gateway =
        &state->world.roles[state->gateway];

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &gateway->tx_queue[i];

        if (queued->valid && !queued->needs_relay_start &&
            queued->outbound.packet.msg_type == MSG_GATEWAY_ACK &&
            queued->outbound.next_hop_id == RELAY_ID) {
            return true;
        }
    }
    return false;
}

static uint64_t transmission_arrival_end_us(
    const struct mesh_sim_world *world, uint16_t transmission_index,
    uint8_t receiver_index)
{
    const struct mesh_sim_transmission *transmission =
        &world->transmissions[transmission_index];
    uint64_t delay_us =
        world->propagation_us[transmission->node_index][receiver_index] +
        transmission->fault_extra_delay_us[receiver_index];

    return transmission->end_us > UINT64_MAX - delay_us ?
           UINT64_MAX : transmission->end_us + delay_us;
}

static int run_direct_gateway_turn(struct fixture *state)
{
    struct mesh_sim_world *world = &state->world;
    uint64_t runtime_ready_us = world->now_us;
    uint64_t payload_air_start_us;
    uint64_t payload_deadline_us;
    uint64_t payload_arrival_end_us;
    uint64_t gateway_rx_end_us;
    uint64_t ack_air_start_us;
    uint64_t ack_window_end_us;
    uint64_t ack_turnaround_us;
    uint16_t payload_tx;
    uint16_t ack_tx;
    int ret;

    if (world->connection_count != 1u) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (world->roles[state->relay].dwm3000.cpu_busy_until_us >
            runtime_ready_us) {
        runtime_ready_us =
            world->roles[state->relay].dwm3000.cpu_busy_until_us;
    }
    if (world->roles[state->gateway].dwm3000.cpu_busy_until_us >
            runtime_ready_us) {
        runtime_ready_us =
            world->roles[state->gateway].dwm3000.cpu_busy_until_us;
    }
    if (runtime_ready_us > world->now_us) {
        ret = mesh_sim_run_until(world, runtime_ready_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    payload_air_start_us = world->now_us + DIRECT_GATEWAY_TX_PREPARE_US;
    payload_deadline_us = payload_air_start_us +
                          DIRECT_GATEWAY_PAYLOAD_SERVICE_US;
    ret = mesh_sim_direct_gateway_start_queued_tx(
        world, state->relay, payload_air_start_us, payload_deadline_us,
        &payload_tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    payload_arrival_end_us = transmission_arrival_end_us(
        world, payload_tx, state->gateway);
    if (payload_arrival_end_us == UINT64_MAX) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    gateway_rx_end_us = payload_arrival_end_us +
                        DIRECT_GATEWAY_RX_COMPLETION_GUARD_US;
    ret = mesh_sim_direct_gateway_arm_rx(world, state->gateway,
                                         payload_air_start_us,
                                         gateway_rx_end_us);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, gateway_rx_end_us);
    }
    state->direct_gateway_turns++;
    if (ret != MESH_SIM_OK || !gateway_ack_queued_for_relay(state)) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    if (world->roles[state->gateway].dwm3000.cpu_busy_until_us >
            world->now_us) {
        ret = mesh_sim_run_until(
            world, world->roles[state->gateway].dwm3000.cpu_busy_until_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    ack_air_start_us = world->now_us +
                       (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS * 1000u;
    ack_window_end_us = ack_air_start_us + DIRECT_GATEWAY_ACK_SERVICE_US;
    ret = mesh_sim_direct_gateway_schedule_ack(
        world, state->gateway, state->relay, ack_air_start_us,
        ack_window_end_us, &ack_tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ack_turnaround_us = world->transmissions[ack_tx].start_us -
                        payload_arrival_end_us;
    if (world->transmissions[ack_tx].start_us <
            payload_arrival_end_us +
                (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS * 1000u ||
        world->transmissions[ack_tx].end_us >
            payload_arrival_end_us +
                (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    state->direct_gateway_ack_turns++;
    if (ack_turnaround_us > state->max_direct_gateway_ack_turnaround_us) {
        state->max_direct_gateway_ack_turnaround_us = ack_turnaround_us;
    }
    return mesh_sim_run_until(world, ack_window_end_us);
}

static int run_next_radio_turn(struct fixture *state)
{
    return relay_has_gateway_packet(state) ? run_direct_gateway_turn(state) :
                                             run_next_connection(state);
}

static int drive_to_delivery_count(struct fixture *state,
                                   size_t expected_deliveries)
{
    for (uint32_t step = 0u; step < MAX_DRIVE_STEPS; step++) {
        struct mesh_sim_invariant_report report;
        int ret;

        if (state->world.roles[state->gateway].delivery_count ==
                expected_deliveries && network_idle(&state->world)) {
            return mesh_sim_check_settled(&state->world, &report);
        }
        ret = schedule_relay_deadlines(&state->world);
        if (ret == MESH_SIM_OK) {
            ret = run_next_radio_turn(state);
        }
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = mesh_sim_check_invariants(&state->world, &report);
        if (ret != MESH_SIM_OK) {
            fprintf(stderr, "invariant=%s node=%zu object=%zu reason=%s\n",
                    mesh_sim_invariant_name(report.code), report.node_index,
                    report.object_index,
                    report.description == NULL ? "unknown" :
                                                 report.description);
            return ret;
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

/*
 * Channel-5 forwarding remains application-owned in production. This seam
 * injects the encoded production frame through the PHY model, decodes it with
 * the production parser, then gives it to the production relay state machine.
 */
static int c5_control_hop(struct fixture *state, uint8_t sender,
                          uint8_t receiver,
                          const struct mesh_outbound *outbound,
                          struct mesh_relay_result *result)
{
    struct proto_packet decoded;
    uint8_t frame[PACKET_EXT_MAX_LEN];
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    size_t frame_len = 0u;
    size_t reception_before = state->world.reception_count;
    uint64_t start_us = state->world.now_us + 2000u;
    uint32_t airtime_us;
    uint16_t rx_index;
    uint16_t tx_index;
    int ret;
    if (outbound->earliest_tx_ms != 0u &&
        (uint64_t)outbound->earliest_tx_ms * 1000u > start_us) {
        start_us = (uint64_t)outbound->earliest_tx_ms * 1000u;
    }
    ret = proto_packet_encode(&outbound->packet, outbound->payload,
                              frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame_len);
    if (airtime_us == 0u || start_us < C5_GUARD_US) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    ret = mesh_sim_schedule_rx(&state->world, receiver,
                               start_us - C5_GUARD_US,
                               start_us + airtime_us + C5_GUARD_US,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                               &rx_index);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_schedule_raw_tx(&state->world, sender, start_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                       frame, frame_len, false, &tx_index);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(&state->world,
                                 start_us + airtime_us + C5_GUARD_US);
    }
    if (ret != MESH_SIM_OK ||
        state->world.reception_count != reception_before + 1u ||
        state->world.receptions[reception_before].outcome !=
            MESH_SIM_RX_DECODED) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    ret = proto_packet_decode(state->world.transmissions[tx_index].frame,
                              state->world.transmissions[tx_index].frame_len,
                              &decoded, &decoded_payload,
                              &decoded_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_handle_rx_with_random(
        &state->world.roles[receiver].relay, &decoded, decoded_payload,
        decoded_payload_len, state->world.roles[sender].id, 96u,
        (uint32_t)(state->world.now_us / 1000u), UINT32_C(0x9e3779b9),
        result);
}

static int setup_forced_relay(struct fixture *state)
{
    struct mesh_event_params child = connection_params(100u);
    int ret;
    memset(state, 0, sizeof(*state));
    mesh_sim_init(&state->world, UINT32_C(0x3100cafe));
    ret = mesh_sim_add_role(&state->world, MESH_SIM_ROLE_ANCHOR, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &state->leaf);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&state->world, MESH_SIM_ROLE_ANCHOR, RELAY_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &state->relay);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&state->world, MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &state->gateway);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&state->world, state->leaf, state->relay,
                                96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&state->world, state->relay, state->gateway,
                                96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_connection(&state->world, state->leaf,
                                      state->relay, &child, true,
                                      &state->connection);
    }
    return ret;
}

static int install_gateway_route_advertisement(struct fixture *state)
{
    struct mesh_outbound gateway_adv;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result leaf_result;
    const struct route_candidate *route;
    int ret = mesh_relay_build_gateway_route_adv(
        &state->world.roles[state->gateway].relay, UINT32_C(0x48494131),
        (uint32_t)(state->world.now_us / 1000u), &gateway_adv);

    if (ret == PROTO_OK) {
        gateway_adv.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
        ret = c5_control_hop(state, state->gateway, state->relay,
                             &gateway_adv, &relay_result);
    }
    if (ret != PROTO_OK ||
        !has_action(&relay_result,
                    MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV)) {
        return ret == PROTO_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    relay_result.gateway_route_adv.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = c5_control_hop(state, state->relay, state->leaf,
                         &relay_result.gateway_route_adv, &leaf_result);
    if (ret != PROTO_OK) {
        return ret;
    }
    route = route_selected(&state->world.roles[state->relay].relay.upstream);
    if (route == NULL || route->next_hop_id != GATEWAY_ID) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    route = route_selected(&state->world.roles[state->leaf].relay.upstream);
    if (route == NULL || route->next_hop_id != RELAY_ID ||
        !has_action(&leaf_result,
                    MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV)) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    ret = mesh_sim_install_downlink(&state->world, state->gateway, LEAF_ID,
                                    state->relay, 2u, ROUTE_EPOCH);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_downlink(&state->world, state->relay, LEAF_ID,
                                        state->leaf, 1u, ROUTE_EPOCH);
    }
    return ret;
}

static int build_assignment_control(uint32_t epoch,
    enum discovery_assignment_phase phase, uint16_t seq,
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    struct mesh_outbound *outbound)
{
    size_t payload_len = 0u;
    int ret;
    memset(outbound, 0, sizeof(*outbound));
    ret = mesh_append_command_id(outbound->payload, sizeof(outbound->payload),
                                 &payload_len,
                                 CMD_ASSIGN_DISCOVERY_SLOTS);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
            outbound->payload, sizeof(outbound->payload), &payload_len,
            phase, epoch);
    }
    if (ret == PROTO_OK && phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE) {
        ret = discovery_assignment_append_table_tlvs(
            outbound->payload, sizeof(outbound->payload), &payload_len,
            entries, entry_count);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    outbound->packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID,
        .dst_id = LEAF_ID,
        .session_id = epoch,
        .seq = seq,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = (uint16_t)payload_len,
    };
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = RELAY_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static int deliver_targeted_control(struct fixture *state,
                                    const struct mesh_outbound *control,
                                    struct mesh_relay_result *leaf_result)
{
    struct mesh_relay_result relay_result;
    int ret = c5_control_hop(state, state->gateway, state->relay,
                             control, &relay_result);
    if (ret != PROTO_OK ||
        !has_action(&relay_result, MESH_RELAY_ACTION_FORWARD) ||
        relay_result.forward.next_hop_id != LEAF_ID) {
        return ret == PROTO_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    relay_result.forward.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = c5_control_hop(state, state->relay, state->leaf,
                         &relay_result.forward, leaf_result);
    if (ret != PROTO_OK ||
        !has_action(leaf_result, MESH_RELAY_ACTION_DELIVER_LOCAL)) {
        return ret == PROTO_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    return PROTO_OK;
}

static int build_assignment_result(uint32_t epoch,
                                   enum discovery_assignment_phase phase,
                                   uint16_t seq, struct proto_packet *packet,
                                   uint8_t *payload,
                                   size_t *payload_len)
{
    size_t length = 0u;
    int ret = mesh_append_command_result(payload, UWB_MESH_MAX_PAYLOAD_LEN,
                                         &length,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_OK, 0u);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
            payload, UWB_MESH_MAX_PAYLOAD_LEN, &length, phase, epoch);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(
            payload, UWB_MESH_MAX_PAYLOAD_LEN, &length,
            discovery_assignment_hash(LEAF_ID));
    }
    if (ret == PROTO_OK) {
        ret = mesh_init_command_result(packet, LEAF_ID, GATEWAY_ID, epoch,
                                       seq, (uint8_t)length, true);
    }
    *payload_len = length;
    return ret;
}

static bool validate_assignment_delivery(const struct mesh_sim_delivery *item,
                                         uint32_t epoch,
                                         enum discovery_assignment_phase phase)
{
    enum discovery_assignment_phase decoded_phase = 0;
    enum command_id command_id = CMD_VENDOR_BASE;
    uint64_t hash = 0u;
    uint32_t decoded_epoch = 0u;
    return item->packet.msg_type == MSG_COMMAND_RESULT &&
           item->packet.src_id == LEAF_ID &&
           item->packet.session_id == epoch &&
           gateway_command_extract_id(item->payload, item->payload_len,
                                      &command_id) == PROTO_OK &&
           command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
           discovery_assignment_extract_control_tlvs(
               item->payload, item->payload_len, &decoded_phase,
               &decoded_epoch) == PROTO_OK &&
           decoded_phase == phase && decoded_epoch == epoch &&
           discovery_assignment_extract_claim_hash(
               item->payload, item->payload_len, &hash) == PROTO_OK &&
           hash == discovery_assignment_hash(LEAF_ID);
}

static int queue_and_drive(struct fixture *state,
                           const struct proto_packet *packet,
                           const uint8_t *payload,
                           size_t payload_len,
                           size_t expected_delivery_count)
{
    int ret = mesh_sim_queue_originated(&state->world, state->leaf, packet,
                                        payload, payload_len);
    return ret == MESH_SIM_OK ?
           drive_to_delivery_count(state, expected_delivery_count) : ret;
}

static bool apply_assignment_table(
    struct app_discovery_assignment_policy *policy,
    const struct mesh_outbound *control,
    enum app_discovery_assignment_table_decision expected)
{
    struct discovery_assignment_entry decoded[2];
    struct discovery_assignment_table_commitment commitment;
    enum discovery_assignment_phase phase = 0;
    uint32_t epoch = 0u;
    uint8_t slot_count = 0u;
    size_t entry_count = 0u;
    enum app_discovery_assignment_table_decision decision;
    if (discovery_assignment_extract_control_tlvs(
            control->payload, control->payload_len, &phase, &epoch) !=
            PROTO_OK || phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE ||
        discovery_assignment_parse_table_tlvs(
            control->payload, control->payload_len, decoded, 2u,
            &entry_count, &slot_count) != PROTO_OK) {
        return false;
    }
    if (!discovery_assignment_table_commitment(
            decoded, entry_count, slot_count, &commitment)) {
        return false;
    }
    decision = app_discovery_assignment_policy_note_table(
        policy, epoch, control->packet.seq, &commitment);
    if (decision != expected) {
        return false;
    }
    return decision != APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY ||
           app_discovery_assignment_policy_commit(
               policy, epoch, control->packet.seq, &commitment);
}

static int build_reach_report(uint64_t source_id, uint64_t peer_id,
                              uint32_t survey_id, uint16_t seq,
                              struct proto_packet *packet,
                              uint8_t *payload,
                              size_t *payload_len)
{
    const struct survey_reachability_entry entry = {
        .peer_id = peer_id,
        .rssi_dbm = -61,
        .quality = 92u,
    };
    int ret = survey_append_reach_report_tlvs(
        payload, UWB_MESH_MAX_PAYLOAD_LEN, payload_len, survey_id,
        source_id, &entry, 1u);
    return ret == PROTO_OK ?
           survey_init_discovery_report_packet(packet, source_id, GATEWAY_ID,
                                               survey_id, 0u, seq,
                                               (uint8_t)*payload_len) : ret;
}

static int note_reach_delivery(struct survey_gateway_context *context,
                               const struct mesh_sim_delivery *item)
{
    struct survey_reachability_entry entries[2];
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    size_t entry_count = 0u;
    int ret = survey_extract_reach_report_tlvs(
        item->payload, item->payload_len, &survey_id, &anchor_id,
        entries, 2u, &entry_count);
    if (ret != PROTO_OK ||
        item->packet.msg_type != MSG_SURVEY_DISCOVERY_REPORT ||
        item->packet.src_id != anchor_id ||
        item->packet.session_id != survey_id) {
        return PROTO_ERR_MALFORMED;
    }
    return survey_gateway_note_reach_report(context, survey_id, anchor_id,
                                            entries, entry_count);
}

static int build_pair_control(const struct survey_pair *pair, uint16_t seq,
                              struct mesh_outbound *outbound)
{
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    size_t payload_len = 0u;
    int ret;
    memset(outbound, 0, sizeof(*outbound));
    ret = survey_append_pair_tlvs(outbound->payload,
                                  sizeof(outbound->payload), &payload_len,
                                  pair);
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(
            outbound->payload, sizeof(outbound->payload), &payload_len,
            SURVEY_ROUND_ID);
    }
    if (ret == PROTO_OK) {
        proto_put_u16_le(round_commitment, SURVEY_ROUND_ID);
        ret = survey_round_commitment_append_tlv(
            outbound->payload, sizeof(outbound->payload), &payload_len,
            round_commitment);
    }
    if (ret == PROTO_OK) {
        ret = survey_init_pair_prepare_packet(&outbound->packet, pair,
                                              GATEWAY_ID, LEAF_ID, seq,
                                              (uint8_t)payload_len);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = RELAY_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static struct node_transaction_key transaction_key(uint32_t survey_id,
                                                   uint16_t seq)
{
    return (struct node_transaction_key) {
        .requester_id = GATEWAY_ID,
        .responder_id = LEAF_ID,
        .session_id = survey_id,
        .transaction_id = seq,
        .operation_id = CMD_SURVEY_PREPARE_PAIR,
    };
}

static int build_control_result(uint32_t survey_id, uint16_t seq,
                                struct proto_packet *packet,
                                uint8_t *payload,
                                size_t *payload_len)
{
    int ret = mesh_append_command_result(payload, UWB_MESH_MAX_PAYLOAD_LEN,
                                         payload_len,
                                         CMD_SURVEY_PREPARE_PAIR,
                                         COMMAND_OK, 0u);
    return ret == PROTO_OK ?
           mesh_init_command_result(packet, LEAF_ID, GATEWAY_ID, survey_id,
                                    seq, (uint8_t)*payload_len, true) : ret;
}

static int finish_transaction_delivery(
    struct survey_gateway_transaction *transaction, uint32_t handle,
    uint32_t token,
    uint64_t now_ms)
{
    struct node_comm_terminal_event event = {
        .handle = handle,
        .client_token = token,
        .reason = NODE_COMM_TERMINAL_DELIVERED,
        .attempts_started = 1u,
    };
    enum node_transaction_action action = NODE_TRANSACTION_ACTION_NONE;
    int ret = survey_gateway_transaction_note_delivery_terminal(
        transaction, &event, now_ms, &action);
    if (ret != 0 || action != NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS) {
        return ret == 0 ? -EINVAL : ret;
    }
    return survey_gateway_transaction_phase_complete(transaction);
}

static int build_pair_result(const struct survey_pair *pair, uint16_t seq,
                             struct proto_packet *packet,
                             uint8_t *payload,
                             size_t *payload_len)
{
    const struct survey_sample sample = {
        .pair = *pair,
        .sample_index = 0u,
        .distance_mm = 4210,
        .quality = 91u,
        .range_status = RANGE_OK,
    };
    int ret = survey_append_sample_tlvs(payload, UWB_MESH_MAX_PAYLOAD_LEN,
                                        payload_len, &sample);
    return ret == PROTO_OK ?
           survey_init_result_packet_from_reporter(
               packet, &sample, LEAF_ID, GATEWAY_ID, seq,
               (uint8_t)*payload_len) : ret;
}

struct lifecycle_context {
    struct fixture fixture;
    struct app_discovery_assignment_policy assignment;
    struct survey_gateway_context survey;
    struct survey_pair pair;
    struct survey_gateway_transaction transaction;
    size_t deliveries;
};
static struct lifecycle_context lifecycle;
static bool run_assignment_phase(void)
{
    struct fixture *fixture = &lifecycle.fixture;
    struct app_discovery_assignment_policy *assignment = &lifecycle.assignment;
    struct discovery_assignment_claim claim = {
        .anchor_id = LEAF_ID,
        .hash = 0u,
    };
    struct discovery_assignment_entry entry;
    struct mesh_outbound control;
    struct mesh_relay_result leaf_result;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    struct discovery_assignment_table_commitment committed_commitment;
    uint32_t committed_table_seq;
    app_discovery_assignment_policy_init(assignment, false, false, false,
                                         0u, 0u, NULL);
    claim.hash = discovery_assignment_hash(claim.anchor_id);
    CHECK(discovery_assignment_entries_from_claims(&claim, 1u, &entry, 1u) ==
              PROTO_OK,
          "assignment entry build failed");

    CHECK(build_assignment_control(ASSIGNMENT_EPOCH_1,
                                   DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                   11u, NULL, 0u, &control) == PROTO_OK &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK,
          "assignment claim control did not cross relay");
    CHECK(app_discovery_assignment_policy_note_claim(
              assignment, ASSIGNMENT_EPOCH_1) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "assignment claim was not admitted");

    payload_len = 0u;
    CHECK(build_assignment_result(ASSIGNMENT_EPOCH_1,
                                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                  21u, &packet, payload, &payload_len) ==
              PROTO_OK,
          "assignment claim result build failed");
    CHECK(mesh_sim_set_directed_rx_failures(
              &fixture->world, fixture->leaf, fixture->relay, 1u,
              MESH_SIM_RX_DECODE_ERROR) == MESH_SIM_OK,
          "assignment retry fault injection failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK,
          "assignment claim did not terminate after one lost hop");
    CHECK(validate_assignment_delivery(
              &fixture->world.roles[fixture->gateway]
                   .deliveries[lifecycle.deliveries - 1u],
              ASSIGNMENT_EPOCH_1, DISCOVERY_ASSIGNMENT_PHASE_CLAIM),
          "gateway accepted malformed assignment claim");

    CHECK(build_assignment_control(ASSIGNMENT_EPOCH_1,
                                   DISCOVERY_ASSIGNMENT_PHASE_TABLE,
                                   31u, &entry, 1u, &control) == PROTO_OK &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK &&
          apply_assignment_table(assignment, &control,
                                 APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY),
          "assignment table did not apply after relayed delivery");

    payload_len = 0u;
    CHECK(build_assignment_result(ASSIGNMENT_EPOCH_1,
                                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                  32u, &packet, payload, &payload_len) ==
              PROTO_OK,
          "assignment ACK build failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK &&
          validate_assignment_delivery(
              &fixture->world.roles[fixture->gateway]
                   .deliveries[lifecycle.deliveries - 1u],
              ASSIGNMENT_EPOCH_1, DISCOVERY_ASSIGNMENT_PHASE_ACK),
          "assignment ACK did not settle through relay");

    CHECK(build_assignment_control(ASSIGNMENT_EPOCH_2,
                                   DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                   41u, NULL, 0u, &control) == PROTO_OK &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK &&
          app_discovery_assignment_policy_note_claim(
              assignment, ASSIGNMENT_EPOCH_2) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "new assignment operation did not start");
    committed_commitment = assignment->committed_table_commitment;
    committed_table_seq = assignment->committed_table_seq;

    /* Operation N's delayed table arrives after N+1 owns the join state. */
    CHECK(build_assignment_control(ASSIGNMENT_EPOCH_1,
                                   DISCOVERY_ASSIGNMENT_PHASE_TABLE,
                                   33u, &entry, 1u, &control) == PROTO_OK &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK,
          "delayed assignment table did not traverse real relay path");
    CHECK(apply_assignment_table(
              assignment, &control,
              APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE),
          "delayed assignment table was not stale");
    CHECK(assignment->joining_epoch == ASSIGNMENT_EPOCH_2 &&
          assignment->committed_epoch == ASSIGNMENT_EPOCH_1 &&
          assignment->committed_table_seq == committed_table_seq &&
          discovery_assignment_table_commitment_equal(
              &assignment->committed_table_commitment,
              &committed_commitment),
          "operation-N table mutated operation N+1");

    CHECK(build_assignment_control(ASSIGNMENT_EPOCH_2,
                                   DISCOVERY_ASSIGNMENT_PHASE_TABLE,
                                   42u, &entry, 1u, &control) == PROTO_OK &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK &&
          apply_assignment_table(assignment, &control,
                                 APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY),
          "new assignment table did not complete");
    CHECK(assignment->provisioned &&
          assignment->committed_epoch == ASSIGNMENT_EPOCH_2 &&
          assignment->joining_epoch == 0u,
          "assignment policy did not settle provisioned");

    payload_len = 0u;
    CHECK(build_assignment_result(ASSIGNMENT_EPOCH_2,
                                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                  43u, &packet, payload, &payload_len) ==
              PROTO_OK,
          "second assignment ACK build failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK,
          "second assignment ACK did not settle");
    return true;
}

static bool run_survey_discovery_phase(void)
{
    struct fixture *fixture = &lifecycle.fixture;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    CHECK(survey_gateway_begin_operation(
              &lifecycle.survey, SURVEY_ID_1,
              SURVEY_OPERATION_GENERATION_1, 1u) == PROTO_OK,
          "survey begin failed");
    payload_len = 0u;
    CHECK(build_reach_report(LEAF_ID, RELAY_ID, SURVEY_ID_1, 51u,
                             &packet, payload, &payload_len) == PROTO_OK,
          "leaf reach report build failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK &&
          note_reach_delivery(
              &lifecycle.survey,
              &fixture->world.roles[fixture->gateway]
                   .deliveries[lifecycle.deliveries - 1u]) ==
              PROTO_OK,
          "leaf reach report did not cross forced relay");

    payload_len = 0u;
    CHECK(build_reach_report(RELAY_ID, LEAF_ID, SURVEY_ID_1, 52u,
                             &packet, payload, &payload_len) == PROTO_OK &&
          mesh_sim_queue_originated(&fixture->world, fixture->relay, &packet,
                                    payload, payload_len) == MESH_SIM_OK,
          "relay reach report build/queue failed");
    lifecycle.deliveries++;
    CHECK(drive_to_delivery_count(fixture, lifecycle.deliveries) == MESH_SIM_OK &&
          note_reach_delivery(
              &lifecycle.survey,
              &fixture->world.roles[fixture->gateway]
                   .deliveries[lifecycle.deliveries - 1u]) ==
              PROTO_OK &&
          survey_gateway_plan_pairs(&lifecycle.survey) == PROTO_OK &&
          survey_gateway_pair_at(&lifecycle.survey, 0u, &lifecycle.pair) ==
              PROTO_OK,
          "survey discovery did not produce a pair");
    CHECK((lifecycle.pair.initiator_id == LEAF_ID &&
           lifecycle.pair.responder_id == RELAY_ID) ||
          (lifecycle.pair.initiator_id == RELAY_ID &&
           lifecycle.pair.responder_id == LEAF_ID),
          "survey planner returned the wrong endpoint pair");
    return true;
}

static bool run_survey_transaction_phase(void)
{
    struct fixture *fixture = &lifecycle.fixture;
    struct survey_gateway_transaction *transaction = &lifecycle.transaction;
    struct survey_pair pair2 = lifecycle.pair;
    struct mesh_outbound control;
    struct mesh_relay_result leaf_result;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    struct node_transaction_key old_key;
    struct node_transaction_key new_key;
    enum survey_gateway_transaction_result transaction_result;
    enum node_transaction_action transaction_action;
    uint8_t old_request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t old_result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t new_request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t new_result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t zero_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    uint8_t cleanup_mask;
    const uint32_t old_handle = 101u, old_token = 1001u;
    const uint32_t new_handle = 102u, new_token = 1002u;

    survey_gateway_transaction_init(transaction);
    CHECK(survey_gateway_transaction_load_pair(transaction, &lifecycle.pair) == 0,
          "survey transaction pair load failed");
    CHECK(build_pair_control(&lifecycle.pair, 61u, &control) == PROTO_OK,
          "survey prepare control build failed");
    old_key = transaction_key(SURVEY_ID_1, 61u);
    CHECK(node_transaction_digest_bytes(
              control.payload, control.payload_len, old_request_digest),
          "survey prepare digest failed");
    CHECK(survey_gateway_transaction_begin(
              transaction, &old_key, CMD_SURVEY_PREPARE_PAIR,
              old_request_digest, old_token, old_handle,
              (fixture->world.now_us / 1000u) + 30000u,
              fixture->world.now_us / 1000u) == 0 &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK,
          "survey prepare did not cross forced relay");
    payload_len = 0u;
    CHECK(build_control_result(SURVEY_ID_1, 61u, &packet,
                               payload, &payload_len) == PROTO_OK,
          "survey control result build failed");
    CHECK(node_transaction_digest_bytes(
              payload, payload_len, old_result_digest),
          "survey control result digest failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK,
          "survey control result did not reach gateway");
    CHECK(survey_gateway_transaction_reconcile_result(
              transaction, &old_key, old_request_digest,
              old_result_digest, packet.seq, COMMAND_OK,
              fixture->world.now_us / 1000u, &transaction_result,
              &transaction_action) == 0 &&
          transaction_result ==
              SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK,
          "survey control result did not reconcile");
    CHECK(finish_transaction_delivery(
              transaction, old_handle, old_token,
              fixture->world.now_us / 1000u) == 0,
          "survey control transaction did not terminalize");

    /*
     * Model the gateway cleanup delivery reaching its absolute deadline
     * without a terminal event. Cleanup is best-effort after that bounded
     * point: the communication handle is abandoned by the Zephyr adapter and
     * the transaction must retire so operation N+1 can be admitted.
     */
    survey_gateway_transaction_require_cleanup(
        transaction, false, fixture->world.now_us / 1000u);
    cleanup_mask = survey_gateway_transaction_cleanup_mask(transaction);
    CHECK(cleanup_mask != 0u &&
          survey_gateway_transaction_note_cleanup_started(
              transaction, cleanup_mask) == 0 &&
          survey_gateway_transaction_note_cleanup_complete(
              transaction, cleanup_mask,
              (fixture->world.now_us / 1000u) + 30001u) == 0 &&
          !survey_gateway_transaction_cleanup_pending(transaction),
          "missing cleanup terminal kept operation N busy past its deadline");
    survey_gateway_transaction_pair_complete(
        transaction, true, (fixture->world.now_us / 1000u) + 30001u);

    pair2.survey_id = SURVEY_ID_2;
    pair2.operation_generation = SURVEY_OPERATION_GENERATION_2;
    CHECK(survey_gateway_transaction_load_pair(transaction, &pair2) == 0 &&
          build_pair_control(&pair2, 62u, &control) == PROTO_OK,
          "new survey operation setup failed");
    new_key = transaction_key(SURVEY_ID_2, 62u);
    CHECK(node_transaction_digest_bytes(
              control.payload, control.payload_len, new_request_digest),
          "new survey control digest failed");
    CHECK(survey_gateway_transaction_begin(
              transaction, &new_key, CMD_SURVEY_PREPARE_PAIR,
              new_request_digest, new_token, new_handle,
              (fixture->world.now_us / 1000u) + 30000u,
              fixture->world.now_us / 1000u) == 0 &&
          deliver_targeted_control(fixture, &control, &leaf_result) ==
              PROTO_OK,
          "new survey control did not start");

    /* The delayed, byte-identical N result must be an inert duplicate. */
    CHECK(survey_gateway_transaction_reconcile_result(
              transaction, &old_key, old_request_digest,
              old_result_digest, 61u, COMMAND_OK,
              fixture->world.now_us / 1000u, &transaction_result,
              &transaction_action) == 0 &&
          transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE &&
          transaction->active.state == NODE_TRANSACTION_ACTIVE &&
          node_transaction_key_equal(&transaction->active.spec.key, &new_key) &&
          semantic_digest_equal(transaction->active.accepted_result_digest,
                                zero_digest,
                                sizeof(zero_digest)) &&
          !transaction->abandoning && !transaction->conflict,
          "operation-N result mutated operation N+1");

    payload_len = 0u;
    CHECK(build_control_result(SURVEY_ID_2, 62u, &packet,
                               payload, &payload_len) == PROTO_OK,
          "new survey result build failed");
    CHECK(node_transaction_digest_bytes(
              payload, payload_len, new_result_digest),
          "new survey result digest failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK &&
          survey_gateway_transaction_reconcile_result(
              transaction, &new_key, new_request_digest,
              new_result_digest, packet.seq, COMMAND_OK,
              fixture->world.now_us / 1000u, &transaction_result,
              &transaction_action) == 0 &&
          transaction_result ==
              SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK &&
          finish_transaction_delivery(
              transaction, new_handle, new_token,
              fixture->world.now_us / 1000u) == 0,
          "operation N+1 did not complete after stale result");
    survey_gateway_transaction_pair_complete(
        transaction, true, fixture->world.now_us / 1000u);

    payload_len = 0u;
    CHECK(build_pair_result(&pair2, 71u, &packet,
                            payload, &payload_len) == PROTO_OK,
          "survey sample build failed");
    lifecycle.deliveries++;
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK,
          "survey sample did not cross forced relay");
    CHECK(fixture->world.roles[fixture->gateway]
              .deliveries[lifecycle.deliveries - 1u]
              .packet.msg_type == MSG_SURVEY_PAIR_RESULT &&
          fixture->world.roles[fixture->gateway]
              .deliveries[lifecycle.deliveries - 1u]
              .packet.session_id == SURVEY_ID_2,
          "gateway did not receive the new survey sample");

    /* An exact RF replay is ACKed idempotently and not delivered twice. */
    CHECK(queue_and_drive(fixture, &packet, payload, payload_len,
                          lifecycle.deliveries) == MESH_SIM_OK &&
          fixture->world.roles[fixture->gateway].delivery_count ==
              lifecycle.deliveries,
          "duplicate survey sample produced a second semantic delivery");
    return true;
}

static bool run_lifecycle(void)
{
    struct fixture *fixture = &lifecycle.fixture;
    uint64_t retries;

    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(setup_forced_relay(fixture) == MESH_SIM_OK,
          "forced-relay setup failed");
    CHECK(fixture->world.connection_count == 1u,
          "gateway incorrectly owns a recurring Channel-9 connection");
    CHECK(install_gateway_route_advertisement(fixture) == MESH_SIM_OK,
          "gateway route advertisement did not install the forced route");
    CHECK(!fixture->world.reachable[fixture->leaf][fixture->gateway],
          "fixture accidentally permits direct leaf-to-gateway RF");
    if (!run_assignment_phase() || !run_survey_discovery_phase() ||
        !run_survey_transaction_phase()) {
        return false;
    }

    retries = mesh_sim_count_transitions(
        &fixture->world, MESH_SIM_TRANSITION_RETRY_READY, 0u);
    CHECK(retries > 0u && retries <= 8u,
          "retries are absent or unbounded count=%" PRIu64, retries);
    CHECK(mesh_sim_count_transitions(
              &fixture->world, MESH_SIM_TRANSITION_ROUTE_REQUIRED, 0u) == 0u,
          "preinstalled gateway route advertisement fell into discovery");
    CHECK(fixture->world.connection_event_total_count < MAX_DRIVE_STEPS,
          "connection polling exceeded lifecycle bound count=%" PRIu64,
          fixture->world.connection_event_total_count);
    CHECK(fixture->direct_gateway_turns > 0u &&
          fixture->direct_gateway_ack_turns ==
              fixture->direct_gateway_turns,
          "direct gateway turn/ACK ownership disagrees turns=%" PRIu64
          " acks=%" PRIu64,
          fixture->direct_gateway_turns,
          fixture->direct_gateway_ack_turns);
    CHECK(fixture->max_direct_gateway_ack_turnaround_us <=
              (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u,
          "gateway ACK escaped same radio turn turnaround=%" PRIu64,
          fixture->max_direct_gateway_ack_turnaround_us);
    {
        struct mesh_sim_invariant_report report;

        CHECK(mesh_sim_check_settled(&fixture->world, &report) == MESH_SIM_OK,
              "lifecycle did not settle invariant=%s node=%zu reason=%s",
              mesh_sim_invariant_name(report.code), report.node_index,
              report.description == NULL ? "unknown" : report.description);
    }
    return true;
}

int main(void)
{
    if (!run_lifecycle()) {
        return EXIT_FAILURE;
    }
    printf("PASS protocol_lifecycle here-i-am assignment survey forced-relay "
           "stale-generation bounded direct_gateway_turns=%" PRIu64
           " max_ack_turnaround_us=%" PRIu64 "\n",
           lifecycle.fixture.direct_gateway_turns,
           lifecycle.fixture.max_direct_gateway_ack_turnaround_us);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
