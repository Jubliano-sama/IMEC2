#include "mesh_sim.h"

#include "report.h"

#include <stdio.h>
#include <string.h>

#define ROUTE_EPOCH UINT32_C(7)
#define GATEWAY_ID UINT64_C(0x9000)
#define TRANSMITTER_ID UINT64_C(0xB001)
#define CLICKER_ID UINT64_C(0xC001)
#define ANCHOR_ID_BASE UINT64_C(0xA100)
#define SCENARIO_SEED_LINE UINT32_C(0x61A0E001)
#define SCENARIO_SEED_CLICK UINT32_C(0x61A0E100)
#define SCENARIO_SEED_EMPTY UINT32_C(0x61A0E200)
#define SCENARIO_SEED_WATCHDOG UINT32_C(0x61A0E300)
#define LINE_PACKET_COUNT 3u
#define LINE_MAX_RELAYS (MESH_DEFAULT_TTL - 1u)
#define SCENARIO_EVENT_MAX_MISSES 3u
#define STRESS_WATCHDOG_US MESH_SIM_WATCHDOG_PRODUCTION_TIMEOUT_US
#define DIRECT_GATEWAY_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_GATEWAY_PAYLOAD_SERVICE_US UINT64_C(50000)
#define DIRECT_GATEWAY_ACK_GUARD_US UINT64_C(10000)
#define DIRECT_GATEWAY_ACK_SERVICE_US UINT64_C(40000)
#define DIRECT_GATEWAY_RX_COMPLETION_GUARD_US UINT64_C(1)
#define DIRECT_GATEWAY_TURN_RESERVATION_US \
    (DIRECT_GATEWAY_TX_PREPARE_US + DIRECT_GATEWAY_PAYLOAD_SERVICE_US + \
     DIRECT_GATEWAY_RX_COMPLETION_GUARD_US + DIRECT_GATEWAY_ACK_GUARD_US + \
     DIRECT_GATEWAY_ACK_SERVICE_US)

#define REQUIRE(scenario, seed, expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "scenario=%s seed=0x%08x line=%d assertion=%s\n", \
                scenario, (unsigned int)(seed), __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static struct mesh_event_params connection_params(uint32_t first_event_ms,
                                                  uint32_t interval_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = interval_ms,
        .event_window_ms = 25u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = SCENARIO_EVENT_MAX_MISSES,
        .supervision_timeout_ms = 20000u,
    };
}

static struct proto_packet data_packet(uint16_t seq, uint16_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x51000000) + seq,
        .seq = seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = payload_len,
    };
}

static size_t data_payload(uint8_t *payload, size_t cap, uint32_t packet_id)
{
    size_t length = 0u;

    if (tlv_append_u32(payload, cap, &length, TLV_MESH_TEST_PACKET_ID,
                       packet_id) != PROTO_OK ||
        tlv_append_u8(payload, cap, &length, TLV_MESH_TEST_ATTEMPT,
                      1u) != PROTO_OK) {
        return 0u;
    }
    return length;
}

static int run_next_connection(struct mesh_sim_world *world,
                               uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret;

    ret = mesh_sim_connection_next_action(world, connection_index, &action);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (!action.already_scheduled &&
        mesh_sim_schedule_next_connection_event(world, connection_index,
                                                false) != MESH_SIM_OK) {
        return world->last_error;
    }
    return mesh_sim_run_until(world, action.end_us);
}

