#include "mesh_sim.h"
#include "survey.h"
#include "app_mesh_direct_gateway_retry.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xa001000000000001)
#define ANCHOR_ID_BASE UINT64_C(0xa002000000010000)
#define SURVEY_ID UINT32_C(0x50665006)
#define ROUTE_EPOCH UINT32_C(19)
#define MAX_ANCHORS 50u
#define MAX_BATCH_ANCHORS 4u
#define MAX_BATCH_CONNECTIONS 4u
#define MAX_CHAIN_DEPTH 4u
#define MAX_STEPS 12000u
#define DIRECT_ANCHORS 20u
#define DIRECT_ATTEMPT_BUDGET_US UINT64_C(280000)
#define DIRECT_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_ACK_SERVICE_US UINT64_C(40000)

static struct mesh_sim_world world;
static struct survey_gateway_context survey_context;

#define REQUIRE(expression)                                                     \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "FAIL %s:%d roles=%zu now=%llu error=%d: %s\n",  \
                    __FILE__, __LINE__, world.role_count,                       \
                    (unsigned long long)world.now_us, world.last_error,         \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = MESH_RADIO_EVENT_INTERVAL_MS,
        .event_window_ms = MESH_RADIO_EVENT_WINDOW_MS,
        .first_event_time_ms = first_event_ms,
        .guard_ms = MESH_RADIO_EVENT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = MESH_RADIO_EVENT_MAX_MISSES,
        .supervision_timeout_ms = MESH_RADIO_EVENT_SUPERVISION_MS,
    };
}

static bool network_idle(const struct mesh_sim_world *sim)
{
    for (size_t i = 0u; i < sim->role_count; i++) {
        if (sim->roles[i].tx_queue_count != 0u ||
            sim->roles[i].relay.pending.state != MESH_RELAY_TX_IDLE) {
            return false;
        }
    }
    return true;
}

static const struct mesh_sim_queued_tx *best_queued_tx_for_peer(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id)
{
    const struct mesh_sim_queued_tx *best = NULL;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *candidate = &node->tx_queue[i];

        if (!candidate->valid || candidate->outbound.next_hop_id != peer_id) {
            continue;
        }
        if (best == NULL || candidate->priority > best->priority ||
            (candidate->priority == best->priority &&
             candidate->enqueue_order < best->enqueue_order)) {
            best = candidate;
        }
    }
    return best;
}

static bool connection_action_runnable(
    const struct mesh_sim_world *sim,
    uint16_t connection_index,
    const struct mesh_sim_connection_action *action)
{
    const struct mesh_sim_connection *connection =
        &sim->connections[connection_index];
    const struct mesh_sim_role_instance *sender;
    const struct mesh_sim_role_instance *receiver;
    const struct mesh_sim_queued_tx *queued;
    bool node_a_tx;

    if (action->kind != MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT) {
        return true;
    }
    node_a_tx = mesh_event_timing_local_tx_slot(&connection->timing_a);
    if ((action->skipped_events & 1u) != 0u) {
        node_a_tx = !node_a_tx;
    }
    sender = &sim->roles[node_a_tx ? connection->node_a : connection->node_b];
    receiver = &sim->roles[node_a_tx ? connection->node_b : connection->node_a];
    queued = best_queued_tx_for_peer(sender, receiver->id);
    return queued == NULL || !queued->needs_relay_start ||
           sender->relay.pending.state == MESH_RELAY_TX_IDLE;
}

