#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include "app_mesh_direct_gateway_retry.h"
#include "mesh_capacity.h"
#include "mesh_radio_timing.h"
#include "route.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0x9000)
#define NODE_ID_BASE UINT64_C(0xA100)
#define ROUTE_EPOCH UINT32_C(41)
#define MAX_PATH_NODES 3u
#define MAX_PACKETS_PER_NODE MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH
#define MAX_EXPECTED_TRANSACTIONS (MAX_PATH_NODES * MAX_PACKETS_PER_NODE)
#define DEFAULT_SEED UINT32_C(0x51A7E551)
#define DEFAULT_MAX_STEPS 800u
#define MAX_CONSECUTIVE_EMPTY_CHANNEL9_EVENTS 12u
#define MAX_POST_COMPLETION_STEPS_PER_PATH_NODE \
    (MESH_RADIO_EVENT_MAX_MISSES * (ROUTE_MAX_FAILURES + 1u) * \
     ROUTE_MAX_CANDIDATES)
#define MAX_POST_COMPLETION_EMPTY_CHANNEL9_PER_CONNECTION \
    (MESH_RADIO_EVENT_MAX_MISSES * (ROUTE_MAX_FAILURES + 1u) * \
     ROUTE_MAX_CANDIDATES)
#define POST_COMPLETION_STEP_FIXED_ALLOWANCE 2u
#define POST_COMPLETION_EMPTY_CHANNEL9_FIXED_ALLOWANCE 2u
#define IDENTICAL_CONTROL_FIXED_ALLOWANCE 2u
#define TRAFFIC_BOUND_FIXED_ALLOWANCE 64u
#define DIRECT_GATEWAY_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_GATEWAY_PAYLOAD_SERVICE_US UINT64_C(50000)
#define DIRECT_GATEWAY_ACK_SERVICE_US UINT64_C(40000)
#define DIRECT_GATEWAY_RX_COMPLETION_GUARD_US UINT64_C(1)

_Static_assert(DIRECT_GATEWAY_ACK_SERVICE_US <=
                   (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u,
               "direct gateway ACK service must fit the production RX bound");

enum scenario_kind {
    SCENARIO_ONE_HOP = 0,
    SCENARIO_BUSY_LINE,
    SCENARIO_QUEUE_FULL,
};

struct runner_config {
    enum scenario_kind scenario;
    struct mesh_sim_fault_config faults;
    const char *trace_path;
    const char *program;
    uint32_t seed;
    uint32_t max_steps;
    uint32_t reset_step;
    uint8_t packets_per_node;
    bool reset_enabled;
    bool quiet;
};

struct expected_transaction {
    uint64_t src_id;
    uint32_t session_id;
    uint16_t seq;
};

struct runner {
    struct mesh_sim_world *world;
    struct runner_config config;
    uint8_t nodes[MAX_PATH_NODES + 1u];
    uint16_t connections[MAX_PATH_NODES];
    size_t path_node_count;
    size_t connection_count;
    size_t expected_deliveries;
    struct expected_transaction transactions[MAX_EXPECTED_TRANSACTIONS];
    size_t transaction_count;
    uint8_t gateway;
    uint8_t reset_target;
    uint32_t steps_run;
    uint64_t first_delivery_us;
    uint64_t last_delivery_us;
    uint64_t operation_started_us;
    uint64_t direct_gateway_turns;
    uint64_t direct_gateway_ack_turns;
    uint64_t max_direct_gateway_ack_turnaround_us;
    uint32_t consecutive_empty_channel9_events;
    uint32_t max_consecutive_empty_channel9_events;
    uint32_t post_completion_steps;
    uint32_t post_completion_empty_channel9_events;
};

static int run_next_pending_event(struct runner *runner);

static struct mesh_sim_world world;

static const char *scenario_name(enum scenario_kind scenario)
{
    switch (scenario) {
    case SCENARIO_ONE_HOP:
        return "one-hop";
    case SCENARIO_BUSY_LINE:
        return "busy-line";
    case SCENARIO_QUEUE_FULL:
        return "queue-full";
    }
    return "unknown";
}

static const char *transition_name(enum mesh_sim_transition_kind kind)
{
    switch (kind) {
    case MESH_SIM_TRANSITION_PACKET_QUEUED:
        return "packet-queued";
    case MESH_SIM_TRANSITION_PACKET_DELIVERED:
        return "packet-delivered";
    case MESH_SIM_TRANSITION_GATEWAY_ACKED:
        return "gateway-acked";
    case MESH_SIM_TRANSITION_HOP_PROGRESS:
        return "hop-progress";
    case MESH_SIM_TRANSITION_RETRY_READY:
        return "retry-ready";
    case MESH_SIM_TRANSITION_ROUTE_REQUIRED:
        return "route-required";
    case MESH_SIM_TRANSITION_CONNECTION_EVENT:
        return "connection-event";
    case MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED:
        return "connection-repair-started";
    case MESH_SIM_TRANSITION_CONNECTION_REPAIRED:
        return "connection-repaired";
    case MESH_SIM_TRANSITION_WATCHDOG_EXPIRED:
        return "watchdog-expired";
    case MESH_SIM_TRANSITION_WATCHDOG_RESET:
        return "node-reset";
    case MESH_SIM_TRANSITION_FAULT_DELAYED:
        return "fault-delay";
    case MESH_SIM_TRANSITION_FAULT_FRAME_DROPPED:
        return "fault-frame-loss";
    case MESH_SIM_TRANSITION_FAULT_ACK_DROPPED:
        return "fault-ack-loss";
    case MESH_SIM_TRANSITION_FAULT_DUPLICATED:
        return "fault-duplicate";
    case MESH_SIM_TRANSITION_PACKET_COALESCED:
        return "packet-coalesced";
    default:
        return "event";
    }
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_rate(const char *text, uint16_t *value)
{
    uint32_t parsed;

    if (parse_u32(text, &parsed) != 0 ||
        parsed > MESH_SIM_FAULT_RATE_SCALE) {
        return -1;
    }
    *value = (uint16_t)parsed;
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--scenario one-hop|busy-line|queue-full] "
            "[--seed N] [--fault-seed N] [--packets 1-%u] "
            "[--loss P] [--ack-loss P] [--duplicate P] [--delay P] "
            "[--max-delay-us N] [--reset-step N] [--max-steps N] "
            "[--trace PATH] [--quiet]\n"
            "fault rates P are parts per %u\n",
            program, MAX_PACKETS_PER_NODE, MESH_SIM_FAULT_RATE_SCALE);
}