static int run_earliest_connection(struct mesh_sim_world *world,
                                   const uint16_t *connections,
                                   size_t connection_count)
{
    size_t selected = SIZE_MAX;
    size_t payload_selected = SIZE_MAX;
    uint64_t earliest_us = UINT64_MAX;
    uint64_t earliest_payload_us = UINT64_MAX;
    int first_error = MESH_SIM_OK;

    for (size_t i = 0u; i < connection_count; i++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(world,
                                                  connections[i],
                                                  &action);

        if (ret != MESH_SIM_OK) {
            if (first_error == MESH_SIM_OK) {
                first_error = ret;
            }
            continue;
        }
        if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
            continue;
        }
        if (action.start_us >= world->now_us &&
            action.start_us < earliest_us) {
            earliest_us = action.start_us;
            selected = i;
        }
        if (action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT) {
            const struct mesh_sim_connection *connection =
                &world->connections[connections[i]];
            bool node_a_tx = mesh_event_timing_local_tx_slot(
                &connection->timing_a);
            uint8_t sender_index;
            uint8_t receiver_index;
            bool has_runnable_payload = false;

            if ((action.skipped_events & 1u) != 0u) {
                node_a_tx = !node_a_tx;
            }
            sender_index = node_a_tx ? connection->node_a : connection->node_b;
            receiver_index = node_a_tx ? connection->node_b : connection->node_a;
            for (size_t queued = 0u;
                 queued < MESH_SIM_TX_QUEUE_CAPACITY;
                 queued++) {
                const struct mesh_sim_queued_tx *entry =
                    &world->roles[sender_index].tx_queue[queued];

                if (!entry->valid ||
                    entry->outbound.next_hop_id !=
                        world->roles[receiver_index].id) {
                    continue;
                }
                if (!entry->needs_relay_start ||
                    world->roles[sender_index].relay.pending.state ==
                        MESH_RELAY_TX_IDLE ||
                    (entry->outbound.packet.msg_type ==
                         world->roles[sender_index].relay.pending.packet.msg_type &&
                     entry->outbound.packet.src_id ==
                         world->roles[sender_index].relay.pending.packet.src_id &&
                     entry->outbound.packet.dst_id ==
                         world->roles[sender_index].relay.pending.packet.dst_id &&
                     entry->outbound.packet.session_id ==
                         world->roles[sender_index].relay.pending.packet.session_id &&
                     entry->outbound.packet.seq ==
                         world->roles[sender_index].relay.pending.packet.seq)) {
                    has_runnable_payload = true;
                    break;
                }
            }
            if (has_runnable_payload && action.start_us >= world->now_us &&
                action.start_us < earliest_payload_us) {
                earliest_payload_us = action.start_us;
                payload_selected = i;
            }
        }
    }
    if (payload_selected != SIZE_MAX) {
        return run_next_connection(world, connections[payload_selected]);
    }
    return selected == SIZE_MAX ?
           (first_error == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : first_error) :
           run_next_connection(world, connections[selected]);
}

static bool queued_for_gateway(const struct mesh_sim_role_instance *node)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            node->tx_queue[i].outbound.next_hop_id == GATEWAY_ID) {
            return true;
        }
    }
    return false;
}

static uint64_t transmission_arrival_end_us(
    const struct mesh_sim_world *world,
    uint16_t transmission_index,
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

static int run_direct_gateway_turn(struct mesh_sim_world *world,
                                   uint8_t sender,
                                   uint8_t gateway)
{
    uint64_t ready_us = world->now_us;
    uint64_t air_start_us;
    uint64_t payload_deadline_us;
    uint64_t arrival_end_us;
    uint64_t rx_end_us;
    uint64_t ack_start_us;
    uint64_t ack_end_us;
    uint64_t turn_deadline_us;
    uint16_t transmission_index;
    int ret;

    if (world->roles[sender].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world->roles[sender].dwm3000.cpu_busy_until_us;
    }
    if (world->roles[gateway].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world->roles[gateway].dwm3000.cpu_busy_until_us;
    }
    if (ready_us > world->now_us) {
        ret = mesh_sim_run_until(world, ready_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    turn_deadline_us = world->now_us + DIRECT_GATEWAY_TURN_RESERVATION_US;
    air_start_us = world->now_us + DIRECT_GATEWAY_TX_PREPARE_US;
    payload_deadline_us =
        air_start_us + DIRECT_GATEWAY_PAYLOAD_SERVICE_US;
    ret = mesh_sim_direct_gateway_start_queued_tx(
        world, sender, air_start_us, payload_deadline_us,
        &transmission_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    arrival_end_us = transmission_arrival_end_us(
        world, transmission_index, gateway);
    if (arrival_end_us == UINT64_MAX) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    rx_end_us = arrival_end_us + DIRECT_GATEWAY_RX_COMPLETION_GUARD_US;
    ret = mesh_sim_direct_gateway_arm_rx(world, gateway,
                                         air_start_us, rx_end_us);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, rx_end_us);
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (world->roles[gateway].dwm3000.cpu_busy_until_us > world->now_us) {
        ret = mesh_sim_run_until(
            world, world->roles[gateway].dwm3000.cpu_busy_until_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    ack_start_us = world->now_us + DIRECT_GATEWAY_ACK_GUARD_US;
    ack_end_us = ack_start_us + DIRECT_GATEWAY_ACK_SERVICE_US;
    ret = mesh_sim_direct_gateway_schedule_ack(
        world, gateway, sender, ack_start_us, ack_end_us, NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, ack_end_us);
    }
    if (ret == MESH_SIM_OK && world->now_us > turn_deadline_us) {
        ret = MESH_SIM_ERR_RADIO_DEADLINE;
    }
    return ret;
}

static bool direct_gateway_turn_fits(const struct mesh_sim_world *world,
                                     uint8_t sender)
{
    const uint64_t required_end_us =
        world->now_us + DIRECT_GATEWAY_TURN_RESERVATION_US;

    for (size_t i = 0u; i < world->connection_count; i++) {
        const struct mesh_sim_connection *connection = &world->connections[i];
        struct mesh_sim_connection_action action;

        if (connection->node_a != sender && connection->node_b != sender) {
            continue;
        }

        if (mesh_sim_connection_next_action(world, (uint16_t)i, &action) !=
                MESH_SIM_OK ||
            action.already_scheduled ||
            (action.kind != MESH_SIM_CONNECTION_ACTION_NONE &&
             action.start_us < required_end_us)) {
            return false;
        }
    }
    return true;
}

static int arm_watchdogs(struct mesh_sim_world *world)
{
    for (uint8_t i = 0u; i < world->role_count; i++) {
        if (mesh_sim_watchdog_arm(world, i, STRESS_WATCHDOG_US,
                                  MESH_SIM_WATCHDOG_FAIL) != MESH_SIM_OK) {
            return world->last_error;
        }
    }
    return MESH_SIM_OK;
}

static int no_watchdog_expired(const struct mesh_sim_world *world)
{
    if (mesh_sim_count_transitions(world,
                                   MESH_SIM_TRANSITION_WATCHDOG_EXPIRED,
                                   0u) != 0u) {
        return 0;
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].watchdog.expirations != 0u) {
            return 0;
        }
    }
    return 1;
}

static int no_route_discovery(const struct mesh_sim_world *world)
{
    if (mesh_sim_count_transitions(world,
                                   MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                   0u) != 0u) {
        return 0;
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].route_discovery_requests != 0u) {
            return 0;
        }
    }
    return 1;
}