static int run_earliest_connection(struct mesh_sim_world *sim,
                                   const uint16_t *connections,
                                   size_t connection_count)
{
    size_t selected = SIZE_MAX;
    uint64_t earliest_us = UINT64_MAX;
    int first_error = MESH_SIM_OK;

    for (size_t i = 0u; i < connection_count; i++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(sim, connections[i], &action);

        if (ret != MESH_SIM_OK) {
            if (first_error == MESH_SIM_OK) {
                first_error = ret;
            }
            continue;
        }
        if (action.kind != MESH_SIM_CONNECTION_ACTION_NONE &&
            connection_action_runnable(sim, connections[i], &action) &&
            action.start_us >= sim->now_us && action.start_us < earliest_us) {
            selected = i;
            earliest_us = action.start_us;
        }
    }
    if (selected == SIZE_MAX) {
        return first_error == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : first_error;
    }
    {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(sim, connections[selected],
                                                  &action);

        if (ret != MESH_SIM_OK) {
            return ret;
        }
        if (!action.already_scheduled) {
            ret = mesh_sim_schedule_next_connection_event(
                sim, connections[selected], false);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
        return mesh_sim_run_until(sim, action.end_us);
    }
}

static int drive_until_drained(struct mesh_sim_world *sim,
                               const uint16_t *connections,
                               size_t connection_count,
                               uint8_t gateway,
                               size_t expected_deliveries)
{
    for (unsigned int step = 0u; step < MAX_STEPS; step++) {
        int ret;

        if (sim->roles[gateway].delivery_count >= expected_deliveries &&
            network_idle(sim)) {
            return MESH_SIM_OK;
        }
        ret = run_earliest_connection(sim, connections, connection_count);
        if (ret != MESH_SIM_OK) {
            fprintf(stderr, "drive ret=%d step=%u deliveries=%zu/%zu\n", ret,
                    step, sim->roles[gateway].delivery_count,
                    expected_deliveries);
            return ret;
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int run_connection_once(struct mesh_sim_world *sim,
                               uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(sim, connection_index, &action);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    if (!action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(sim, connection_index,
                                                      false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return mesh_sim_run_until(sim, action.end_us);
}

static size_t append_peer_entries(size_t anchor_count,
                                  size_t anchor_index,
                                  struct survey_reachability_entry *entries)
{
    size_t count = 0u;

    for (size_t distance = 1u;
         distance < anchor_count && count < SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR;
         distance++) {
        size_t forward = (anchor_index + distance) % anchor_count;
        size_t reverse = (anchor_index + anchor_count - distance) % anchor_count;

        entries[count++] = (struct survey_reachability_entry) {
            .peer_id = ANCHOR_ID_BASE + forward,
            .rssi_dbm = (int8_t)(-45 - (int)distance),
            .quality = (uint8_t)(100u - distance),
        };
        if (reverse != forward && count < SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            entries[count++] = (struct survey_reachability_entry) {
                .peer_id = ANCHOR_ID_BASE + reverse,
                .rssi_dbm = (int8_t)(-46 - (int)distance),
                .quality = (uint8_t)(99u - distance),
            };
        }
    }
    return count;
}

static int build_report(size_t anchor_count,
                        size_t anchor_index,
                        uint16_t seq,
                        struct proto_packet *packet,
                        uint8_t *payload,
                        size_t *payload_len)
{
    struct survey_reachability_entry entries[
        SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR];
    size_t entry_count = append_peer_entries(anchor_count, anchor_index, entries);
    int ret = survey_append_reach_report_tlvs(
        payload, UWB_MESH_MAX_PAYLOAD_LEN, payload_len, SURVEY_ID,
        ANCHOR_ID_BASE + anchor_index, entries, entry_count);

    if (ret != PROTO_OK) {
        return ret;
    }
    return survey_init_discovery_report_packet(
        packet, ANCHOR_ID_BASE + anchor_index, GATEWAY_ID, SURVEY_ID, seq,
        (uint8_t)*payload_len);
}

static int note_delivery(const struct mesh_sim_delivery *delivery)
{
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    size_t entry_count = 0u;
    int ret = survey_extract_reach_report_tlvs(
        delivery->payload, delivery->payload_len, &survey_id, &anchor_id,
        entries, sizeof(entries) / sizeof(entries[0]), &entry_count);

    if (ret != PROTO_OK ||
        delivery->packet.msg_type != MSG_SURVEY_DISCOVERY_REPORT ||
        delivery->packet.session_id != survey_id ||
        delivery->packet.src_id != anchor_id) {
        return PROTO_ERR_MALFORMED;
    }
    return survey_gateway_note_reach_report(
        &survey_context, survey_id, anchor_id, entries, entry_count);
}

/*
 * This is deliberately a component fixture: each batch represents transient
 * production-valid route reservations, while survey_context is the one gateway
 * command context that accumulates reports from every logical anchor.
 */
static int run_report_batch(size_t anchor_count,
                            size_t first_anchor,
                            size_t batch_count,
                            bool duplicate_first)
{
    uint8_t nodes[MAX_BATCH_ANCHORS];
    uint16_t connections[MAX_BATCH_CONNECTIONS];
    size_t connection_count = 0u;
    size_t expected_deliveries = 0u;
    uint8_t gateway;

    REQUIRE(batch_count > 0u && batch_count <= MAX_BATCH_ANCHORS);
    REQUIRE(batch_count <= MAX_CHAIN_DEPTH);

    mesh_sim_init(&world, UINT32_C(0x5a170000) +
                          (uint32_t)anchor_count * 64u +
                          (uint32_t)first_anchor);
    for (size_t i = 0u; i < batch_count; i++) {
        REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                  ANCHOR_ID_BASE + first_anchor + i,
                                  GATEWAY_ID, ROUTE_EPOCH, &nodes[i]) ==
                MESH_SIM_OK);
    }
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);

    {
        for (size_t depth = 0u; depth < batch_count; depth++) {
            size_t index = depth;
            uint8_t parent = depth == 0u ? gateway : nodes[index - 1u];
            uint32_t first_event_ms = MESH_RADIO_EVENT_FIRST_DELAY_MS +
                ((depth & 1u) != 0u ?
                    MESH_RADIO_EVENT_INTERVAL_MS / 2u : 0u);
            struct mesh_event_params params = connection_params(first_event_ms);

            REQUIRE(mesh_sim_set_link(&world, nodes[index], parent, 96u, 1u) ==
                    MESH_SIM_OK);
            REQUIRE(mesh_sim_add_connection(&world, nodes[index], parent,
                                            &params, true,
                                            &connections[connection_count]) ==
                    MESH_SIM_OK);
            REQUIRE(mesh_sim_install_route(&world, nodes[index], parent,
                                           (uint8_t)depth, ROUTE_EPOCH) ==
                    PROTO_OK);
            connection_count++;
        }
        for (size_t source_depth = 1u; source_depth < batch_count;
             source_depth++) {
            size_t source = source_depth;

            for (size_t relay_depth = 0u; relay_depth < source_depth;
                 relay_depth++) {
                size_t relay = relay_depth;
                size_t child = relay + 1u;

                REQUIRE(mesh_sim_install_downlink(
                            &world, nodes[relay],
                            ANCHOR_ID_BASE + first_anchor + source,
                            nodes[child], (uint8_t)(source_depth - relay_depth),
                            ROUTE_EPOCH) == MESH_SIM_OK);
            }
        }
    }
    REQUIRE(connection_count == batch_count);

    for (size_t remaining = batch_count; remaining > 0u; remaining--) {
        size_t i = remaining - 1u;
        struct proto_packet packet;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t payload_len = 0u;

        REQUIRE(build_report(anchor_count, first_anchor + i,
                             (uint16_t)(first_anchor + i + 1u), &packet,
                             payload, &payload_len) == PROTO_OK);
        REQUIRE(mesh_sim_queue_originated(&world, nodes[i], &packet, payload,
                                          payload_len) == MESH_SIM_OK);
        expected_deliveries++;
        REQUIRE(drive_until_drained(&world, connections, connection_count,
                                    gateway, expected_deliveries) == MESH_SIM_OK);
    }
    REQUIRE(world.roles[gateway].delivery_count == expected_deliveries);
    REQUIRE(network_idle(&world));
    for (size_t i = 0u; i < expected_deliveries; i++) {
        REQUIRE(note_delivery(&world.roles[gateway].deliveries[i]) == PROTO_OK);
    }
    if (duplicate_first) {
        size_t report_count = survey_context.report_count;

        REQUIRE(note_delivery(&world.roles[gateway].deliveries[0]) == PROTO_OK);
        REQUIRE(survey_context.report_count == report_count);
    }
    return 0;
}

static int run_topology(size_t anchor_count)
{
    size_t delivered = 0u;

    REQUIRE(anchor_count >= 2u && anchor_count <= MAX_ANCHORS);
    REQUIRE(survey_gateway_begin(&survey_context, SURVEY_ID, 3u) == PROTO_OK);
    while (delivered < anchor_count) {
        size_t batch_count = anchor_count - delivered;

        if (batch_count > MAX_BATCH_ANCHORS) {
            batch_count = MAX_BATCH_ANCHORS;
        }
        REQUIRE(run_report_batch(anchor_count, delivered, batch_count,
                                 delivered == 0u) == 0);
        delivered += batch_count;
        REQUIRE(survey_context.report_count == delivered);
    }
    REQUIRE(survey_gateway_plan_pairs(&survey_context) == PROTO_OK);
    REQUIRE(survey_context.report_count == anchor_count);
    REQUIRE(survey_context.pair_count > 0u);
    if (anchor_count == MAX_ANCHORS) {
        REQUIRE(survey_context.pair_count == SURVEY_GATEWAY_MAX_PAIRS);
    }
    return 0;
}

static bool persistent_topology_valid(const uint8_t *parents,
                                      size_t anchor_count,
                                      uint8_t gateway_index)
{
    size_t upstream[MAX_BATCH_ANCHORS] = {0u};
    size_t downstream[MAX_BATCH_ANCHORS] = {0u};

    if (parents == NULL || anchor_count > MAX_BATCH_ANCHORS) {
        return false;
    }
    for (size_t child = 0u; child < anchor_count; child++) {
        if (parents[child] == gateway_index) {
            continue;
        } else if (parents[child] < anchor_count) {
            upstream[child]++;
            downstream[parents[child]]++;
        } else {
            return false;
        }
    }
    for (size_t i = 0u; i < anchor_count; i++) {
        if (upstream[i] > 1u || downstream[i] > 1u ||
            upstream[i] + downstream[i] > 2u) {
            return false;
        }
    }
    return true;
}

static int test_over_capacity_relay_topology_rejected(void)
{
    static const uint8_t valid_two_chains[] = {4u, 0u, 1u, 4u};
    static const uint8_t valid_many_gateway_roots[] = {4u, 4u, 4u, 4u};
    static const uint8_t invalid_branching_relay[] = {4u, 0u, 0u, 4u};

    REQUIRE(persistent_topology_valid(valid_two_chains, 4u, 4u));
    REQUIRE(persistent_topology_valid(valid_many_gateway_roots, 4u, 4u));
    REQUIRE(!persistent_topology_valid(invalid_branching_relay, 4u, 4u));
    return 0;
}

static int test_missing_route_fails_explicitly(void)
{
    uint8_t anchor;
    uint8_t gateway;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    mesh_sim_init(&world, UINT32_C(0x5a17ffff));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID_BASE,
                              GATEWAY_ID, ROUTE_EPOCH, &anchor) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    REQUIRE(build_report(2u, 0u, 1u, &packet, payload, &payload_len) == PROTO_OK);
    REQUIRE(mesh_sim_queue_originated(&world, anchor, &packet, payload,
                                      payload_len) == MESH_SIM_OK);
    REQUIRE(world.roles[anchor].route_waiting_valid);
    REQUIRE(world.roles[gateway].delivery_count == 0u);
    REQUIRE(mesh_sim_count_transitions(&world, MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                       ANCHOR_ID_BASE) == 1u);
    return 0;
}

static int test_unscheduled_direct_gateway_custody(void)
{
    uint8_t anchor;
    uint8_t gateway;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    uint64_t report_air_start_us = UINT64_C(50000);
    uint64_t report_window_end_us = UINT64_C(100000);
    uint64_t ack_air_start_us;
    uint64_t ack_window_end_us;

    mesh_sim_init(&world, UINT32_C(0x5a17f100));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID_BASE,
                              GATEWAY_ID, ROUTE_EPOCH, &anchor) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    REQUIRE(mesh_sim_set_link(&world, anchor, gateway, 96u, 1u) == MESH_SIM_OK);
    REQUIRE(mesh_relay_note_direct_gateway_route(&world.roles[anchor].relay,
                                                 0u) == PROTO_OK);
    REQUIRE(build_report(2u, 0u, 1u, &packet, payload, &payload_len) == PROTO_OK);
    REQUIRE(mesh_sim_queue_originated(&world, anchor, &packet, payload,
                                      payload_len) == MESH_SIM_OK);
    REQUIRE(world.connection_count == 0u);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway,
                                           report_air_start_us,
                                           report_window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, report_air_start_us, report_window_end_us,
                NULL) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, report_window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    REQUIRE(world.roles[anchor].relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    {
        size_t events_before = world.event_count;
        size_t rx_before = world.rx_window_count;
        size_t tx_before = world.transmission_count;
        size_t queued_before = world.roles[gateway].tx_queue_count;
        int ret = mesh_sim_direct_gateway_schedule_ack(
            &world, gateway, anchor, world.now_us + 1u,
            world.now_us + UINT64_C(50000), NULL);

        REQUIRE(ret == DWM3000_RUNTIME_ERR_BUSY ||
                ret == MESH_SIM_ERR_RADIO_DEADLINE);
        REQUIRE(world.event_count == events_before);
        REQUIRE(world.rx_window_count == rx_before);
        REQUIRE(world.transmission_count == tx_before);
        REQUIRE(world.roles[gateway].tx_queue_count == queued_before);
        REQUIRE(world.roles[anchor].relay.pending.state ==
                MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        world.last_error = MESH_SIM_OK;
    }

    REQUIRE(world.roles[gateway].dwm3000.cpu_busy_until_us > world.now_us);
    REQUIRE(mesh_sim_run_until(
                &world, world.roles[gateway].dwm3000.cpu_busy_until_us) ==
            MESH_SIM_OK);
    ack_air_start_us = world.now_us + UINT64_C(50000);
    ack_window_end_us = ack_air_start_us + UINT64_C(50000);
    REQUIRE(mesh_sim_direct_gateway_schedule_ack(
                &world, gateway, anchor, ack_air_start_us, ack_window_end_us,
                NULL) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, ack_window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[anchor].relay.pending.state == MESH_RELAY_TX_IDLE);
    REQUIRE(mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                       ANCHOR_ID_BASE) == 1u);
    REQUIRE(world.connection_count == 0u);
    return 0;
}

struct direct_survey_candidate {
    struct app_mesh_direct_gateway_retry_state retry;
    uint64_t due_us;
    uint8_t node;
    bool done;
};

static int gateway_ack_queue_index(uint8_t gateway, uint64_t sender_id)
{
    const struct mesh_sim_role_instance *node = &world.roles[gateway];

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            node->tx_queue[i].outbound.packet.msg_type == MSG_GATEWAY_ACK &&
            node->tx_queue[i].outbound.next_hop_id == sender_id) {
            return (int)i;
        }
    }
    return -1;
}