static int parse_args(int argc, char **argv, struct runner_config *config)
{
    bool explicit_fault_seed = false;

    *config = (struct runner_config) {
        .scenario = SCENARIO_ONE_HOP,
        .program = argv[0],
        .seed = DEFAULT_SEED,
        .max_steps = DEFAULT_MAX_STEPS,
        .packets_per_node = 2u,
    };
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        uint32_t parsed;

        if (strcmp(arg, "--quiet") == 0) {
            config->quiet = true;
            continue;
        }
        if (strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (value == NULL) {
            return -1;
        }
        if (strcmp(arg, "--scenario") == 0) {
            if (strcmp(value, "one-hop") == 0) {
                config->scenario = SCENARIO_ONE_HOP;
            } else if (strcmp(value, "busy-line") == 0) {
                config->scenario = SCENARIO_BUSY_LINE;
            } else if (strcmp(value, "queue-full") == 0) {
                config->scenario = SCENARIO_QUEUE_FULL;
            } else {
                return -1;
            }
        } else if (strcmp(arg, "--seed") == 0) {
            if (parse_u32(value, &config->seed) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--fault-seed") == 0) {
            if (parse_u32(value, &config->faults.seed) != 0) {
                return -1;
            }
            explicit_fault_seed = true;
        } else if (strcmp(arg, "--packets") == 0) {
            if (parse_u32(value, &parsed) != 0 || parsed == 0u ||
                parsed > MAX_PACKETS_PER_NODE) {
                return -1;
            }
            config->packets_per_node = (uint8_t)parsed;
        } else if (strcmp(arg, "--max-steps") == 0) {
            if (parse_u32(value, &config->max_steps) != 0 ||
                config->max_steps == 0u) {
                return -1;
            }
        } else if (strcmp(arg, "--reset-step") == 0) {
            if (parse_u32(value, &config->reset_step) != 0) {
                return -1;
            }
            config->reset_enabled = true;
        } else if (strcmp(arg, "--loss") == 0) {
            if (parse_rate(value, &config->faults.frame_loss_permyriad) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--ack-loss") == 0) {
            if (parse_rate(value, &config->faults.ack_loss_permyriad) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--duplicate") == 0) {
            if (parse_rate(value, &config->faults.duplicate_permyriad) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--delay") == 0) {
            if (parse_rate(value, &config->faults.delay_permyriad) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--max-delay-us") == 0) {
            if (parse_u32(value, &config->faults.max_extra_delay_us) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--trace") == 0) {
            config->trace_path = value;
        } else {
            return -1;
        }
        i++;
    }
    if (!explicit_fault_seed) {
        config->faults.seed = config->seed ^ UINT32_C(0xF4175EED);
    }
    if (config->faults.delay_permyriad != 0u &&
        config->faults.max_extra_delay_us == 0u) {
        config->faults.max_extra_delay_us = 4000u;
    }
    if (config->reset_enabled && config->scenario != SCENARIO_BUSY_LINE) {
        return -1;
    }
    return 0;
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

static int add_connection(struct runner *runner,
                          uint8_t node_a,
                          uint8_t node_b,
                          uint32_t phase_ms,
                          uint16_t *connection_index)
{
    struct mesh_event_params params = connection_params(phase_ms);
    int ret = mesh_sim_set_link(runner->world, node_a, node_b, 96u, 1u);

    return ret == MESH_SIM_OK ?
           mesh_sim_add_connection(runner->world, node_a, node_b, &params,
                                   true, connection_index) : ret;
}

static int build_packet(uint64_t source_id,
                        uint16_t seq,
                        uint32_t packet_id,
                        struct proto_packet *packet,
                        uint8_t *payload,
                        size_t payload_capacity,
                        size_t *payload_len)
{
    size_t length = 0u;
    int ret = tlv_append_u32(payload, payload_capacity, &length,
                             TLV_MESH_TEST_PACKET_ID, packet_id);

    if (ret == PROTO_OK) {
        ret = tlv_append_u8(payload, payload_capacity, &length,
                            TLV_MESH_TEST_ATTEMPT, 1u);
    }
    if (ret != PROTO_OK || length > UINT16_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    *packet = (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = source_id,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x71000000) ^ (uint32_t)source_id ^ packet_id,
        .seq = seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = (uint16_t)length,
    };
    *payload_len = length;
    return PROTO_OK;
}

static int install_line(struct runner *runner, size_t path_node_count)
{
    struct mesh_sim_world *sim = runner->world;
    int ret;

    runner->path_node_count = path_node_count;
    runner->connection_count = path_node_count > 0u ? path_node_count - 1u : 0u;
    for (size_t i = 0u; i < path_node_count; i++) {
        ret = mesh_sim_add_role(sim,
                                i == 0u ? MESH_SIM_ROLE_TRANSMITTER :
                                          MESH_SIM_ROLE_ANCHOR,
                                NODE_ID_BASE + i,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &runner->nodes[i]);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = mesh_sim_set_tx_queue_capacity(
            sim, runner->nodes[i],
            MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    ret = mesh_sim_add_role(sim, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &runner->gateway);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    runner->nodes[path_node_count] = runner->gateway;
    for (size_t i = 0u; i < path_node_count; i++) {
        if (i + 1u < path_node_count) {
            ret = add_connection(runner, runner->nodes[i],
                                 runner->nodes[i + 1u],
                                 100u + (uint32_t)i * 45u,
                                 &runner->connections[i]);
        } else {
            ret = mesh_sim_set_link(sim, runner->nodes[i], runner->gateway,
                                    96u, 1u);
        }
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = mesh_sim_install_route(sim, runner->nodes[i],
                                     runner->nodes[i + 1u],
                                     (uint8_t)(path_node_count - i - 1u),
                                     ROUTE_EPOCH);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    for (size_t relay = 1u; relay <= path_node_count; relay++) {
        for (size_t target = 0u; target < relay; target++) {
            ret = mesh_sim_install_downlink(sim,
                                            runner->nodes[relay],
                                            sim->roles[runner->nodes[target]].id,
                                            runner->nodes[relay - 1u],
                                            (uint8_t)(relay - target),
                                            ROUTE_EPOCH);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    return MESH_SIM_OK;
}

static int queue_packets(struct runner *runner)
{
    for (size_t origin = 0u; origin < runner->path_node_count; origin++) {
        if (runner->config.reset_enabled &&
            runner->nodes[origin] == runner->reset_target) {
            continue;
        }
        for (uint8_t index = 0u; index < runner->config.packets_per_node;
             index++) {
            struct proto_packet packet;
            uint8_t payload[32];
            size_t payload_len = 0u;
            uint16_t seq = (uint16_t)((origin + 1u) * 256u + index + 1u);
            uint32_t packet_id = ((uint32_t)origin << 16) | index;
            int ret = build_packet(
                runner->world->roles[runner->nodes[origin]].id,
                seq, packet_id, &packet, payload, sizeof(payload), &payload_len);

            if (ret != PROTO_OK) {
                return ret;
            }
            ret = mesh_sim_queue_originated(runner->world,
                                            runner->nodes[origin],
                                            &packet, payload, payload_len);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
            if (runner->transaction_count >= MAX_EXPECTED_TRANSACTIONS) {
                return MESH_SIM_ERR_CAPACITY;
            }
            runner->transactions[runner->transaction_count++] =
                (struct expected_transaction) {
                    .src_id = packet.src_id,
                    .session_id = packet.session_id,
                    .seq = packet.seq,
                };
            runner->expected_deliveries++;
        }
    }
    return MESH_SIM_OK;
}

static int run_next_connection(struct runner *runner,
                               bool *empty_channel9_event)
{
    size_t selected = SIZE_MAX;
    uint64_t earliest = UINT64_MAX;
    int first_error = MESH_SIM_OK;

    if (empty_channel9_event == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    *empty_channel9_event = false;
    for (size_t i = 0u; i < runner->connection_count; i++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(
            runner->world, runner->connections[i], &action);

        if (ret != MESH_SIM_OK) {
            if (first_error == MESH_SIM_OK) {
                first_error = ret;
            }
        } else if (action.kind != MESH_SIM_CONNECTION_ACTION_NONE &&
                   action.start_us >= runner->world->now_us &&
                   action.start_us < earliest) {
            selected = i;
            earliest = action.start_us;
        }
    }
    if (selected == SIZE_MAX) {
        return first_error == MESH_SIM_OK ? run_next_pending_event(runner) :
                                            first_error;
    }
    {
        struct mesh_sim_connection_action action;
        uint16_t connection = runner->connections[selected];
        uint64_t connection_events_before =
            runner->world->connection_event_total_count;
        int ret = mesh_sim_connection_next_action(runner->world,
                                                  connection, &action);

        if (ret == MESH_SIM_OK && !action.already_scheduled) {
            ret = mesh_sim_schedule_next_connection_event(runner->world,
                                                          connection, false);
        }
        if (ret == MESH_SIM_OK) {
            ret = mesh_sim_run_until(runner->world, action.end_us);
        }
        if (ret == MESH_SIM_OK &&
            action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT &&
            runner->world->connection_event_total_count >
                connection_events_before &&
            runner->world->connection_event_count > 0u) {
            const struct mesh_sim_connection_event *event =
                &runner->world->connection_events[
                    runner->world->connection_event_count - 1u];

            *empty_channel9_event = !event->had_packet;
        }
        return ret;
    }
}

static int direct_gateway_queue_index(const struct runner *runner)
{
    const struct mesh_sim_role_instance *sender;
    size_t best = SIZE_MAX;

    if (runner->path_node_count == 0u) {
        return -1;
    }
    sender = &runner->world->roles[
        runner->nodes[runner->path_node_count - 1u]];
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *candidate = &sender->tx_queue[i];

        if (!candidate->valid ||
            candidate->outbound.next_hop_id != GATEWAY_ID) {
            continue;
        }
        if (!mesh_sim_relay_queue_entry_runnable(sender,
                                                 candidate,
                                                 GATEWAY_ID)) {
            continue;
        }
        if (best == SIZE_MAX || candidate->priority >
                sender->tx_queue[best].priority ||
            (candidate->priority == sender->tx_queue[best].priority &&
             candidate->enqueue_order <
                sender->tx_queue[best].enqueue_order)) {
            best = i;
        }
    }
    return best == SIZE_MAX ? -1 : (int)best;
}

static bool gateway_ack_queued_for_sender(const struct runner *runner,
                                          uint8_t sender_index)
{
    const struct mesh_sim_role_instance *gateway =
        &runner->world->roles[runner->gateway];
    uint64_t sender_id = runner->world->roles[sender_index].id;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &gateway->tx_queue[i];

        if (queued->valid && !queued->needs_relay_start &&
            queued->outbound.packet.msg_type == MSG_GATEWAY_ACK &&
            queued->outbound.next_hop_id == sender_id) {
            return true;
        }
    }
    return false;
}

static uint64_t transmission_arrival_end_us(
    const struct mesh_sim_world *sim,
    uint16_t transmission_index,
    uint8_t receiver_index)
{
    const struct mesh_sim_transmission *tx =
        &sim->transmissions[transmission_index];
    uint64_t delay_us = sim->propagation_us[tx->node_index][receiver_index] +
                        tx->fault_extra_delay_us[receiver_index];

    if (tx->end_us > UINT64_MAX - delay_us) {
        return UINT64_MAX;
    }
    return tx->end_us + delay_us;
}

static int run_direct_gateway_turn(struct runner *runner)
{
    struct mesh_sim_world *sim = runner->world;
    uint8_t sender = runner->nodes[runner->path_node_count - 1u];
    uint64_t runtime_ready_us = sim->now_us;
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

    if (sim->connection_count != runner->connection_count) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (sim->roles[sender].dwm3000.cpu_busy_until_us > runtime_ready_us) {
        runtime_ready_us = sim->roles[sender].dwm3000.cpu_busy_until_us;
    }
    if (sim->roles[runner->gateway].dwm3000.cpu_busy_until_us >
            runtime_ready_us) {
        runtime_ready_us =
            sim->roles[runner->gateway].dwm3000.cpu_busy_until_us;
    }
    if (runtime_ready_us > sim->now_us) {
        ret = mesh_sim_run_until(sim, runtime_ready_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    payload_air_start_us = sim->now_us + DIRECT_GATEWAY_TX_PREPARE_US;
    payload_deadline_us = payload_air_start_us +
                          DIRECT_GATEWAY_PAYLOAD_SERVICE_US;
    ret = mesh_sim_direct_gateway_start_queued_tx(
        sim, sender, payload_air_start_us, payload_deadline_us, &payload_tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    payload_arrival_end_us = transmission_arrival_end_us(
        sim, payload_tx, runner->gateway);
    if (payload_arrival_end_us == UINT64_MAX) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    gateway_rx_end_us = payload_arrival_end_us +
                        DIRECT_GATEWAY_RX_COMPLETION_GUARD_US;
    ret = mesh_sim_direct_gateway_arm_rx(sim, runner->gateway,
                                         payload_air_start_us,
                                         gateway_rx_end_us);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(sim, gateway_rx_end_us);
    }
    runner->direct_gateway_turns++;
    if (ret != MESH_SIM_OK ||
        !gateway_ack_queued_for_sender(runner, sender)) {
        return ret;
    }
    if (sim->roles[runner->gateway].dwm3000.cpu_busy_until_us > sim->now_us) {
        ret = mesh_sim_run_until(
            sim, sim->roles[runner->gateway].dwm3000.cpu_busy_until_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    ack_air_start_us = sim->now_us +
                       (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS * 1000u;
    ack_window_end_us = ack_air_start_us + DIRECT_GATEWAY_ACK_SERVICE_US;
    ret = mesh_sim_direct_gateway_schedule_ack(
        sim, runner->gateway, sender, ack_air_start_us, ack_window_end_us,
        &ack_tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ack_turnaround_us = sim->transmissions[ack_tx].start_us -
                        payload_arrival_end_us;
    if (sim->transmissions[ack_tx].start_us <
            payload_arrival_end_us +
                (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS * 1000u ||
        sim->transmissions[ack_tx].end_us >
            payload_arrival_end_us +
                (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u) {
        fprintf(stderr,
                "direct gateway ACK escaped same turn: turnaround=%" PRIu64
                " us ack_end=%" PRIu64 " bound_end=%" PRIu64 "\n",
                ack_turnaround_us, sim->transmissions[ack_tx].end_us,
                payload_arrival_end_us +
                    (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u);
        return MESH_SIM_ERR_PROTOCOL;
    }
    runner->direct_gateway_ack_turns++;
    if (ack_turnaround_us > runner->max_direct_gateway_ack_turnaround_us) {
        runner->max_direct_gateway_ack_turnaround_us = ack_turnaround_us;
    }
    return mesh_sim_run_until(sim, ack_window_end_us);
}

static int run_next_pending_event(struct runner *runner)
{
    uint64_t earliest_us = UINT64_MAX;

    for (size_t i = 0u; i < runner->world->event_count; i++) {
        const struct mesh_sim_event *event = &runner->world->events[i];

        if (event->pending && event->time_us >= runner->world->now_us &&
            event->time_us < earliest_us) {
            earliest_us = event->time_us;
        }
    }
    return earliest_us == UINT64_MAX ? MESH_SIM_ERR_EVENT_ORDER :
                                       mesh_sim_run_until(runner->world,
                                                          earliest_us);
}

static int run_next_radio_turn(struct runner *runner,
                               bool *empty_channel9_event)
{
    if (direct_gateway_queue_index(runner) >= 0) {
        *empty_channel9_event = false;
        return run_direct_gateway_turn(runner);
    }
    if (runner->connection_count != 0u) {
        return run_next_connection(runner, empty_channel9_event);
    }
    *empty_channel9_event = false;
    return run_next_pending_event(runner);
}

static bool network_idle(const struct runner *runner)
{
    for (size_t i = 0u; i < runner->world->role_count; i++) {
        const struct mesh_sim_role_instance *node = &runner->world->roles[i];

        if (node->tx_queue_count != 0u || node->route_waiting_valid ||
            (node->relay_initialized &&
             node->relay.pending.state != MESH_RELAY_TX_IDLE)) {
            return false;
        }
    }
    return !mesh_sim_has_pending_finite_work(runner->world);
}

static int schedule_relay_deadlines(struct runner *runner)
{
    for (size_t i = 0u; i < runner->world->role_count; i++) {
        const struct mesh_pending_tx *pending =
            &runner->world->roles[i].relay.pending;
        uint32_t deadline_ms = 0u;
        uint64_t deadline_us;

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
        if (deadline_us < runner->world->now_us) {
            deadline_us = runner->world->now_us;
        }
        {
            int ret = mesh_sim_schedule_relay_tick(
                runner->world, (uint8_t)i, deadline_us);

            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    return MESH_SIM_OK;
}

static int feed_idle_watchdogs(struct runner *runner)
{
    for (size_t i = 0u; i < runner->world->role_count; i++) {
        const struct mesh_sim_watchdog *watchdog =
            &runner->world->roles[i].watchdog;
        int ret;

        if (!watchdog->armed || watchdog->expired ||
            watchdog->workers_active != 0u) {
            continue;
        }
        ret = mesh_sim_watchdog_feed(runner->world, (uint8_t)i);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return MESH_SIM_OK;
}

static int check_invariants(const struct runner *runner, bool settled)
{
    struct mesh_sim_invariant_report report;
    int ret = settled ? mesh_sim_check_settled(runner->world, &report) :
                        mesh_sim_check_invariants(runner->world, &report);

    if (ret != MESH_SIM_OK) {
        fprintf(stderr,
                "invariant=%s node=%zu object=%zu detail=%" PRIu64
                " reason=%s\n",
                mesh_sim_invariant_name(report.code), report.node_index,
                report.object_index, report.detail,
                report.description == NULL ? "unknown" : report.description);
    }
    return ret;
}

static uint64_t operation_liveness_bound_us(const struct runner *runner)
{
    size_t admitted_transactions = runner->transaction_count == 0u ?
                                   1u : runner->transaction_count;
    const struct mesh_event_params params = connection_params(0u);
    size_t path_phases = runner->path_node_count == 0u ?
                         1u : runner->path_node_count;
    uint64_t settlement_allowance_us =
        ((uint64_t)params.event_interval_ms * path_phases +
         params.event_window_ms) * 1000u +
        DIRECT_GATEWAY_TX_PREPARE_US +
        DIRECT_GATEWAY_PAYLOAD_SERVICE_US +
        DIRECT_GATEWAY_ACK_SERVICE_US +
        DIRECT_GATEWAY_RX_COMPLETION_GUARD_US +
        runner->config.faults.max_extra_delay_us;

    return (uint64_t)MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS * 1000u *
           admitted_transactions * ROUTE_MAX_CANDIDATES +
           settlement_allowance_us;
}

static uint64_t transaction_retry_bound(const struct runner *runner)
{
    uint64_t bounded_backoff_retries =
        operation_liveness_bound_us(runner) /
        ((uint64_t)RELAY_BUSY_RETRY_MIN_MS * 1000u);
    uint64_t immediate_parent_switches =
        (uint64_t)(runner->path_node_count == 0u ? 1u :
                   runner->path_node_count) *
        ROUTE_MAX_CANDIDATES;

    /*
     * A durable transaction may revisit a parent after hold-down and route
     * repair, so ROUTE_MAX_FAILURES is not a transaction-wide retry budget.
     * Bound retry rate by the shortest production backoff and allow one
     * immediate switch per candidate at every custody owner.
     */
    return bounded_backoff_retries + immediate_parent_switches;
}

static void update_delivery_timestamps(struct runner *runner)
{
    const struct mesh_sim_role_instance *gateway =
        &runner->world->roles[runner->gateway];

    if (gateway->delivery_count == 0u) {
        return;
    }
    runner->first_delivery_us = gateway->deliveries[0].delivered_at_us;
    runner->last_delivery_us =
        gateway->deliveries[gateway->delivery_count - 1u].delivered_at_us;
}

static int incremental_liveness_checks(const struct runner *runner)
{
    const struct mesh_sim_role_instance *gateway =
        &runner->world->roles[runner->gateway];
    uint64_t elapsed_us = runner->world->now_us - runner->operation_started_us;
    uint64_t bound_us = operation_liveness_bound_us(runner);
    uint32_t post_completion_step_bound =
        (uint32_t)(runner->path_node_count == 0u ? 1u :
                   runner->path_node_count) *
        MAX_POST_COMPLETION_STEPS_PER_PATH_NODE +
        (uint32_t)runner->connection_count * 2u +
        POST_COMPLETION_STEP_FIXED_ALLOWANCE;
    uint32_t post_completion_empty_bound =
        (uint32_t)(runner->connection_count == 0u ? 1u :
                   runner->connection_count) *
        MAX_POST_COMPLETION_EMPTY_CHANNEL9_PER_CONNECTION +
        POST_COMPLETION_EMPTY_CHANNEL9_FIXED_ALLOWANCE;

    if (elapsed_us > bound_us &&
        (gateway->delivery_count != runner->expected_deliveries ||
         !network_idle(runner))) {
        fprintf(stderr,
                "operation liveness bound exceeded: elapsed=%" PRIu64
                " us bound=%" PRIu64 " us deliveries=%zu/%zu idle=%u\n",
                elapsed_us, bound_us, gateway->delivery_count,
                runner->expected_deliveries, network_idle(runner) ? 1u : 0u);
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (runner->post_completion_steps > post_completion_step_bound) {
        fprintf(stderr,
                "post-completion polling bound exceeded: actual=%" PRIu32
                " bound=%" PRIu32 "\n",
                runner->post_completion_steps, post_completion_step_bound);
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (runner->post_completion_empty_channel9_events >
        post_completion_empty_bound) {
        fprintf(stderr,
                "post-completion empty Channel-9 bound exceeded: actual=%"
                PRIu32 " bound=%" PRIu32 "\n",
                runner->post_completion_empty_channel9_events,
                post_completion_empty_bound);
        return MESH_SIM_ERR_PROTOCOL;
    }
    return MESH_SIM_OK;
}

static bool transition_matches_transaction(
    const struct mesh_sim_transition *transition,
    const struct expected_transaction *transaction)
{
    return transition->packet_src_id == transaction->src_id &&
           transition->packet_session_id == transaction->session_id &&
           transition->packet_seq == transaction->seq;
}

static bool bounded_control_type(uint8_t msg_type)
{
    switch (msg_type) {
    case MSG_MESH_HOP_ACK:
    case MSG_GATEWAY_ACK:
    case MSG_ROUTE_REQ:
    case MSG_ROUTE_REPLY:
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
    case MSG_ROUTE_REPLY_ACK:
    case MSG_GATEWAY_ROUTE_ADV:
    case MSG_RELAY_BUSY:
    case MSG_RESULT_BUSY:
    case MSG_GATEWAY_ROUTE_REQ:
    case MSG_COMMAND:
    case MSG_RESULT_OFFER:
    case MSG_RESULT_GRANT:
    case MSG_GATEWAY_COLLECTION_EACK:
    case MSG_SURVEY_REACH_REQ:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_DISCOVERY_START:
    case MSG_GATEWAY_COMMAND_EVENT:
        return true;
    default:
        return false;
    }
}

static bool same_control_semantics(const struct mesh_sim_transmission *a,
                                   const struct mesh_sim_transmission *b)
{
    return a->valid && b->valid && a->has_outbound && b->has_outbound &&
           a->outbound.packet.msg_type == b->outbound.packet.msg_type &&
           a->outbound.packet.src_id == b->outbound.packet.src_id &&
           a->outbound.packet.dst_id == b->outbound.packet.dst_id &&
           a->outbound.packet.session_id == b->outbound.packet.session_id &&
           a->outbound.payload_len == b->outbound.payload_len &&
           memcmp(a->outbound.payload, b->outbound.payload,
                  a->outbound.payload_len) == 0;
}

static int traffic_liveness_checks(const struct runner *runner)
{
    const struct mesh_sim_world *sim = runner->world;
    uint32_t identical_control_bound =
        ((uint32_t)runner->path_node_count + 1u) *
        ROUTE_MAX_FAILURES * ROUTE_MAX_CANDIDATES +
        IDENTICAL_CONTROL_FIXED_ALLOWANCE;
    uint64_t traffic_bound = TRAFFIC_BOUND_FIXED_ALLOWANCE +
        (uint64_t)runner->transaction_count * runner->path_node_count *
        (ROUTE_MAX_FAILURES + 1u) * 6u;
    uint64_t settle_bound_us = operation_liveness_bound_us(runner);

    if (sim->connection_event_dropped_count != 0u ||
        sim->rx_window_dropped_count != 0u ||
        sim->transmission_dropped_count != 0u ||
        sim->reception_dropped_count != 0u ||
        mesh_sim_trace_is_truncated(sim)) {
        fprintf(stderr, "liveness telemetry truncated; bounds are unprovable\n");
        return MESH_SIM_ERR_CAPACITY;
    }
    if (sim->transmission_total_count > traffic_bound) {
        fprintf(stderr,
                "RF transmission bound exceeded: actual=%" PRIu64
                " bound=%" PRIu64 "\n",
                sim->transmission_total_count, traffic_bound);
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (runner->max_consecutive_empty_channel9_events >
        MAX_CONSECUTIVE_EMPTY_CHANNEL9_EVENTS) {
        fprintf(stderr,
                "empty Channel-9 polling bound exceeded: actual=%" PRIu32
                " bound=%u\n",
                runner->max_consecutive_empty_channel9_events,
                MAX_CONSECUTIVE_EMPTY_CHANNEL9_EVENTS);
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (runner->last_delivery_us != 0u &&
        sim->now_us - runner->last_delivery_us > settle_bound_us) {
        fprintf(stderr,
                "post-delivery settlement bound exceeded: actual=%" PRIu64
                " us bound=%" PRIu64 " us\n",
                sim->now_us - runner->last_delivery_us, settle_bound_us);
        return MESH_SIM_ERR_PROTOCOL;
    }
    for (size_t i = 0u; i < sim->transmission_count; i++) {
        const struct mesh_sim_transmission *candidate = &sim->transmissions[i];
        uint32_t repeats = 0u;

        if (!candidate->valid || !candidate->has_outbound ||
            !bounded_control_type(candidate->outbound.packet.msg_type)) {
            continue;
        }
        for (size_t j = 0u; j < sim->transmission_count; j++) {
            repeats += same_control_semantics(candidate,
                                              &sim->transmissions[j]) ? 1u : 0u;
        }
        /*
         * A semantically identical gateway ACK retains its end-to-end packet
         * identity while traversing each hop, and the gateway can regenerate
         * it after a lost upstream payload attempt. Bound physical copies by
         * one production per-parent failure budget for every custody owner
         * plus the gateway, with a fixed handoff allowance.
         */
        if (repeats > identical_control_bound) {
            fprintf(stderr,
                    "identical control traffic bound exceeded: msg=%u src=%" PRIx64
                    " dst=%" PRIx64 " session=%" PRIu32
                    " actual=%" PRIu32 " bound=%u\n",
                    candidate->outbound.packet.msg_type,
                    candidate->outbound.packet.src_id,
                    candidate->outbound.packet.dst_id,
                    candidate->outbound.packet.session_id,
                    repeats, identical_control_bound);
            return MESH_SIM_ERR_PROTOCOL;
        }
    }
    for (size_t i = 0u; i < sim->role_count; i++) {
        if (sim->roles[i].radio_state != MESH_SIM_RADIO_SLEEP) {
            fprintf(stderr, "node %zu did not return to idle radio duty state\n", i);
            return MESH_SIM_ERR_PROTOCOL;
        }
    }
    return MESH_SIM_OK;
}

static int terminal_checks(struct runner *runner)
{
    uint64_t retry_bound = transaction_retry_bound(runner);
    int ret;

    if (runner->world->connection_count != runner->connection_count ||
        (runner->expected_deliveries != 0u &&
         runner->direct_gateway_ack_turns < runner->expected_deliveries) ||
        runner->max_direct_gateway_ack_turnaround_us >
            (uint64_t)APP_MESH_DIRECT_GATEWAY_ACK_RX_MS * 1000u) {
        fprintf(stderr,
                "direct gateway terminal bound failed: connections=%zu/%zu "
                "acks=%" PRIu64 "/%zu max_turnaround=%" PRIu64 " us\n",
                runner->world->connection_count, runner->connection_count,
                runner->direct_gateway_ack_turns,
                runner->expected_deliveries,
                runner->max_direct_gateway_ack_turnaround_us);
        return MESH_SIM_ERR_PROTOCOL;
    }
    if (mesh_sim_trace_is_truncated(runner->world)) {
        return MESH_SIM_ERR_CAPACITY;
    }
    for (size_t i = 0u; i < runner->world->connection_count; i++) {
        ret = mesh_sim_expire_connection_ownership(
            runner->world, (uint16_t)i, NULL);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    for (size_t transaction_index = 0u;
         transaction_index < runner->transaction_count;
         transaction_index++) {
        const struct expected_transaction *transaction =
            &runner->transactions[transaction_index];
        uint32_t completions = 0u;
        uint32_t retries = 0u;

        for (size_t i = 0u; i < runner->world->transition_count; i++) {
            const struct mesh_sim_transition *transition =
                &runner->world->transitions[i];

            if (!transition_matches_transaction(transition, transaction)) {
                continue;
            }
            if (transition->kind == MESH_SIM_TRANSITION_GATEWAY_ACKED &&
                transition->node_id == transaction->src_id) {
                completions++;
            } else if (transition->kind == MESH_SIM_TRANSITION_RETRY_READY) {
                retries++;
            }
        }
        if (completions != 1u || retries > retry_bound) {
            fprintf(stderr,
                    "transaction bound failed: src=%" PRIx64
                    " session=%" PRIu32 " seq=%u completions=%" PRIu32
                    " retries=%" PRIu32 " retry_bound=%" PRIu64 "\n",
                    transaction->src_id, transaction->session_id,
                    transaction->seq, completions, retries, retry_bound);
            return MESH_SIM_ERR_PROTOCOL;
        }
    }
    ret = traffic_liveness_checks(runner);
    return ret == MESH_SIM_OK ? check_invariants(runner, true) : ret;
}

static int run_queue_full(struct runner *runner)
{
    int ret = install_line(runner, 1u);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    for (uint16_t seq = 1u;
         seq <= MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY + 1u; seq++) {
        struct proto_packet packet;
        uint8_t payload[32];
        size_t payload_len = 0u;

        ret = build_packet(runner->world->roles[runner->nodes[0]].id,
                           seq, seq, &packet, payload, sizeof(payload),
                           &payload_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = mesh_sim_queue_originated(runner->world, runner->nodes[0],
                                        &packet, payload, payload_len);
        if ((seq <= MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY &&
             ret != MESH_SIM_OK) ||
            (seq > MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY &&
             ret != MESH_SIM_ERR_CAPACITY)) {
            return MESH_SIM_ERR_PROTOCOL;
        }
    }
    runner->expected_deliveries =
        MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY;
    if (runner->world->roles[runner->nodes[0]].tx_queue_count !=
            runner->expected_deliveries ||
        runner->world->last_error != MESH_SIM_OK) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    return check_invariants(runner, false);
}

static int run_delivery(struct runner *runner)
{
    int ret = install_line(runner,
                           runner->config.scenario == SCENARIO_ONE_HOP ?
                           1u : 3u);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    runner->reset_target = runner->path_node_count > 1u ?
                           runner->nodes[1] : runner->nodes[0];
    ret = mesh_sim_set_fault_config(runner->world, &runner->config.faults);
    if (ret == MESH_SIM_OK) {
        ret = queue_packets(runner);
    }
    for (size_t i = 0u; ret == MESH_SIM_OK && i < runner->world->role_count;
         i++) {
        ret = mesh_sim_watchdog_arm(
            runner->world, (uint8_t)i,
            MESH_SIM_WATCHDOG_PRODUCTION_TIMEOUT_US,
            MESH_SIM_WATCHDOG_FAIL);
    }
    if (ret != MESH_SIM_OK || check_invariants(runner, false) != MESH_SIM_OK) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    runner->operation_started_us = runner->world->now_us;
    for (uint32_t step = 0u; step < runner->config.max_steps; step++) {
        bool empty_channel9_event = false;

        if (runner->world->roles[runner->gateway].delivery_count ==
                runner->expected_deliveries && network_idle(runner)) {
            runner->steps_run = step;
            break;
        }
        if (runner->config.reset_enabled && step == runner->config.reset_step) {
            ret = mesh_sim_reset_role(runner->world, runner->reset_target);
        }
        if (ret == MESH_SIM_OK) {
            ret = schedule_relay_deadlines(runner);
        }
        if (ret == MESH_SIM_OK) {
            ret = run_next_radio_turn(runner, &empty_channel9_event);
        }
        if (ret == MESH_SIM_OK) {
            ret = feed_idle_watchdogs(runner);
        }
        update_delivery_timestamps(runner);
        if (ret == MESH_SIM_OK &&
            runner->world->roles[runner->gateway].delivery_count ==
                runner->expected_deliveries && !network_idle(runner)) {
            runner->post_completion_steps++;
            if (empty_channel9_event) {
                runner->post_completion_empty_channel9_events++;
            }
        }
        if (ret == MESH_SIM_OK && empty_channel9_event) {
            runner->consecutive_empty_channel9_events++;
            if (runner->consecutive_empty_channel9_events >
                runner->max_consecutive_empty_channel9_events) {
                runner->max_consecutive_empty_channel9_events =
                    runner->consecutive_empty_channel9_events;
            }
        } else if (ret == MESH_SIM_OK) {
            runner->consecutive_empty_channel9_events = 0u;
        }
        if (runner->max_consecutive_empty_channel9_events >
            MAX_CONSECUTIVE_EMPTY_CHANNEL9_EVENTS) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        if (ret == MESH_SIM_OK) {
            ret = incremental_liveness_checks(runner);
        }
        if (ret != MESH_SIM_OK ||
            check_invariants(runner, false) != MESH_SIM_OK) {
            return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
        }
        runner->steps_run = step + 1u;
    }
    if (runner->world->roles[runner->gateway].delivery_count !=
            runner->expected_deliveries || !network_idle(runner)) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    update_delivery_timestamps(runner);
    return terminal_checks(runner);
}

static void print_replay(const struct runner_config *config, FILE *stream)
{
    fprintf(stream,
            "%s --scenario %s --seed 0x%08" PRIx32
            " --fault-seed 0x%08" PRIx32 " --packets %u"
            " --loss %u --ack-loss %u --duplicate %u --delay %u"
            " --max-delay-us %" PRIu32 " --max-steps %" PRIu32,
            config->program, scenario_name(config->scenario), config->seed,
            config->faults.seed, config->packets_per_node,
            config->faults.frame_loss_permyriad,
            config->faults.ack_loss_permyriad,
            config->faults.duplicate_permyriad,
            config->faults.delay_permyriad,
            config->faults.max_extra_delay_us, config->max_steps);
    if (config->reset_enabled) {
        fprintf(stream, " --reset-step %" PRIu32, config->reset_step);
    }
    fputc('\n', stream);
}

static void dump_tail(const struct mesh_sim_world *sim, size_t count)
{
    size_t start = sim->transition_count > count ?
                   sim->transition_count - count : 0u;

    for (size_t i = start; i < sim->transition_count; i++) {
        const struct mesh_sim_transition *entry = &sim->transitions[i];

        fprintf(stderr,
                "trace[%zu] t=%" PRIu64 " kind=%s/%u node=%" PRIx64
                " peer=%" PRIx64 " msg=%u detail=%" PRIu32
                " tx={src=%" PRIx64 " dst=%" PRIx64
                " session=%" PRIu32 " seq=%u}"
                " fault_ordinal=%" PRIu64 "\n",
                i, entry->time_us, transition_name(entry->kind),
                (unsigned int)entry->kind, entry->node_id, entry->peer_id,
                entry->msg_type, entry->detail,
                entry->packet_src_id, entry->packet_dst_id,
                entry->packet_session_id, entry->packet_seq,
                entry->fault_decision_ordinal);
    }
}

static void dump_nodes(const struct mesh_sim_world *sim)
{
    if (sim->last_error_file != NULL) {
        fprintf(stderr, "failure_source %s:%" PRIu32 "\n",
                sim->last_error_file, sim->last_error_line);
    }
    if (sim->last_error_event_valid) {
        fprintf(stderr,
                "failure_event type=%u object=%u time_us=%" PRIu64 "\n",
                sim->last_error_event_type,
                sim->last_error_event_object_index,
                sim->last_error_event_time_us);
    }
    for (size_t i = 0u; i < sim->role_count; i++) {
        const struct mesh_sim_role_instance *node = &sim->roles[i];
        size_t runtime_work = 0u;

        for (size_t j = 0u; j < MESH_RUNTIME_WORK_CAPACITY; j++) {
            runtime_work += node->runtime.work[j].valid ? 1u : 0u;
        }
        fprintf(stderr,
                "node[%zu] id=%" PRIx64 " queue=%zu pending=%u pending_seq=%u "
                "next=%" PRIx64 " radio=%u runtime_owner=%u work=%zu "
                "transit=%u repair=%u workers=%" PRIu32 "/%" PRIu32
                "/%" PRIu32 " deliveries=%zu "
                "route_waiting=%u epoch=%" PRIu32 "\n",
                i, node->id, node->tx_queue_count,
                (unsigned int)node->relay.pending.state,
                node->relay.pending.packet.seq,
                node->relay.pending.next_hop_id,
                (unsigned int)node->radio_state,
                (unsigned int)node->runtime.radio_owner, runtime_work,
                node->runtime.transit_reserved,
                node->runtime.event_repair_pending,
                node->watchdog.workers_active,
                node->watchdog.workers_started,
                node->watchdog.workers_completed,
                node->delivery_count,
                node->route_waiting_valid, node->work_epoch);
        for (size_t j = 0u; j < MESH_SIM_TX_QUEUE_CAPACITY; j++) {
            const struct mesh_sim_queued_tx *queued = &node->tx_queue[j];

            if (!queued->valid) {
                continue;
            }
            fprintf(stderr,
                    "  queue[%zu] msg=%u src=%" PRIx64 " dst=%" PRIx64
                    " session=%" PRIu32 " seq=%u next=%" PRIx64
                    " priority=%u relay_start=%u order=%" PRIu32 "\n",
                    j, queued->outbound.packet.msg_type,
                    queued->outbound.packet.src_id,
                    queued->outbound.packet.dst_id,
                    queued->outbound.packet.session_id,
                    queued->outbound.packet.seq,
                    queued->outbound.next_hop_id, queued->priority,
                    queued->needs_relay_start, queued->enqueue_order);
        }
    }
    for (size_t i = 0u; i < sim->connection_count; i++) {
        struct mesh_sim_connection_action action = {0};
        int ret = mesh_sim_connection_next_action(sim, (uint16_t)i, &action);

        fprintf(stderr,
                "connection[%zu] ret=%d kind=%u start=%" PRIu64
                " end=%" PRIu64 " skipped=%u scheduled=%u valid=%u "
                "fresh=%u/%u fallback=%u/%u repairs=%" PRIu32 "\n",
                i, ret, (unsigned int)action.kind, action.start_us,
                action.end_us, action.skipped_events,
                action.already_scheduled, sim->connections[i].valid,
                sim->connections[i].timing_a.timing_fresh,
                sim->connections[i].timing_b.timing_fresh,
                sim->connections[i].timing_a.fallback_required,
                sim->connections[i].timing_b.fallback_required,
                sim->connections[i].completed_repairs);
    }
}

static int write_trace(const struct runner *runner, int status)
{
    struct mesh_sim_fault_stats stats = {0};
    uint64_t retries;
    uint64_t retry_bound;
    FILE *file;

    if (runner->config.trace_path == NULL) {
        return 0;
    }
    file = fopen(runner->config.trace_path, "w");
    if (file == NULL) {
        return -1;
    }
    (void)mesh_sim_get_fault_stats(runner->world, &stats);
    fprintf(file,
            "{\"type\":\"config\",\"scenario\":\"%s\","
            "\"seed\":%" PRIu32 ",\"fault_seed\":%" PRIu32
            ",\"packets\":%u,\"loss\":%u,\"ack_loss\":%u,"
            "\"duplicate\":%u,\"delay\":%u,\"max_delay_us\":%" PRIu32
            ",\"reset_enabled\":%s,\"reset_step\":%" PRIu32
            ",\"max_steps\":%" PRIu32 "}\n",
            scenario_name(runner->config.scenario), runner->config.seed,
            runner->config.faults.seed, runner->config.packets_per_node,
            runner->config.faults.frame_loss_permyriad,
            runner->config.faults.ack_loss_permyriad,
            runner->config.faults.duplicate_permyriad,
            runner->config.faults.delay_permyriad,
            runner->config.faults.max_extra_delay_us,
            runner->config.reset_enabled ? "true" : "false",
            runner->config.reset_step, runner->config.max_steps);
    for (size_t i = 0u; i < runner->world->transition_count; i++) {
        const struct mesh_sim_transition *entry = &runner->world->transitions[i];

        fprintf(file,
                "{\"type\":\"transition\",\"index\":%zu,"
                "\"time_us\":%" PRIu64 ",\"kind\":\"%s\","
                "\"kind_id\":%u,\"node\":\"%" PRIx64 "\","
                "\"peer\":\"%" PRIx64 "\",\"msg_type\":%u,"
                "\"packet_src\":\"%" PRIx64 "\","
                "\"packet_dst\":\"%" PRIx64 "\","
                "\"packet_session\":%" PRIu32 ",\"packet_seq\":%u,"
                "\"detail\":%" PRIu32 ",\"fault_ordinal\":%" PRIu64
                "}\n",
                i, entry->time_us, transition_name(entry->kind),
                (unsigned int)entry->kind, entry->node_id, entry->peer_id,
                entry->msg_type, entry->packet_src_id,
                entry->packet_dst_id, entry->packet_session_id,
                entry->packet_seq, entry->detail,
                entry->fault_decision_ordinal);
    }
    for (size_t i = 0u;
         i < runner->world->roles[runner->gateway].delivery_count; i++) {
        const struct mesh_sim_delivery *delivery =
            &runner->world->roles[runner->gateway].deliveries[i];

        fprintf(file,
                "{\"type\":\"delivery\",\"time_us\":%" PRIu64
                ",\"source\":\"%" PRIx64 "\",\"session\":%" PRIu32
                ",\"seq\":%u,\"previous_hop\":\"%" PRIx64 "\"}\n",
                delivery->delivered_at_us, delivery->packet.src_id,
                delivery->packet.session_id, delivery->packet.seq,
                delivery->previous_hop_id);
    }
    for (size_t i = 0u; i < runner->world->role_count; i++) {
        const struct mesh_sim_role_instance *node = &runner->world->roles[i];
        size_t runtime_work = 0u;

        for (size_t j = 0u; j < MESH_RUNTIME_WORK_CAPACITY; j++) {
            runtime_work += node->runtime.work[j].valid ? 1u : 0u;
        }
        fprintf(file,
                "{\"type\":\"node-final\",\"index\":%zu,"
                "\"id\":\"%" PRIx64 "\",\"role\":%u,\"epoch\":%" PRIu32
                ",\"queue\":%zu,\"queue_capacity\":%zu,"
                "\"pending_state\":%u,\"pending_seq\":%u,"
                "\"runtime_work\":%zu,\"transit_reserved\":%s,"
                "\"radio_state\":%u,\"workers_active\":%" PRIu32
                ",\"workers_started\":%" PRIu32
                ",\"workers_completed\":%" PRIu32
                ",\"deliveries\":%zu,\"route_requests\":%" PRIu32
                ",\"decoded\":%" PRIu32 ",\"partial\":%" PRIu32
                ",\"collisions\":%" PRIu32 "}\n",
                i, node->id, (unsigned int)node->role, node->work_epoch,
                node->tx_queue_count, node->tx_queue_capacity,
                (unsigned int)node->relay.pending.state,
                node->relay.pending.packet.seq, runtime_work,
                node->runtime.transit_reserved ? "true" : "false",
                (unsigned int)node->radio_state,
                node->watchdog.workers_active,
                node->watchdog.workers_started,
                node->watchdog.workers_completed,
                node->delivery_count,
                node->route_discovery_requests, node->decoded_frames,
                node->partial_frames, node->collision_frames);
    }
    retries = mesh_sim_count_transitions(
        runner->world, MESH_SIM_TRANSITION_RETRY_READY, 0u);
    retry_bound = runner->expected_deliveries *
                  transaction_retry_bound(runner);
    fprintf(file,
            "{\"type\":\"summary\",\"status\":%d,"
            "\"now_us\":%" PRIu64 ",\"steps\":%" PRIu32
            ",\"expected\":%zu,\"deliveries\":%zu,"
            "\"first_delivery_us\":%" PRIu64
            ",\"last_delivery_us\":%" PRIu64
            ",\"fault_decisions\":%" PRIu64 ",\"delays\":%" PRIu64
            ",\"frame_losses\":%" PRIu64 ",\"ack_losses\":%" PRIu64
            ",\"duplicates\":%" PRIu64 ",\"retries\":%" PRIu64
            ",\"retry_bound\":%" PRIu64
            ",\"rf_transmissions\":%" PRIu64
            ",\"direct_gateway_turns\":%" PRIu64
            ",\"direct_gateway_ack_turns\":%" PRIu64
            ",\"max_direct_gateway_ack_turnaround_us\":%" PRIu64
            ",\"max_empty_channel9_events\":%" PRIu32
            ",\"post_completion_steps\":%" PRIu32
            ",\"post_completion_empty_channel9_events\":%" PRIu32
            ",\"operation_liveness_bound_us\":%" PRIu64
            ",\"last_error\":%d"
            ",\"connection_event_dropped\":%" PRIu64
            ",\"rx_window_dropped\":%" PRIu64
            ",\"transmission_dropped\":%" PRIu64
            ",\"reception_dropped\":%" PRIu64
            ",\"trace_dropped\":%" PRIu64
            "}\n",
            status, runner->world->now_us, runner->steps_run,
            runner->expected_deliveries,
            runner->world->roles[runner->gateway].delivery_count,
            runner->first_delivery_us, runner->last_delivery_us,
            stats.receiver_decisions, stats.delay_injections,
            stats.frame_losses, stats.ack_losses, stats.duplicates,
            retries, retry_bound,
            runner->world->transmission_total_count,
            runner->direct_gateway_turns,
            runner->direct_gateway_ack_turns,
            runner->max_direct_gateway_ack_turnaround_us,
            runner->max_consecutive_empty_channel9_events,
            runner->post_completion_steps,
            runner->post_completion_empty_channel9_events,
            operation_liveness_bound_us(runner),
            runner->world->last_error,
            runner->world->connection_event_dropped_count,
            runner->world->rx_window_dropped_count,
            runner->world->transmission_dropped_count,
            runner->world->reception_dropped_count,
            mesh_sim_trace_dropped_count(runner->world));
    return fclose(file);
}

int main(int argc, char **argv)
{
    struct runner_config config;
    struct runner runner;
    struct mesh_sim_fault_stats stats = {0};
    int parsed = parse_args(argc, argv, &config);
    int status;

    if (parsed != 0) {
        if (parsed < 0) {
            usage(argv[0]);
            return 2;
        }
        return 0;
    }
    mesh_sim_init(&world, config.seed);
    runner = (struct runner) {.world = &world, .config = config};
    status = config.scenario == SCENARIO_QUEUE_FULL ?
             run_queue_full(&runner) : run_delivery(&runner);
    (void)mesh_sim_get_fault_stats(&world, &stats);
    if (write_trace(&runner, status) != 0 && status == MESH_SIM_OK) {
        status = MESH_SIM_ERR_PROTOCOL;
    }
    if (status != MESH_SIM_OK) {
        fprintf(stderr,
                "FAIL scenario=%s seed=0x%08" PRIx32
                " fault_seed=0x%08" PRIx32 " status=%d last_error=%d\n",
                scenario_name(config.scenario), config.seed,
                config.faults.seed, status, world.last_error);
        fputs("replay: ", stderr);
        print_replay(&config, stderr);
        dump_nodes(&world);
        dump_tail(&world, 32u);
        return 1;
    }
    if (!config.quiet) {
        printf("PASS scenario=%s seed=0x%08" PRIx32
               " deliveries=%zu steps=%" PRIu32 " settled_us=%" PRIu64
               " first_delivery_us=%" PRIu64 " last_delivery_us=%" PRIu64
               " direct_turns=%" PRIu64 " direct_acks=%" PRIu64
               " max_ack_turnaround_us=%" PRIu64
               " faults={decisions=%" PRIu64 ",delay=%" PRIu64
               ",loss=%" PRIu64 ",ack_loss=%" PRIu64
               ",duplicate=%" PRIu64 "}\n",
               scenario_name(config.scenario), config.seed,
               world.roles[runner.gateway].delivery_count, runner.steps_run,
               world.now_us, runner.first_delivery_us, runner.last_delivery_us,
               runner.direct_gateway_turns,
               runner.direct_gateway_ack_turns,
               runner.max_direct_gateway_ack_turnaround_us,
               stats.receiver_decisions, stats.delay_injections,
               stats.frame_losses, stats.ack_losses, stats.duplicates);
    }
    return 0;
}