static bool all_relay_custody_settled(const struct mesh_sim_world *world)
{
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].relay.pending.state != MESH_RELAY_TX_IDLE ||
            world->roles[i].tx_queue_count != 0u) {
            return false;
        }
    }
    return true;
}

static size_t count_transitions_for_message(
    const struct mesh_sim_world *world,
    enum mesh_sim_transition_kind kind,
    uint64_t node_id,
    uint8_t msg_type)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->transition_count; i++) {
        const struct mesh_sim_transition *transition = &world->transitions[i];

        if (transition->kind == kind && transition->node_id == node_id &&
            transition->msg_type == msg_type) {
            count++;
        }
    }
    return count;
}

static void print_line_failure(const struct mesh_sim_world *world,
                               uint8_t relay_count)
{
    const struct mesh_sim_reception *last_ack = NULL;

    for (size_t i = 0u; i < world->reception_count; i++) {
        if (world->receptions[i].packet.msg_type == MSG_GATEWAY_ACK) {
            last_ack = &world->receptions[i];
        }
    }
    fprintf(stderr,
            "line-depth=%u error=%d origin-pending=%d gateway-deliveries=%zu "
            "route-required=%u now-us=%llu\n",
            relay_count, world->last_error, world->roles[0].relay.pending.state,
            world->roles[relay_count + 1u].delivery_count,
            world->roles[0].route_discovery_requests,
            (unsigned long long)world->now_us);
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].watchdog.expirations != 0u) {
            fprintf(stderr,
                    "watchdog node=%llx expirations=%u deadline-us=%llu "
                    "queue=%zu pending=%d\n",
                    (unsigned long long)world->roles[i].id,
                    world->roles[i].watchdog.expirations,
                    (unsigned long long)world->roles[i].watchdog.deadline_us,
                    world->roles[i].tx_queue_count,
                    world->roles[i].relay.pending.state);
        }
    }
    if (last_ack != NULL) {
        fprintf(stderr,
                "last-gateway-ack receiver=%llx source=%llx sess=%u seq=%u ttl=%u outcome=%d at_us=%llu\n",
                (unsigned long long)last_ack->receiver_id,
                (unsigned long long)last_ack->source_id,
                last_ack->packet.session_id, last_ack->packet.seq,
                last_ack->packet.ttl, last_ack->outcome,
                (unsigned long long)last_ack->end_us);
    }
    for (size_t i = 0u; i < world->transition_count; i++) {
        const struct mesh_sim_transition *transition = &world->transitions[i];

        if (transition->kind == MESH_SIM_TRANSITION_GATEWAY_ACKED) {
            fprintf(stderr,
                    "gateway-acked node=%llx at-us=%llu msg=%u sess=%u seq=%u\n",
                    (unsigned long long)transition->node_id,
                    (unsigned long long)transition->time_us,
                    transition->msg_type,
                    transition->packet_session_id,
                    transition->packet_seq);
        }
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        fprintf(stderr,
                "role node=%llx queue=%zu pending=%d next=%llx\n",
                (unsigned long long)world->roles[i].id,
                world->roles[i].tx_queue_count,
                world->roles[i].relay.pending.state,
                (unsigned long long)world->roles[i].relay.pending.next_hop_id);
        for (size_t queued = 0u;
             queued < MESH_SIM_TX_QUEUE_CAPACITY;
             queued++) {
            const struct mesh_sim_queued_tx *entry =
                &world->roles[i].tx_queue[queued];

            if (entry->valid) {
                fprintf(stderr,
                        "  queued msg=%u src=%llx session=%u seq=%u next=%llx start=%u\n",
                        entry->outbound.packet.msg_type,
                        (unsigned long long)entry->outbound.packet.src_id,
                        entry->outbound.packet.session_id,
                        entry->outbound.packet.seq,
                        (unsigned long long)entry->outbound.next_hop_id,
                        entry->needs_relay_start ? 1u : 0u);
            }
        }
    }
    for (size_t i = 0u; i < world->connection_count; i++) {
        struct mesh_sim_connection_action action;
        int action_ret = mesh_sim_connection_next_action(world,
                                                         (uint16_t)i,
                                                         &action);

        fprintf(stderr,
                "connection=%zu action-ret=%d kind=%d fresh=%u/%u "
                "next=%u/%u repairs=%u misses=%u/%u\n",
                i,
                action_ret,
                action_ret == MESH_SIM_OK ? (int)action.kind : -1,
                world->connections[i].timing_a.timing_fresh,
                world->connections[i].timing_b.timing_fresh,
                world->connections[i].timing_a.next_event_time_ms,
                world->connections[i].timing_b.next_event_time_ms,
                world->connections[i].completed_repairs,
                world->connections[i].diagnostics_a.ch9_event_misses,
                world->connections[i].diagnostics_b.ch9_event_misses);
    }
}