static void discard_gateway_ack(uint8_t gateway, size_t queue_index)
{
    memset(&world.roles[gateway].tx_queue[queue_index], 0,
           sizeof(world.roles[gateway].tx_queue[queue_index]));
    world.roles[gateway].tx_queue_count--;
}

static int schedule_mesh_payload_retry(uint8_t node, uint64_t *due_us)
{
    struct mesh_relay *relay = &world.roles[node].relay;
    uint64_t timeout_us;

    if (due_us == NULL || relay->pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK) {
        return MESH_SIM_ERR_ARG;
    }
    timeout_us = (uint64_t)relay->pending.gateway_ack_deadline_ms * 1000u;
    if (timeout_us < world.now_us) {
        timeout_us = world.now_us;
    }
    if (mesh_sim_schedule_relay_tick(&world, node, timeout_us) != MESH_SIM_OK ||
        mesh_sim_run_until(&world, timeout_us) != MESH_SIM_OK ||
        relay->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    *due_us = (uint64_t)relay->pending.retry_after_ms * 1000u;
    if (*due_us < world.now_us) {
        *due_us = world.now_us;
    }
    if (mesh_sim_schedule_relay_tick(&world, node, *due_us) != MESH_SIM_OK) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    return MESH_SIM_OK;
}

static int test_gateway_semantic_rejection_retry_and_sticky_ack(void)
{
    uint8_t anchor;
    uint8_t gateway;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    uint64_t due_us;
    uint64_t air_start_us;
    uint64_t window_end_us;
    int ack_index;

    mesh_sim_init(&world, UINT32_C(0x5a17f102));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID_BASE,
                              GATEWAY_ID, ROUTE_EPOCH, &anchor) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    REQUIRE(mesh_sim_set_link(&world, anchor, gateway, 96u, 1u) == MESH_SIM_OK);
    REQUIRE(mesh_relay_note_direct_gateway_route(&world.roles[anchor].relay,
                                                 0u) == PROTO_OK);
    REQUIRE(mesh_sim_gateway_reject_next_semantic_deliveries(
                &world, gateway, 1u) == MESH_SIM_OK);
    REQUIRE(build_report(2u, 0u, 10u, &packet, payload, &payload_len) ==
            PROTO_OK);
    REQUIRE(mesh_sim_queue_originated(&world, anchor, &packet, payload,
                                      payload_len) == MESH_SIM_OK);

    air_start_us = UINT64_C(50000);
    window_end_us = UINT64_C(100000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 0u);
    REQUIRE(world.roles[gateway].gateway_semantic_rejection_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_commit_count == 0u);
    REQUIRE(world.roles[gateway].gateway_semantic_duplicate_ack_count == 0u);
    REQUIRE(gateway_ack_queue_index(gateway, ANCHOR_ID_BASE) < 0);
    REQUIRE(world.roles[anchor].relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    REQUIRE(schedule_mesh_payload_retry(anchor, &due_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, due_us) == MESH_SIM_OK);
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + UINT64_C(50000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_rejection_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_commit_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_duplicate_ack_count == 0u);
    ack_index = gateway_ack_queue_index(gateway, ANCHOR_ID_BASE);
    REQUIRE(ack_index >= 0);
    discard_gateway_ack(gateway, (size_t)ack_index);

    REQUIRE(schedule_mesh_payload_retry(anchor, &due_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, due_us) == MESH_SIM_OK);
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + UINT64_C(50000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_commit_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_duplicate_ack_count == 1u);
    REQUIRE(gateway_ack_queue_index(gateway, ANCHOR_ID_BASE) >= 0);

    if (world.roles[gateway].dwm3000.cpu_busy_until_us > world.now_us) {
        REQUIRE(mesh_sim_run_until(
                    &world, world.roles[gateway].dwm3000.cpu_busy_until_us) ==
                MESH_SIM_OK);
    }
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + DIRECT_ACK_SERVICE_US;
    REQUIRE(mesh_sim_direct_gateway_schedule_ack(
                &world, gateway, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[anchor].relay.pending.state == MESH_RELAY_TX_IDLE);
    REQUIRE(mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                       ANCHOR_ID_BASE) == 1u);
    return 0;
}

static int test_gateway_collection_duplicate_redelivers_for_eack_rearm(void)
{
    uint8_t anchor;
    uint8_t gateway;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_ID_BASE,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x5a17f103),
        .seq = 12u,
        .ttl = 2u,
    };
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = ROUTE_EPOCH,
        .command_seq = UINT32_C(0x4a17f103),
        .node_id = ANCHOR_ID_BASE,
        .node_boot_counter = 17u,
        .result_seq = 18u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;
    uint64_t due_us;
    uint64_t air_start_us;
    uint64_t window_end_us;
    int ack_index;

    mesh_sim_init(&world, UINT32_C(0x5a17f103));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID_BASE,
                              GATEWAY_ID, ROUTE_EPOCH, &anchor) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    REQUIRE(mesh_sim_set_link(&world, anchor, gateway, 96u, 1u) == MESH_SIM_OK);
    REQUIRE(mesh_relay_note_direct_gateway_route(&world.roles[anchor].relay,
                                                 0u) == PROTO_OK);
    REQUIRE(command_result_id_append_tlvs(payload,
                                          sizeof(payload),
                                          &payload_len,
                                          &result_id) == PROTO_OK);
    REQUIRE(tlv_append_u32(payload,
                           sizeof(payload),
                           &payload_len,
                           TLV_COLLECTION_EPOCH_ID,
                           UINT32_C(0x6a17f103)) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    REQUIRE(mesh_sim_queue_originated(&world, anchor, &packet, payload,
                                      payload_len) == MESH_SIM_OK);

    air_start_us = UINT64_C(50000);
    window_end_us = UINT64_C(100000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_commit_count == 1u);
    REQUIRE(world.roles[gateway].gateway_semantic_duplicate_ack_count == 0u);
    REQUIRE(world.roles[anchor].relay.outbox_record.delivery_state ==
            MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    ack_index = gateway_ack_queue_index(gateway, ANCHOR_ID_BASE);
    REQUIRE(ack_index >= 0);
    discard_gateway_ack(gateway, (size_t)ack_index);

    REQUIRE(schedule_mesh_payload_retry(anchor, &due_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, due_us) == MESH_SIM_OK);
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + UINT64_C(50000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 2u);
    REQUIRE(world.roles[gateway].gateway_semantic_commit_count == 2u);
    REQUIRE(world.roles[gateway].gateway_semantic_duplicate_ack_count == 0u);
    REQUIRE(gateway_ack_queue_index(gateway, ANCHOR_ID_BASE) >= 0);

    if (world.roles[gateway].dwm3000.cpu_busy_until_us > world.now_us) {
        REQUIRE(mesh_sim_run_until(
                    &world, world.roles[gateway].dwm3000.cpu_busy_until_us) ==
                MESH_SIM_OK);
    }
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + DIRECT_ACK_SERVICE_US;
    REQUIRE(mesh_sim_direct_gateway_schedule_ack(
                &world, gateway, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[anchor].relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    REQUIRE(world.roles[anchor].relay.outbox_record.delivery_state ==
            MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    REQUIRE(mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                       ANCHOR_ID_BASE) == 0u);
    return 0;
}

static int test_direct_short_rx_and_mismatched_ack_retry(void)
{
    uint8_t anchor;
    uint8_t gateway;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    uint16_t tx_index;
    uint64_t due_us;
    uint64_t air_start_us;
    uint64_t window_end_us;
    int ack_index;

    mesh_sim_init(&world, UINT32_C(0x5a17f101));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID_BASE,
                              GATEWAY_ID, ROUTE_EPOCH, &anchor) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    REQUIRE(mesh_sim_set_link(&world, anchor, gateway, 96u, 1u) == MESH_SIM_OK);
    REQUIRE(mesh_relay_note_direct_gateway_route(&world.roles[anchor].relay,
                                                 0u) == PROTO_OK);
    REQUIRE(build_report(2u, 0u, 9u, &packet, payload, &payload_len) == PROTO_OK);
    REQUIRE(mesh_sim_queue_originated(&world, anchor, &packet, payload,
                                      payload_len) == MESH_SIM_OK);
    air_start_us = UINT64_C(50000);
    window_end_us = UINT64_C(100000);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, &tx_index) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(
                &world, gateway, air_start_us,
                world.transmissions[tx_index].end_us - 1u) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 0u);
    REQUIRE(world.roles[anchor].relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    REQUIRE(schedule_mesh_payload_retry(anchor, &due_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, due_us) == MESH_SIM_OK);
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + UINT64_C(50000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    if (world.roles[gateway].dwm3000.cpu_busy_until_us > world.now_us) {
        REQUIRE(mesh_sim_run_until(
                    &world, world.roles[gateway].dwm3000.cpu_busy_until_us) ==
                MESH_SIM_OK);
    }
    ack_index = gateway_ack_queue_index(gateway, ANCHOR_ID_BASE);
    REQUIRE(ack_index >= 0);
    world.roles[gateway].tx_queue[ack_index].outbound.packet.session_id++;
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + DIRECT_ACK_SERVICE_US;
    REQUIRE(mesh_sim_direct_gateway_schedule_ack(
                &world, gateway, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[anchor].relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    REQUIRE(schedule_mesh_payload_retry(anchor, &due_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, due_us) == MESH_SIM_OK);
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + UINT64_C(50000);
    REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, air_start_us,
                                           window_end_us) == MESH_SIM_OK);
    REQUIRE(mesh_sim_direct_gateway_start_queued_tx(
                &world, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    if (world.roles[gateway].dwm3000.cpu_busy_until_us > world.now_us) {
        REQUIRE(mesh_sim_run_until(
                    &world, world.roles[gateway].dwm3000.cpu_busy_until_us) ==
                MESH_SIM_OK);
    }
    air_start_us = world.now_us + DIRECT_TX_PREPARE_US;
    window_end_us = air_start_us + DIRECT_ACK_SERVICE_US;
    REQUIRE(mesh_sim_direct_gateway_schedule_ack(
                &world, gateway, anchor, air_start_us, window_end_us, NULL) ==
            MESH_SIM_OK);
    REQUIRE(mesh_sim_run_until(&world, window_end_us) == MESH_SIM_OK);
    REQUIRE(world.roles[anchor].relay.pending.state == MESH_RELAY_TX_IDLE);
    REQUIRE(world.event_count == 0u);
    REQUIRE(world.connection_count == 0u);
    return 0;
}

/*
 * Single-packet custody stress, not gateway batch-ACK coverage. Twenty anchors
 * are RF-direct, but no persistent gateway connection is installed. Fifteen
 * direct-route probes defer without consuming a survey-policy attempt. After
 * route installation, fifteen hostile simultaneous reports collide. Payload
 * retries use only mesh_relay ACK timeout/backoff, while guarded admission
 * serializes ready retransmissions. One injected ACK loss proves ACK-sticky
 * deduplication.
 */
static int test_twenty_direct_reports_retry_to_exactly_once(void)
{
    struct direct_survey_candidate candidates[DIRECT_ANCHORS];
    uint8_t gateway;
    bool injected_ack_loss = false;
    uint64_t deadline_us;
    size_t completed = 0u;

    mesh_sim_init(&world, UINT32_C(0x20d1ec7));
    memset(candidates, 0, sizeof(candidates));
    for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
        REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                  ANCHOR_ID_BASE + i, GATEWAY_ID, ROUTE_EPOCH,
                                  &candidates[i].node) == MESH_SIM_OK);
    }
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
        struct proto_packet packet;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t payload_len = 0u;
        struct app_mesh_direct_gateway_retry_decision decision;

        REQUIRE(mesh_sim_set_link(&world, candidates[i].node, gateway, 96u, 1u) ==
                MESH_SIM_OK);
        REQUIRE(app_mesh_direct_gateway_retry_init(
                    &candidates[i].retry,
                    APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
                    ANCHOR_ID_BASE + i, SURVEY_ID) == 0);
        candidates[i].due_us = UINT64_C(100000);
        if (i >= 15u) {
            REQUIRE(app_mesh_direct_gateway_retry_note(
                        &candidates[i].retry,
                        APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY, 0u,
                        &decision) == 0);
            REQUIRE(decision.retry && !decision.attempt_consumed &&
                    !decision.exhausted);
            candidates[i].due_us += (uint64_t)decision.delay_ms * 1000u;
        }
        REQUIRE(app_mesh_direct_gateway_retry_note(
                    &candidates[i].retry,
                    APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS, 0u,
                    &decision) == 0);
        REQUIRE(decision.attempt_consumed && !decision.retry &&
                !decision.exhausted);
        REQUIRE(mesh_relay_note_direct_gateway_route(
                    &world.roles[candidates[i].node].relay, 0u) == PROTO_OK);
        REQUIRE(build_report(DIRECT_ANCHORS, i, (uint16_t)(i + 1u), &packet,
                             payload, &payload_len) == PROTO_OK);
        REQUIRE(mesh_sim_queue_originated(&world, candidates[i].node, &packet,
                                          payload, payload_len) == MESH_SIM_OK);
    }
    REQUIRE(world.connection_count == 0u);
    deadline_us = UINT64_C(100000) + (uint64_t)
        (app_mesh_direct_gateway_retry_policy_horizon_ms(
             APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY, 280u,
             APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS) +
         MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS +
         DIRECT_ANCHORS * (uint32_t)(DIRECT_ACK_SERVICE_US / 1000u)) * 1000u;

    for (uint8_t round = 0u;
         round < 100u && completed < DIRECT_ANCHORS;
         round++) {
        bool attempted[DIRECT_ANCHORS] = {false};
        bool payload_failed[DIRECT_ANCHORS] = {false};
        uint64_t min_due_us = UINT64_MAX;
        uint64_t max_due_us;
        uint64_t wave_end_us;

        for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
            if (!candidates[i].done) {
                if (candidates[i].due_us <= world.now_us) {
                    candidates[i].due_us = world.now_us + DIRECT_TX_PREPARE_US;
                }
                if (candidates[i].due_us < min_due_us) {
                    min_due_us = candidates[i].due_us;
                }
            }
        }
        REQUIRE(min_due_us != UINT64_MAX && min_due_us >= world.now_us);
        max_due_us = min_due_us;
        for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
            if (!candidates[i].done &&
                candidates[i].due_us <= min_due_us + UINT64_C(20000) &&
                candidates[i].due_us > max_due_us) {
                max_due_us = candidates[i].due_us;
            }
        }
        wave_end_us = max_due_us + DIRECT_TX_PREPARE_US + UINT64_C(50000);
        REQUIRE(wave_end_us <= deadline_us);
        REQUIRE(mesh_sim_direct_gateway_arm_rx(&world, gateway, min_due_us,
                                               wave_end_us) == MESH_SIM_OK);

        size_t scheduled_limit = round == 0u ? DIRECT_ANCHORS : 1u;

        for (size_t scheduled = 0u; scheduled < scheduled_limit; scheduled++) {
            size_t selected = SIZE_MAX;
            uint64_t selected_due = UINT64_MAX;

            for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
                if (!candidates[i].done && !attempted[i] &&
                    candidates[i].due_us >= world.now_us &&
                    candidates[i].due_us < selected_due) {
                    selected = i;
                    selected_due = candidates[i].due_us;
                }
            }
            if (selected == SIZE_MAX || selected_due > max_due_us) {
                break;
            }
            {
                int run_ret = mesh_sim_run_until(&world, selected_due);
                REQUIRE(run_ret == MESH_SIM_OK);
            }
            {
                int direct_ret = mesh_sim_direct_gateway_start_queued_tx(
                    &world, candidates[selected].node,
                    selected_due + DIRECT_TX_PREPARE_US, wave_end_us, NULL);

                REQUIRE(direct_ret == MESH_SIM_OK);
            }
            attempted[selected] = true;
        }
        REQUIRE(mesh_sim_run_until(&world, wave_end_us) == MESH_SIM_OK);
        if (world.roles[gateway].dwm3000.cpu_busy_until_us > world.now_us) {
            REQUIRE(mesh_sim_run_until(
                        &world,
                        world.roles[gateway].dwm3000.cpu_busy_until_us) ==
                    MESH_SIM_OK);
        }

        {
            uint64_t latest_timeout_us = world.now_us;

            for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
                int ack_index;

                if (candidates[i].done || !attempted[i]) {
                    continue;
                }
                ack_index = gateway_ack_queue_index(gateway, ANCHOR_ID_BASE + i);
                if (ack_index >= 0 && !injected_ack_loss) {
                    discard_gateway_ack(gateway, (size_t)ack_index);
                    injected_ack_loss = true;
                    ack_index = -1;
                }
                if (ack_index < 0) {
                    uint64_t timeout_us = (uint64_t)
                        world.roles[candidates[i].node].relay.pending
                            .gateway_ack_deadline_ms * 1000u;

                    if (timeout_us < world.now_us) {
                        timeout_us = world.now_us;
                    }
                    REQUIRE(mesh_sim_schedule_relay_tick(
                                &world, candidates[i].node, timeout_us) ==
                            MESH_SIM_OK);
                    if (timeout_us > latest_timeout_us) {
                        latest_timeout_us = timeout_us;
                    }
                    payload_failed[i] = true;
                }
            }

            for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
                uint64_t ack_air_start_us = world.now_us +
                                            DIRECT_TX_PREPARE_US;
                uint64_t ack_end_us = ack_air_start_us +
                                      DIRECT_ACK_SERVICE_US;

                if (candidates[i].done || !attempted[i] || payload_failed[i]) {
                    continue;
                }
                REQUIRE(mesh_sim_direct_gateway_schedule_ack(
                            &world, gateway, candidates[i].node,
                            ack_air_start_us, ack_end_us, NULL) == MESH_SIM_OK);
                REQUIRE(mesh_sim_run_until(&world, ack_end_us) == MESH_SIM_OK);
                REQUIRE(world.roles[candidates[i].node].relay.pending.state ==
                        MESH_RELAY_TX_IDLE);
                candidates[i].done = true;
                completed++;
            }

            if (latest_timeout_us > world.now_us) {
                REQUIRE(mesh_sim_run_until(&world, latest_timeout_us) ==
                        MESH_SIM_OK);
            }
            for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
                struct mesh_relay *relay;

                if (!payload_failed[i]) {
                    continue;
                }
                relay = &world.roles[candidates[i].node].relay;
                REQUIRE(relay->pending.state ==
                        MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
                candidates[i].due_us =
                    (uint64_t)relay->pending.retry_after_ms * 1000u;
                if (candidates[i].due_us < world.now_us) {
                    candidates[i].due_us = world.now_us;
                }
                REQUIRE(mesh_sim_schedule_relay_tick(
                            &world, candidates[i].node,
                            candidates[i].due_us) == MESH_SIM_OK);
            }
        }
    }

    REQUIRE(injected_ack_loss);
    REQUIRE(completed == DIRECT_ANCHORS);
    REQUIRE(world.now_us <= deadline_us);
    REQUIRE(world.roles[gateway].delivery_count == DIRECT_ANCHORS);
    for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
        REQUIRE(candidates[i].done);
        REQUIRE(candidates[i].retry.attempts == 1u);
        REQUIRE(candidates[i].retry.busy_deferrals == (i >= 15u ? 1u : 0u));
        REQUIRE(world.roles[candidates[i].node].relay.pending.state ==
                MESH_RELAY_TX_IDLE);
        REQUIRE(mesh_sim_count_transitions(&world,
                                           MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                           ANCHOR_ID_BASE + i) == 1u);
    }
    REQUIRE(mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_RETRY_READY,
                                       ANCHOR_ID_BASE + 15u) >= 1u);
    REQUIRE(world.connection_count == 0u);
    return 0;
}

