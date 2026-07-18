#include "mesh_sim.h"

#include "report.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DELIVERY_SWEEP_CASES 128u
#define DELIVERY_GATEWAY_ID UINT64_C(0xb100000000000001)
#define DELIVERY_SOURCE_BASE UINT64_C(0xb200000000000000)
#define DELIVERY_RELAY_BASE UINT64_C(0xb300000000000000)
#define DELIVERY_ROUTE_EPOCH 7u
#define DELIVERY_MAX_STEPS 96u

struct delivery_metrics {
    uint32_t cases;
    uint32_t attempted;
    uint32_t deferred;
    uint32_t delivered;
    uint32_t dropped;
    uint32_t retries;
    uint32_t direct_cases;
    uint32_t relayed_cases;
    uint64_t latency_max_us;
};

static int failures;

#define CHECK(expression, ...) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line=%d ", __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
        return; \
    } \
} while (0)

static uint32_t case_seed(uint32_t index)
{
    uint32_t value = UINT32_C(0xd3110001) +
                     index * UINT32_C(0x9e3779b9);

    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    return value ^ (value >> 16);
}

static struct mesh_event_params connection_params(uint32_t first_event_ms,
                                                  uint32_t interval_ms,
                                                  int32_t skew_ppm)
{
    return (struct mesh_event_params) {
        .event_interval_ms = interval_ms,
        .event_window_ms = 25u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = skew_ppm,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static int build_click_report(uint64_t anchor_id,
                              uint32_t seed,
                              struct proto_packet *packet,
                              uint8_t *payload,
                              size_t payload_capacity,
                              size_t *payload_len)
{
    const struct range_report_fields fields = {
        .clicker_id = UINT64_C(0xbc00000000000000) | seed,
        .anchor_id = anchor_id,
        .event_seq = seed,
        .timestamp_ms = seed,
        .distance_mm = 750 + (int32_t)(seed % 250u),
        .quality = (uint8_t)(80u + seed % 21u),
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .omit_cir = true,
    };
    size_t length = 0u;
    int ret = report_append_range_tlvs(payload,
                                       payload_capacity,
                                       &length,
                                       &fields);

    if (ret != PROTO_OK || length > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = report_init_click_packet(packet,
                                   anchor_id,
                                   DELIVERY_GATEWAY_ID,
                                   seed,
                                   (uint16_t)(seed | 1u),
                                   (uint8_t)length);
    if (ret == PROTO_OK) {
        *payload_len = length;
    }
    return ret;
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

static bool action_runnable(const struct mesh_sim_world *world,
                            uint16_t connection_index,
                            const struct mesh_sim_connection_action *action)
{
    const struct mesh_sim_connection *connection =
        &world->connections[connection_index];
    bool node_a_tx;
    const struct mesh_sim_role_instance *sender;
    const struct mesh_sim_role_instance *receiver;
    const struct mesh_sim_queued_tx *queued;

    if (action->kind != MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT) {
        return true;
    }
    node_a_tx = mesh_event_timing_local_tx_slot(&connection->timing_a);
    if ((action->skipped_events & 1u) != 0u) {
        node_a_tx = !node_a_tx;
    }
    sender = &world->roles[node_a_tx ? connection->node_a : connection->node_b];
    receiver = &world->roles[node_a_tx ? connection->node_b : connection->node_a];
    queued = best_queued_tx_for_peer(sender, receiver->id);
    return queued == NULL || !queued->needs_relay_start ||
           sender->relay.pending.state == MESH_RELAY_TX_IDLE;
}

static int run_connection(struct mesh_sim_world *world,
                          uint16_t connection_index,
                          bool receiver_preempted)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world, connection_index, &action);

    if (ret != MESH_SIM_OK || action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    if (!action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(world,
                                                      connection_index,
                                                      receiver_preempted);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return mesh_sim_run_until(world, action.end_us);
}

static int run_earliest(struct mesh_sim_world *world,
                        const uint16_t *connections,
                        size_t connection_count)
{
    size_t selected = SIZE_MAX;
    uint64_t earliest_us = UINT64_MAX;

    for (size_t i = 0u; i < connection_count; i++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(world, connections[i], &action);

        if (ret == MESH_SIM_OK &&
            action.kind != MESH_SIM_CONNECTION_ACTION_NONE &&
            action_runnable(world, connections[i], &action) &&
            action.start_us >= world->now_us && action.start_us < earliest_us) {
            selected = i;
            earliest_us = action.start_us;
        }
    }
    return selected == SIZE_MAX ? MESH_SIM_ERR_EVENT_ORDER :
           run_connection(world, connections[selected], false);
}

static bool network_idle(const struct mesh_sim_world *world)
{
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].tx_queue_count != 0u ||
            world->roles[i].relay.pending.state != MESH_RELAY_TX_IDLE) {
            return false;
        }
    }
    return true;
}

static int queue_pending_retry(struct mesh_sim_world *world, uint8_t source)
{
    struct mesh_sim_role_instance *node = &world->roles[source];
    uint64_t at_us;
    int ret;

    if (node->relay.pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    at_us = (uint64_t)node->relay.pending.gateway_ack_deadline_ms * 1000u;
    ret = mesh_sim_schedule_relay_tick(world, source, at_us);
    if (ret != MESH_SIM_OK || mesh_sim_run_until(world, at_us) != MESH_SIM_OK ||
        node->relay.pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    at_us = (uint64_t)node->relay.pending.retry_after_ms * 1000u;
    ret = mesh_sim_schedule_relay_tick(world, source, at_us);
    return ret == MESH_SIM_OK ? mesh_sim_run_until(world, at_us) : ret;
}

static void run_production_case(uint32_t index, struct delivery_metrics *metrics)
{
    static struct mesh_sim_world world;
    struct proto_packet packet;
    uint8_t payload[160];
    size_t payload_len = 0u;
    uint8_t source;
    uint8_t relay = UINT8_MAX;
    uint8_t gateway;
    uint8_t next_hop;
    uint16_t connections[2];
    size_t connection_count;
    uint32_t seed = case_seed(index);
    uint8_t fault = (uint8_t)(seed % 3u);
    bool relayed = (seed & 1u) != 0u;
    int32_t skew = (int32_t)(seed % 81u) - 40;
    int drive_error = MESH_SIM_OK;

    mesh_sim_init(&world, seed);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            DELIVERY_SOURCE_BASE + index,
                            DELIVERY_GATEWAY_ID,
                            DELIVERY_ROUTE_EPOCH,
                            &source) == MESH_SIM_OK,
          "add source seed=0x%08" PRIx32, seed);
    if (relayed) {
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                DELIVERY_RELAY_BASE + index,
                                DELIVERY_GATEWAY_ID,
                                DELIVERY_ROUTE_EPOCH,
                                &relay) == MESH_SIM_OK,
              "add relay seed=0x%08" PRIx32, seed);
    }
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            DELIVERY_GATEWAY_ID,
                            DELIVERY_GATEWAY_ID,
                            DELIVERY_ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK,
          "add gateway seed=0x%08" PRIx32, seed);
    next_hop = relayed ? relay : gateway;
    {
        struct mesh_event_params params = connection_params(
            100u + seed % 31u, 260u + seed % 101u, skew);

        CHECK(mesh_sim_set_link(&world, source, next_hop, 96u, 1u) == MESH_SIM_OK &&
                  mesh_sim_add_connection(&world,
                                          source,
                                          next_hop,
                                          &params,
                                          true,
                                          &connections[0]) == MESH_SIM_OK,
              "add child connection seed=0x%08" PRIx32, seed);
    }
    connection_count = 1u;
    if (relayed) {
        struct mesh_event_params params = connection_params(
            140u + seed % 29u, 300u + seed % 83u, -skew);

        CHECK(mesh_sim_set_link(&world, relay, gateway, 96u, 1u) == MESH_SIM_OK &&
                  mesh_sim_add_connection(&world,
                                          relay,
                                          gateway,
                                          &params,
                                          true,
                                          &connections[1]) == MESH_SIM_OK,
              "add upstream connection seed=0x%08" PRIx32, seed);
        connection_count = 2u;
    }
    CHECK(mesh_sim_install_route(&world,
                                 source,
                                 next_hop,
                                 relayed ? 1u : 0u,
                                 DELIVERY_ROUTE_EPOCH) == PROTO_OK,
          "source route seed=0x%08" PRIx32, seed);
    if (relayed) {
        CHECK(mesh_sim_install_route(&world,
                                     relay,
                                     gateway,
                                     0u,
                                     DELIVERY_ROUTE_EPOCH) == PROTO_OK &&
                  mesh_sim_install_downlink(&world,
                                            relay,
                                            world.roles[source].id,
                                            source,
                                            1u,
                                            DELIVERY_ROUTE_EPOCH) == MESH_SIM_OK,
              "relay route seed=0x%08" PRIx32, seed);
    }
    CHECK(build_click_report(world.roles[source].id,
                             seed,
                             &packet,
                             payload,
                             sizeof(payload),
                             &payload_len) == PROTO_OK &&
              mesh_sim_queue_originated(&world,
                                        source,
                                        &packet,
                                        payload,
                                        payload_len) == MESH_SIM_OK,
          "queue full click report seed=0x%08" PRIx32, seed);

    if (fault == 1u) {
        CHECK(mesh_sim_set_directed_rx_failures(&world,
                                                source,
                                                next_hop,
                                                1u,
                                                MESH_SIM_RX_FRAME_TIMEOUT) ==
                  MESH_SIM_OK,
              "inject loss seed=0x%08" PRIx32, seed);
    } else if (fault == 2u) {
        world.reachable[source][next_hop] = false;
        world.reachable[next_hop][source] = false;
        metrics->deferred++;
    }

    metrics->attempted++;
    CHECK(run_connection(&world, connections[0], false) == MESH_SIM_OK,
          "first attempt seed=0x%08" PRIx32, seed);
    if (fault != 0u) {
        if (fault == 2u) {
            CHECK(mesh_sim_set_link(&world, source, next_hop, 96u, 1u) == MESH_SIM_OK,
                  "restore route seed=0x%08" PRIx32, seed);
        }
        CHECK(queue_pending_retry(&world, source) == MESH_SIM_OK,
              "production retry seed=0x%08" PRIx32, seed);
        metrics->retries++;
    }

    for (unsigned int step = 0u;
         step < DELIVERY_MAX_STEPS &&
         (world.roles[gateway].delivery_count == 0u || !network_idle(&world));
         step++) {
        drive_error = run_earliest(&world, connections, connection_count);
        if (drive_error != MESH_SIM_OK) {
            break;
        }
    }
    metrics->cases++;
    metrics->direct_cases += relayed ? 0u : 1u;
    metrics->relayed_cases += relayed ? 1u : 0u;
    metrics->attempted += world.transmission_total_count > 0u ?
                          (uint32_t)world.transmission_total_count - 1u : 0u;
    if (world.roles[gateway].delivery_count == 1u && network_idle(&world)) {
        uint64_t latency = world.roles[gateway].deliveries[0].delivered_at_us;

        metrics->delivered++;
        if (latency > metrics->latency_max_us) {
            metrics->latency_max_us = latency;
        }
    } else {
        metrics->dropped++;
    }
    printf("production-bound seed=0x%08" PRIx32
           " path=%s fault=%u delivered=%zu retries=%" PRIu64
           " drive_error=%d"
           " latency_us=%" PRIu64 "\n",
           seed,
           relayed ? "two-hop" : "direct",
           fault,
           world.roles[gateway].delivery_count,
           mesh_sim_count_transitions(&world,
                                      MESH_SIM_TRANSITION_RETRY_READY,
                                      world.roles[source].id),
           drive_error,
           world.roles[gateway].delivery_count == 0u ? 0u :
               world.roles[gateway].deliveries[0].delivered_at_us);
}