static int run_line_until_confirmed(struct mesh_sim_world *world,
                                    const uint16_t *connections,
                                    size_t connection_count,
                                    uint8_t transmitter,
                                    uint8_t final_relay,
                                    uint8_t gateway,
                                    size_t expected_deliveries)
{
    const unsigned int max_events =
        128u * (unsigned int)(connection_count + 1u);
    unsigned int direct_turns = 0u;
    unsigned int connection_turns = 0u;
    unsigned int queued_seen = 0u;
    unsigned int fit_false = 0u;

    for (unsigned int event = 0u; event < max_events; event++) {
        int ret;

        if (world->roles[gateway].delivery_count >= expected_deliveries &&
            world->roles[transmitter].relay.pending.state == MESH_RELAY_TX_IDLE &&
            all_relay_custody_settled(world)) {
            return MESH_SIM_OK;
        }
        if (queued_for_gateway(&world->roles[final_relay])) {
            queued_seen++;
        }
        if (queued_for_gateway(&world->roles[final_relay]) &&
            direct_gateway_turn_fits(world, final_relay)) {
            direct_turns++;
            ret = run_direct_gateway_turn(world, final_relay, gateway);
        } else {
            if (queued_for_gateway(&world->roles[final_relay])) {
                fit_false++;
            }
            connection_turns++;
            ret = run_earliest_connection(world, connections, connection_count);
        }
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    fprintf(stderr, "line-driver turns direct=%u conn=%u queued=%u fitfalse=%u now=%llu\\n",
            direct_turns, connection_turns, queued_seen, fit_false,
            (unsigned long long)world->now_us);
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int test_line_depth(uint8_t relay_count)
{
    static struct mesh_sim_world world;
    uint8_t nodes[LINE_MAX_RELAYS + 2u];
    uint16_t connections[LINE_MAX_RELAYS + 1u];
    uint8_t payload[32];
    uint8_t gateway;
    uint64_t terminal_owner_id;
    uint32_t seed = SCENARIO_SEED_LINE + relay_count;

    mesh_sim_init(&world, seed);
    REQUIRE("line_topology", seed,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                              TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &nodes[0]) == MESH_SIM_OK);
    for (uint8_t i = 0u; i < relay_count; i++) {
        REQUIRE("line_topology", seed,
                mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                  ANCHOR_ID_BASE + i, GATEWAY_ID,
                                  ROUTE_EPOCH, &nodes[i + 1u]) == MESH_SIM_OK);
    }
    REQUIRE("line_topology", seed,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    nodes[relay_count + 1u] = gateway;
    terminal_owner_id = relay_count == 0u ?
        TRANSMITTER_ID : ANCHOR_ID_BASE + relay_count - 1u;

    for (uint8_t i = 0u; i <= relay_count; i++) {
        REQUIRE("line_topology", seed,
                mesh_sim_set_link(&world, nodes[i], nodes[i + 1u],
                                  95u, 1u) == MESH_SIM_OK);
        if (i < relay_count) {
            struct mesh_event_params params = connection_params(
                50u + (uint32_t)i * 60u,
                MESH_RADIO_EVENT_INTERVAL_MS);

            REQUIRE("line_topology", seed,
                    mesh_sim_add_connection(
                        &world, nodes[i], nodes[i + 1u],
                        &params, true, &connections[i]) == MESH_SIM_OK);
        }
        REQUIRE("line_topology", seed,
                mesh_sim_install_route(&world, nodes[i], nodes[i + 1u],
                                       relay_count - i,
                                       ROUTE_EPOCH) == PROTO_OK);
        if (i > 0u) {
            REQUIRE("line_topology", seed,
                    mesh_sim_install_downlink(&world, nodes[i],
                                              TRANSMITTER_ID, nodes[i - 1u],
                                              i, ROUTE_EPOCH) == MESH_SIM_OK);
        }
    }
    REQUIRE("line_topology", seed, arm_watchdogs(&world) == MESH_SIM_OK);

    for (uint16_t seq = 1u; seq <= LINE_PACKET_COUNT; seq++) {
        struct proto_packet packet;
        size_t payload_len = data_payload(payload, sizeof(payload), seq);
        int ret;

        REQUIRE("line_topology", seed, payload_len != 0u);
        packet = data_packet(seq, (uint16_t)payload_len);
        REQUIRE("line_topology", seed,
                mesh_sim_queue_originated(&world, nodes[0], &packet,
                                          payload, payload_len) == MESH_SIM_OK);
        ret = run_line_until_confirmed(&world, connections,
                                       relay_count, nodes[0],
                                       nodes[relay_count], gateway, seq);
        if (ret != MESH_SIM_OK) {
            fprintf(stderr, "line-run-ret=%d seq=%u\n", ret, seq);
            print_line_failure(&world, relay_count);
        }
        REQUIRE("line_topology", seed, ret == MESH_SIM_OK);
        REQUIRE("line_topology", seed,
                world.roles[gateway].delivery_count == seq &&
                world.roles[gateway].deliveries[seq - 1u].packet.seq == seq);
        if (mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                       terminal_owner_id) != seq) {
            print_line_failure(&world, relay_count);
        }
        REQUIRE("line_topology", seed,
                mesh_sim_count_transitions(&world,
                                           MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                           terminal_owner_id) == seq);
        REQUIRE("line_topology", seed,
                count_transitions_for_message(
                    &world, MESH_SIM_TRANSITION_TX_START,
                    terminal_owner_id, MSG_GATEWAY_ACK_CONFIRM) == seq);
        for (uint8_t child = 0u; child < relay_count; child++) {
            REQUIRE("line_topology", seed,
                    count_transitions_for_message(
                        &world, MESH_SIM_TRANSITION_TX_START,
                        world.roles[nodes[child + 1u]].id,
                        MSG_MESH_HOP_ACK) == seq);
            REQUIRE("line_topology", seed,
                    count_transitions_for_message(
                        &world, MESH_SIM_TRANSITION_TX_START,
                        world.roles[nodes[child]].id,
                        MSG_GATEWAY_ACK_CONFIRM) == 0u);
        }
    }
    REQUIRE("line_topology", seed,
            world.last_error == MESH_SIM_OK &&
            mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                       0u) == 0u &&
            no_watchdog_expired(&world));
    return 0;
}