static int test_multihop_route_loss_recovers_exactly_once(void)
{
    uint8_t child;
    uint8_t relay;
    uint8_t gateway;
    uint16_t connections[2];
    struct mesh_event_params child_params =
        connection_params(MESH_RADIO_EVENT_FIRST_DELAY_MS);
    struct mesh_event_params upstream_params = connection_params(
        MESH_RADIO_EVENT_FIRST_DELAY_MS + MESH_RADIO_EVENT_INTERVAL_MS / 2u);
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    mesh_sim_init(&world, UINT32_C(0x5a17ff01));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              ANCHOR_ID_BASE + 1u, GATEWAY_ID, ROUTE_EPOCH,
                              &child) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID_BASE,
                              GATEWAY_ID, ROUTE_EPOCH, &relay) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    REQUIRE(mesh_sim_set_link(&world, child, relay, 96u, 1u) == MESH_SIM_OK);
    REQUIRE(mesh_sim_set_link(&world, relay, gateway, 96u, 1u) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_connection(&world, child, relay, &child_params, true,
                                    &connections[0]) == MESH_SIM_OK);
    REQUIRE(mesh_sim_add_connection(&world, relay, gateway, &upstream_params,
                                    true, &connections[1]) == MESH_SIM_OK);
    REQUIRE(mesh_sim_install_route(&world, child, relay, 1u, ROUTE_EPOCH) ==
            PROTO_OK);
    REQUIRE(mesh_sim_install_route(&world, relay, gateway, 0u, ROUTE_EPOCH) ==
            PROTO_OK);
    REQUIRE(mesh_sim_install_downlink(&world, relay, ANCHOR_ID_BASE + 1u,
                                      child, 1u, ROUTE_EPOCH) == MESH_SIM_OK);
    REQUIRE(build_report(2u, 1u, 77u, &packet, payload, &payload_len) ==
            PROTO_OK);
    REQUIRE(mesh_sim_queue_originated(&world, child, &packet, payload,
                                      payload_len) == MESH_SIM_OK);

    world.reachable[child][relay] = false;
    world.reachable[relay][child] = false;
    REQUIRE(run_connection_once(&world, connections[0]) == MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 0u);
    REQUIRE(world.roles[child].relay.pending.state != MESH_RELAY_TX_IDLE);
    world.reachable[child][relay] = true;
    world.reachable[relay][child] = true;

    {
        uint64_t tick_us = (uint64_t)
            world.roles[child].relay.pending.gateway_ack_deadline_ms * 1000u;

        if (tick_us < world.now_us) {
            tick_us = world.now_us;
        }
        REQUIRE(mesh_sim_schedule_relay_tick(&world, child, tick_us) ==
                MESH_SIM_OK);
        REQUIRE(mesh_sim_run_until(&world, tick_us) == MESH_SIM_OK);
        REQUIRE(world.roles[child].relay.pending.state ==
                MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        tick_us = (uint64_t)world.roles[child].relay.pending.retry_after_ms *
                  1000u;
        if (tick_us < world.now_us) {
            tick_us = world.now_us;
        }
        REQUIRE(mesh_sim_schedule_relay_tick(&world, child, tick_us) ==
                MESH_SIM_OK);
        REQUIRE(mesh_sim_run_until(&world, tick_us) == MESH_SIM_OK);
    }

    REQUIRE(drive_until_drained(&world, connections, 2u, gateway, 1u) ==
            MESH_SIM_OK);
    REQUIRE(world.roles[gateway].delivery_count == 1u);
    REQUIRE(world.roles[gateway].deliveries[0].packet.src_id ==
            ANCHOR_ID_BASE + 1u);
    REQUIRE(network_idle(&world));
    REQUIRE(mesh_sim_count_transitions(&world,
                                       MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                       ANCHOR_ID_BASE + 1u) == 1u);
    return 0;
}