int main(void)
{
    struct delivery_metrics metrics = {0};

    for (uint32_t index = 0u; index < DELIVERY_SWEEP_CASES; index++) {
        run_production_case(index, &metrics);
        if (failures != 0) {
            return 1;
        }
    }
    printf("production-bound aggregate cases=%u direct=%u relayed=%u"
           " attempted=%u deferred=%u delivered=%u dropped=%u retries=%u"
           " latency_max_us=%" PRIu64
           " empirical_delivery_percent=%.3f reliability_claim=none"
           " missing_seam=click-wake-ranging-to-report-and-native-BLE-stream\n",
           metrics.cases,
           metrics.direct_cases,
           metrics.relayed_cases,
           metrics.attempted,
           metrics.deferred,
           metrics.delivered,
           metrics.dropped,
           metrics.retries,
           metrics.latency_max_us,
           metrics.cases == 0u ? 0.0 :
               100.0 * metrics.delivered / metrics.cases);
    if (metrics.cases != DELIVERY_SWEEP_CASES ||
        metrics.direct_cases == 0u || metrics.relayed_cases == 0u ||
        metrics.attempted == 0u || metrics.retries == 0u ||
        metrics.deferred == 0u ||
        metrics.latency_max_us >= UINT64_C(20000000)) {
        fprintf(stderr,
                "FAIL aggregate cases=%u direct=%u relayed=%u attempted=%u"
                " retries=%u deferred=%u latency_max=%" PRIu64 "\n",
                metrics.cases,
                metrics.direct_cases,
                metrics.relayed_cases,
                metrics.attempted,
                metrics.retries,
                metrics.deferred,
                metrics.latency_max_us);
        failures++;
    }
    return failures == 0 ? 0 : 1;
}