static int test_click_preemption_and_retry(void)
{
    static struct mesh_sim_world world;
    uint8_t transmitter;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t child;
    uint16_t upstream;
    uint16_t connections[2];
    uint8_t transit_payload[32];
    uint8_t click_payload[160];
    size_t transit_len;
    size_t click_len = 0u;
    struct proto_packet transit_packet;
    struct proto_packet click_packet;
    struct mesh_event_params child_params = connection_params(100u, 900u);
    struct mesh_event_params upstream_params = connection_params(100u, 850u);
    uint32_t timeout_ms;
    uint32_t retry_at_ms;
    size_t data_tx_starts;
    size_t propose_tx_starts;
    size_t accept_tx_starts;
    size_t repair_starts;
    size_t repaired_connections;
    const int32_t distance_samples_mm[] = {1234};
    const uint8_t range_round_indices[] = {0u};
    const uint64_t sequence_start_timestamps_ms[] = {100u};
    const uint64_t participant_anchor_ids[] = {
        ANCHOR_ID_BASE,
        ANCHOR_ID_BASE + 1u,
    };
    const struct range_report_fields fields = {
        .clicker_id = CLICKER_ID,
        .anchor_id = ANCHOR_ID_BASE,
        .event_seq = UINT32_C(0x52000001),
        .timestamp_ms = 100u,
        .distance_mm = 1234,
        .quality = 99u,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples_mm,
        .range_round_indices = range_round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = 1u,
        .distance_sample_count = 1u,
        .participant_anchor_ids = participant_anchor_ids,
        .participant_anchor_count = 2u,
        .burst_id = UINT32_C(0x52000001),
        .omit_rsl = true,
        .omit_cir = true,
        .burst_id_present = true,
    };

    mesh_sim_init(&world, SCENARIO_SEED_CLICK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                              TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &transmitter) == MESH_SIM_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              ANCHOR_ID_BASE, GATEWAY_ID, ROUTE_EPOCH,
                              &anchor) == MESH_SIM_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                              GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &gateway) == MESH_SIM_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_set_link(&world, transmitter, anchor, 98u, 0u) == MESH_SIM_OK &&
            mesh_sim_set_link(&world, anchor, gateway, 98u, 0u) == MESH_SIM_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_install_route(&world, transmitter, anchor, 1u,
                                   ROUTE_EPOCH) == PROTO_OK &&
            mesh_sim_install_route(&world, anchor, gateway, 0u,
                                   ROUTE_EPOCH) == PROTO_OK &&
            mesh_sim_install_downlink(&world, anchor, TRANSMITTER_ID,
                                      transmitter, 1u,
                                      ROUTE_EPOCH) == MESH_SIM_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_add_connection(&world, transmitter, anchor,
                                    &child_params, true, &child) == MESH_SIM_OK &&
            mesh_sim_add_connection(&world, anchor, gateway,
                                    &upstream_params, true,
                                    &upstream) == MESH_SIM_OK);
    connections[0] = child;
    connections[1] = upstream;
    transit_len = data_payload(transit_payload, sizeof(transit_payload), 1u);
    transit_packet = data_packet(1u, (uint16_t)transit_len);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            report_append_range_tlvs(click_payload, sizeof(click_payload),
                                     &click_len, &fields) == PROTO_OK &&
            click_len <= UINT8_MAX &&
            report_init_click_packet(&click_packet, ANCHOR_ID_BASE,
                                     GATEWAY_ID,
                                     proto_click_report_session_id(
                                         fields.clicker_id,
                                         fields.event_seq),
                                     1u,
                                     (uint8_t)click_len) == PROTO_OK &&
            report_validate_click_payload(&click_packet, click_payload,
                                          click_len) == PROTO_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_queue_originated(&world, transmitter, &transit_packet,
                                      transit_payload, transit_len) == MESH_SIM_OK &&
            mesh_sim_queue_originated(&world, anchor, &click_packet,
                                      click_payload, click_len) == MESH_SIM_OK &&
            arm_watchdogs(&world) == MESH_SIM_OK);

    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_schedule_next_connection_event(&world, child,
                                                    true) == MESH_SIM_OK &&
            mesh_sim_schedule_next_connection_event(&world, upstream,
                                                    false) == MESH_SIM_OK &&
            mesh_sim_run_until(&world, 125000u) == MESH_SIM_OK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            world.roles[gateway].delivery_count == 1u &&
            world.roles[gateway].deliveries[0].packet.msg_type == MSG_CLICK_REPORT &&
            world.roles[transmitter].relay.pending.state ==
                MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_CONNECTION_PREEMPTED,
                                       ANCHOR_ID_BASE) == 1u);

    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            run_next_connection(&world, upstream) == MESH_SIM_OK);
    timeout_ms = world.roles[transmitter].relay.pending.gateway_ack_deadline_ms;
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            timeout_ms > 100u &&
            mesh_sim_schedule_relay_tick(&world, transmitter,
                                         (uint64_t)timeout_ms * 1000u) == MESH_SIM_OK);
    while (world.now_us < (uint64_t)timeout_ms * 1000u) {
        REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
                run_earliest_connection(&world, connections, 2u) == MESH_SIM_OK);
    }
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            world.roles[transmitter].relay.pending.state ==
                MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_at_ms = world.roles[transmitter].relay.pending.retry_after_ms;
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            retry_at_ms > timeout_ms &&
            mesh_sim_schedule_relay_tick(&world, transmitter,
                                         (uint64_t)retry_at_ms * 1000u) == MESH_SIM_OK);
    while (world.now_us < (uint64_t)retry_at_ms * 1000u) {
        REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
                run_earliest_connection(&world, connections, 2u) == MESH_SIM_OK);
    }
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_RETRY_READY,
                                       TRANSMITTER_ID) == 1u);
    for (unsigned int event = 0u;
         event < 48u &&
             (world.roles[gateway].delivery_count != 2u ||
              !all_relay_custody_settled(&world));
         event++) {
        int ret = run_earliest_connection(&world, connections, 2u);

        if (ret != MESH_SIM_OK) {
            fprintf(stderr,
                    "click-recovery blocked error=%d pending=%d child-fresh=%u/%u "
                    "child-misses=%u/%u watchdog-expiry=%u/%u/%u\n",
                    world.last_error,
                    world.roles[transmitter].relay.pending.state,
                    world.connections[child].timing_a.timing_fresh,
                    world.connections[child].timing_b.timing_fresh,
                    world.connections[child].diagnostics_a.ch9_event_misses,
                    world.connections[child].diagnostics_b.ch9_event_misses,
                    world.roles[transmitter].watchdog.expirations,
                    world.roles[anchor].watchdog.expirations,
                    world.roles[gateway].watchdog.expirations);
        }
        REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
                ret == MESH_SIM_OK);
    }
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            world.roles[transmitter].relay.pending.state == MESH_RELAY_TX_IDLE &&
            world.roles[gateway].delivery_count == 2u &&
            world.roles[gateway].deliveries[0].packet.msg_type == MSG_CLICK_REPORT &&
            world.roles[gateway].deliveries[0].packet.src_id == click_packet.src_id &&
            world.roles[gateway].deliveries[0].packet.dst_id == click_packet.dst_id &&
            world.roles[gateway].deliveries[0].packet.session_id == click_packet.session_id &&
            world.roles[gateway].deliveries[0].packet.seq == click_packet.seq &&
            world.roles[gateway].deliveries[1].packet.msg_type == MSG_MESH_DATA &&
            world.roles[gateway].deliveries[1].packet.src_id == transit_packet.src_id &&
            world.roles[gateway].deliveries[1].packet.dst_id == transit_packet.dst_id &&
            world.roles[gateway].deliveries[1].packet.session_id == transit_packet.session_id &&
            world.roles[gateway].deliveries[1].packet.seq == transit_packet.seq);
    data_tx_starts = count_transitions_for_message(
        &world, MESH_SIM_TRANSITION_TX_START, TRANSMITTER_ID, MSG_MESH_DATA);
    propose_tx_starts = count_transitions_for_message(
        &world, MESH_SIM_TRANSITION_TX_START, TRANSMITTER_ID,
        MSG_MESH_EVENT_PROPOSE);
    accept_tx_starts = count_transitions_for_message(
        &world, MESH_SIM_TRANSITION_TX_START, ANCHOR_ID_BASE,
        MSG_MESH_EVENT_ACCEPT);
    repair_starts = mesh_sim_count_transitions(
        &world, MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED, TRANSMITTER_ID);
    repaired_connections = mesh_sim_count_transitions(
        &world, MESH_SIM_TRANSITION_CONNECTION_REPAIRED, TRANSMITTER_ID);
    if (data_tx_starts != 2u || propose_tx_starts != 0u ||
        accept_tx_starts != 0u || repair_starts != 0u ||
        repaired_connections != 0u ||
        world.connections[child].completed_repairs != 0u ||
        world.now_us > ((uint64_t)retry_at_ms + 8500u) * 1000u ||
        !no_route_discovery(&world) ||
        !no_watchdog_expired(&world)) {
        fprintf(stderr,
                "click-recovery data-tx=%zu control-tx(propose=%zu accept=%zu) "
                "repair(start=%zu done=%zu connection=%u) "
                "now-us=%llu retry-at-ms=%u routes=%u/%u/%u required=%zu watchdog=%u/%u/%u\n",
                data_tx_starts,
                propose_tx_starts,
                accept_tx_starts,
                repair_starts,
                repaired_connections,
                world.connections[child].completed_repairs,
                (unsigned long long)world.now_us,
                retry_at_ms,
                world.roles[transmitter].route_discovery_requests,
                world.roles[anchor].route_discovery_requests,
                world.roles[gateway].route_discovery_requests,
                mesh_sim_count_transitions(&world,
                                           MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                           0u),
                world.roles[transmitter].watchdog.expirations,
                world.roles[anchor].watchdog.expirations,
                world.roles[gateway].watchdog.expirations);
    }
    REQUIRE("click_preemption", SCENARIO_SEED_CLICK,
            data_tx_starts == 2u &&
            propose_tx_starts == 0u &&
            accept_tx_starts == 0u &&
            repair_starts == 0u &&
            repaired_connections == 0u &&
            world.connections[child].completed_repairs == 0u &&
            world.now_us <= ((uint64_t)retry_at_ms + 8500u) * 1000u &&
            no_route_discovery(&world) &&
            no_watchdog_expired(&world));
    return 0;
}