static int test_gateway_reset_reinstalls_fifty_accepted_reverse_hints(void)
{
    struct survey_gateway_context context;
    struct mesh_relay gateway;
    const uint64_t relay_base = UINT64_C(0xa003000000010000);
    size_t resident_routes = 0u;

    /* A gateway reset starts the new survey with an empty downlink table. */
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID,
                    GATEWAY_ID,
                    ROUTE_EPOCH);
    REQUIRE(mesh_relay_find_downlink(&gateway, ANCHOR_ID_BASE) == NULL);
    REQUIRE(survey_gateway_begin(&context, SURVEY_ID, 1u) == PROTO_OK);
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        const uint64_t target_id = ANCHOR_ID_BASE + i;
        const struct survey_gateway_reverse_hint hint = {
            .target_id = target_id,
            .next_hop_id = i < DIRECT_ANCHORS ?
                           target_id : relay_base + (i % 6u),
            .quality = (uint8_t)(100u - i),
            .valid = true,
        };

        REQUIRE(survey_gateway_note_reach_report_with_reverse_hint(
                    &context,
                    SURVEY_ID,
                    target_id,
                    NULL,
                    0u,
                    &hint) == PROTO_OK);
    }
    REQUIRE(context.report_count == SURVEY_GATEWAY_MAX_REPORTS);
    REQUIRE(mesh_relay_find_downlink(&gateway, ANCHOR_ID_BASE) == NULL);

    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        struct survey_gateway_reverse_hint hint = {0};
        uint64_t next_hop_id = 0u;
        const uint64_t target_id = ANCHOR_ID_BASE + i;

        REQUIRE(survey_gateway_reverse_hint_for_target(&context,
                                                       target_id,
                                                       &hint) == PROTO_OK);
        REQUIRE(mesh_relay_note_gateway_survey_reverse_route(
                    &gateway,
                    hint.target_id,
                    hint.next_hop_id,
                    hint.quality,
                    (uint32_t)(1000u + i)) == PROTO_OK);
        REQUIRE(mesh_relay_select_next_hop(&gateway,
                                           target_id,
                                           &next_hop_id) == PROTO_OK);
        REQUIRE(next_hop_id == hint.next_hop_id);
    }

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        if (gateway.downlinks[i].valid) {
            resident_routes++;
        }
    }
    REQUIRE(resident_routes == MESH_RELAY_DOWNLINK_ROUTES);
    REQUIRE(mesh_relay_find_downlink(&gateway,
                                     ANCHOR_ID_BASE +
                                     (MESH_RELAY_DOWNLINK_ROUTES - 1u)) == NULL);

    {
        struct survey_gateway_reverse_hint first_hint = {0};
        uint64_t next_hop_id = 0u;

        const uint64_t evicted_target = ANCHOR_ID_BASE +
            (MESH_RELAY_DOWNLINK_ROUTES - 1u);

        REQUIRE(survey_gateway_reverse_hint_for_target(&context,
                                                       evicted_target,
                                                       &first_hint) == PROTO_OK);
        REQUIRE(mesh_relay_note_gateway_survey_reverse_route(
                    &gateway,
                    first_hint.target_id,
                    first_hint.next_hop_id,
                    first_hint.quality,
                    2000u) == PROTO_OK);
        REQUIRE(mesh_relay_select_next_hop(&gateway,
                                           evicted_target,
                                           &next_hop_id) == PROTO_OK);
        REQUIRE(next_hop_id == evicted_target);
    }
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        REQUIRE(!gateway.event_timings[i].valid);
    }
    return 0;
}