static int test_empty_receive_slots_preserve_timing_for_delayed_delivery(void)
{
    static struct mesh_sim_world world;
    uint8_t transmitter;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t child;
    uint8_t payload[32];
    size_t payload_len;
    struct proto_packet packet;
    struct mesh_event_params params = connection_params(100u,
                                                        MESH_RADIO_EVENT_INTERVAL_MS);

    mesh_sim_init(&world, SCENARIO_SEED_EMPTY);
    REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                              TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &transmitter) == MESH_SIM_OK &&
            mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              ANCHOR_ID_BASE, GATEWAY_ID, ROUTE_EPOCH,
                              &anchor) == MESH_SIM_OK &&
            mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                              GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &gateway) == MESH_SIM_OK);
    REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
            mesh_sim_set_link(&world, transmitter, anchor, 98u, 0u) == MESH_SIM_OK &&
            mesh_sim_set_link(&world, anchor, gateway, 98u, 0u) == MESH_SIM_OK &&
            mesh_sim_install_route(&world, transmitter, anchor, 1u,
                                   ROUTE_EPOCH) == PROTO_OK &&
            mesh_sim_install_route(&world, anchor, gateway, 0u,
                                   ROUTE_EPOCH) == PROTO_OK &&
            mesh_sim_install_downlink(&world, anchor, TRANSMITTER_ID,
                                      transmitter, 1u,
                                      ROUTE_EPOCH) == MESH_SIM_OK &&
            mesh_sim_add_connection(&world, transmitter, anchor, &params,
                                    true, &child) == MESH_SIM_OK);
    REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
            mesh_sim_watchdog_arm(&world, transmitter, STRESS_WATCHDOG_US,
                                  MESH_SIM_WATCHDOG_FAIL) == MESH_SIM_OK &&
            mesh_sim_watchdog_arm(&world, anchor, STRESS_WATCHDOG_US,
                                  MESH_SIM_WATCHDOG_FAIL) == MESH_SIM_OK);
    for (unsigned int event = 0u;
         event < (unsigned int)params.max_missed_events * 4u;
         event++) {
        REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
                run_next_connection(&world, child) == MESH_SIM_OK);
    }
    REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
            world.connections[child].timing_a.timing_fresh &&
            world.connections[child].timing_b.timing_fresh &&
            world.connections[child].timing_a.missed_event_count == 0u &&
            world.connections[child].timing_b.missed_event_count == 0u &&
            world.connections[child].diagnostics_a.ch9_event_misses == 0u &&
            world.connections[child].diagnostics_b.ch9_event_misses == 0u);

    payload_len = data_payload(payload, sizeof(payload), 1u);
    packet = data_packet(1u, (uint16_t)payload_len);
    REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
            mesh_sim_queue_originated(&world, transmitter, &packet,
                                      payload, payload_len) == MESH_SIM_OK);
    for (unsigned int event = 0u;
         event < 2u &&
         world.roles[transmitter].relay.pending.state == MESH_RELAY_TX_IDLE;
         event++) {
        REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
                run_next_connection(&world, child) == MESH_SIM_OK);
    }
    REQUIRE("empty_rx_retention", SCENARIO_SEED_EMPTY,
            world.roles[transmitter].relay.pending.state ==
                MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
            world.connections[child].timing_a.timing_fresh &&
            world.connections[child].timing_b.timing_fresh &&
            no_watchdog_expired(&world));
    return 0;
}

static int test_stalled_work_expires_watchdog(void)
{
    static struct mesh_sim_world world;
    uint8_t anchor;
    const uint64_t timeout_us = UINT64_C(10000);

    mesh_sim_init(&world, SCENARIO_SEED_WATCHDOG);
    REQUIRE("watchdog_stall", SCENARIO_SEED_WATCHDOG,
            mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              ANCHOR_ID_BASE, GATEWAY_ID, ROUTE_EPOCH,
                              &anchor) == MESH_SIM_OK &&
            mesh_sim_watchdog_arm(&world, anchor, timeout_us,
                                  MESH_SIM_WATCHDOG_FAIL) == MESH_SIM_OK);
    REQUIRE("watchdog_stall", SCENARIO_SEED_WATCHDOG,
            mesh_sim_run_until(&world, timeout_us) == MESH_SIM_ERR_WATCHDOG);
    REQUIRE("watchdog_stall", SCENARIO_SEED_WATCHDOG,
            world.roles[anchor].watchdog.expired &&
            world.roles[anchor].watchdog.expirations == 1u &&
            world.roles[anchor].watchdog.deadline_us == timeout_us &&
            world.roles[anchor].watchdog.expired_radio_owner ==
                MESH_RUNTIME_RADIO_NONE &&
            world.roles[anchor].watchdog.expired_queue_count == 0u &&
            world.roles[anchor].watchdog.expired_pending_state ==
                MESH_RELAY_TX_IDLE &&
            mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_WATCHDOG_EXPIRED,
                                       ANCHOR_ID_BASE) == 1u);
    return 0;
}

int main(int argc, char **argv)
{
    int failed = 0;

    if (argc == 2 && strcmp(argv[1], "watchdog_stall") == 0) {
        return test_stalled_work_expires_watchdog();
    }
    if (argc == 2 && strcmp(argv[1], "click") == 0) {
        return test_click_preemption_and_retry();
    }
    if (argc == 2 && strcmp(argv[1], "empty_rx") == 0) {
        return test_empty_receive_slots_preserve_timing_for_delayed_delivery();
    }
    failed |= test_click_preemption_and_retry();
    failed |= test_empty_receive_slots_preserve_timing_for_delayed_delivery();
    failed |= test_stalled_work_expires_watchdog();
    for (uint8_t relay_count = 0u; relay_count <= LINE_MAX_RELAYS;
         relay_count++) {
        failed |= test_line_depth(relay_count);
    }
    if (failed != 0) {
        return 1;
    }
    printf("mesh routing/click production scenarios passed\n");
    return 0;
}