static int test_survey_ttl_exhaustion_fails_explicitly(void)
{
    uint8_t nodes[6];
    uint16_t connections[5];
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret = MESH_SIM_OK;

    mesh_sim_init(&world, UINT32_C(0x5a17ff02));
    for (uint8_t i = 0u; i < 5u; i++) {
        REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                  ANCHOR_ID_BASE + i, GATEWAY_ID, ROUTE_EPOCH,
                                  &nodes[i]) == MESH_SIM_OK);
    }
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                              GATEWAY_ID, ROUTE_EPOCH, &nodes[5]) == MESH_SIM_OK);
    for (uint8_t child = 0u; child < 5u; child++) {
        uint8_t parent = child == 0u ? nodes[5] : nodes[child - 1u];
        uint32_t first_event_ms = MESH_RADIO_EVENT_FIRST_DELAY_MS +
            ((child & 1u) != 0u ? MESH_RADIO_EVENT_INTERVAL_MS / 2u : 0u);
        struct mesh_event_params params = connection_params(first_event_ms);

        REQUIRE(mesh_sim_set_link(&world, nodes[child], parent, 96u, 1u) ==
                MESH_SIM_OK);
        REQUIRE(mesh_sim_add_connection(&world, nodes[child], parent, &params,
                                        true, &connections[child]) == MESH_SIM_OK);
        REQUIRE(mesh_sim_install_route(&world, nodes[child], parent,
                                       child, ROUTE_EPOCH) ==
                PROTO_OK);
    }
    REQUIRE(build_report(5u, 4u, 88u, &packet, payload, &payload_len) ==
            PROTO_OK);
    REQUIRE(packet.ttl == SURVEY_DEFAULT_TTL);
    REQUIRE(mesh_sim_queue_originated(&world, nodes[4], &packet, payload,
                                      payload_len) == MESH_SIM_OK);
    for (unsigned int step = 0u; step < 256u && ret == MESH_SIM_OK; step++) {
        ret = run_earliest_connection(&world, connections, 5u);
    }
    REQUIRE(ret == MESH_SIM_OK);
    REQUIRE(world.roles[nodes[5]].delivery_count == 0u);
    REQUIRE(world.roles[nodes[4]].relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    return 0;
}

int main(void)
{
    static const size_t anchor_counts[] = {2u, 6u, 16u, 32u, 50u};

    for (size_t i = 0u; i < sizeof(anchor_counts) / sizeof(anchor_counts[0]);
         i++) {
        if (run_topology(anchor_counts[i]) != 0) {
            return EXIT_FAILURE;
        }
    }
    if (test_over_capacity_relay_topology_rejected() != 0 ||
        test_missing_route_fails_explicitly() != 0 ||
        test_unscheduled_direct_gateway_custody() != 0 ||
        test_gateway_semantic_rejection_retry_and_sticky_ack() != 0 ||
        test_gateway_collection_duplicate_redelivers_for_eack_rearm() != 0 ||
        test_direct_short_rx_and_mismatched_ack_retry() != 0 ||
        test_twenty_direct_reports_retry_to_exactly_once() != 0 ||
        test_multihop_route_loss_recovers_exactly_once() != 0 ||
        test_gateway_reset_reinstalls_fifty_accepted_reverse_hints() != 0 ||
        test_survey_ttl_exhaustion_fails_explicitly() != 0) {
        return EXIT_FAILURE;
    }
    printf("PASS mesh_survey_topology_scenarios\n");
    return EXIT_SUCCESS;
}
