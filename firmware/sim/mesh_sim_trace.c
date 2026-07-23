#include "mesh_sim_internal.h"

#include <limits.h>
#include <string.h>

static bool node_id_known(const struct mesh_sim_world *world,
                          uint64_t node_id,
                          size_t *node_index)
{
    if (node_id == 0u) {
        return true;
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].id == node_id) {
            if (node_index != NULL) {
                *node_index = i;
            }
            return true;
        }
    }
    return false;
}

static uint64_t trace_total(const struct mesh_sim_world *world,
                            enum mesh_sim_transition_kind kind,
                            uint64_t node_id)
{
    size_t node_index = 0u;

    if (node_id == 0u) {
        return world->transition_kind_counts[kind];
    }
    if (!node_id_known(world, node_id, &node_index)) {
        return 0u;
    }
    return world->transition_role_counts[node_index][kind];
}

static uint64_t retained_matches(const struct mesh_sim_world *world,
                                 enum mesh_sim_transition_kind kind,
                                 uint64_t node_id)
{
    uint64_t matches = 0u;

    for (size_t i = 0u; i < world->transition_count; i++) {
        const struct mesh_sim_transition *transition = &world->transitions[i];

        if (transition->kind == kind &&
            (node_id == 0u || transition->node_id == node_id)) {
            matches++;
        }
    }
    return matches;
}

static int trace_add(struct mesh_sim_world *world,
                     uint64_t time_us,
                     uint64_t node_id,
                     uint64_t peer_id,
                     enum mesh_sim_transition_kind kind,
                     const struct proto_packet *packet,
                     uint8_t msg_type,
                     uint32_t detail)
{
    struct mesh_sim_transition *transition;
    size_t node_index = 0u;

    if (world == NULL || kind >= MESH_SIM_TRANSITION_COUNT) {
        return MESH_SIM_ERR_ARG;
    }
    world->transition_trace_total_count++;
    world->transition_kind_counts[kind]++;
    if (node_id_known(world, node_id, &node_index) && node_id != 0u) {
        world->transition_role_counts[node_index][kind]++;
    }
    if (world->transition_count == MESH_SIM_MAX_TRANSITIONS) {
        memmove(&world->transitions[0],
                &world->transitions[1],
                (MESH_SIM_MAX_TRANSITIONS - 1u) * sizeof(world->transitions[0]));
        world->transition_count--;
        world->transition_trace_dropped_count++;
    }
    transition = &world->transitions[world->transition_count++];
    *transition = (struct mesh_sim_transition) {
        .time_us = time_us,
        .node_id = node_id,
        .peer_id = peer_id,
        .packet_src_id = packet == NULL ? 0u : packet->src_id,
        .packet_dst_id = packet == NULL ? 0u : packet->dst_id,
        .packet_session_id = packet == NULL ? 0u : packet->session_id,
        .kind = kind,
        .detail = detail,
        .packet_seq = packet == NULL ? 0u : packet->seq,
        .msg_type = packet == NULL ? msg_type : packet->msg_type,
    };
    return MESH_SIM_OK;
}

int mesh_sim_trace_add(struct mesh_sim_world *world,
                       uint64_t time_us,
                       uint64_t node_id,
                       uint64_t peer_id,
                       enum mesh_sim_transition_kind kind,
                       uint8_t msg_type,
                       uint32_t detail)
{
    return trace_add(world, time_us, node_id, peer_id, kind, NULL, msg_type,
                     detail);
}

int mesh_sim_trace_add_packet(struct mesh_sim_world *world,
                              uint64_t time_us,
                              uint64_t node_id,
                              uint64_t peer_id,
                              enum mesh_sim_transition_kind kind,
                              const struct proto_packet *packet,
                              uint32_t detail)
{
    if (packet == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    return trace_add(world, time_us, node_id, peer_id, kind, packet, 0u,
                     detail);
}

size_t mesh_sim_count_transitions(const struct mesh_sim_world *world,
                                  enum mesh_sim_transition_kind kind,
                                  uint64_t node_id)
{
    uint64_t count;

    if (world == NULL || kind >= MESH_SIM_TRANSITION_COUNT) {
        return 0u;
    }
    count = trace_total(world, kind, node_id);
    return count > SIZE_MAX ? SIZE_MAX : (size_t)count;
}

bool mesh_sim_trace_is_truncated(const struct mesh_sim_world *world)
{
    return world != NULL && world->transition_trace_dropped_count > 0u;
}

uint64_t mesh_sim_trace_total_count(const struct mesh_sim_world *world)
{
    return world == NULL ? 0u : world->transition_trace_total_count;
}

uint64_t mesh_sim_trace_dropped_count(const struct mesh_sim_world *world)
{
    return world == NULL ? 0u : world->transition_trace_dropped_count;
}

enum mesh_sim_snapshot_status mesh_sim_find_transition_snapshot(
    const struct mesh_sim_world *world,
    enum mesh_sim_transition_kind kind,
    uint64_t node_id,
    uint64_t occurrence,
    struct mesh_sim_transition *snapshot)
{
    uint64_t total;
    uint64_t retained;
    uint64_t dropped;
    uint64_t retained_occurrence;
    uint64_t found = 0u;

    if (world == NULL || snapshot == NULL || kind >= MESH_SIM_TRANSITION_COUNT) {
        return MESH_SIM_SNAPSHOT_ERR_ARG;
    }
    total = trace_total(world, kind, node_id);
    if (occurrence >= total) {
        return MESH_SIM_SNAPSHOT_NOT_FOUND;
    }
    retained = retained_matches(world, kind, node_id);
    dropped = total - retained;
    if (occurrence < dropped) {
        return MESH_SIM_SNAPSHOT_TRUNCATED;
    }
    retained_occurrence = occurrence - dropped;
    for (size_t i = 0u; i < world->transition_count; i++) {
        const struct mesh_sim_transition *transition = &world->transitions[i];

        if (transition->kind != kind ||
            (node_id != 0u && transition->node_id != node_id)) {
            continue;
        }
        if (found++ == retained_occurrence) {
            *snapshot = *transition;
            return MESH_SIM_SNAPSHOT_OK;
        }
    }
    return MESH_SIM_SNAPSHOT_TRUNCATED;
}

const struct mesh_sim_transition *mesh_sim_find_transition(
    const struct mesh_sim_world *world,
    enum mesh_sim_transition_kind kind,
    uint64_t node_id,
    size_t occurrence)
{
    uint64_t total;
    uint64_t retained;
    uint64_t dropped;
    uint64_t retained_occurrence;
    uint64_t found = 0u;

    if (world == NULL || kind >= MESH_SIM_TRANSITION_COUNT) {
        return NULL;
    }
    total = trace_total(world, kind, node_id);
    if ((uint64_t)occurrence >= total) {
        return NULL;
    }
    retained = retained_matches(world, kind, node_id);
    dropped = total - retained;
    if ((uint64_t)occurrence < dropped) {
        return NULL;
    }
    retained_occurrence = (uint64_t)occurrence - dropped;
    for (size_t i = 0u; i < world->transition_count; i++) {
        const struct mesh_sim_transition *transition = &world->transitions[i];

        if (transition->kind == kind &&
            (node_id == 0u || transition->node_id == node_id) &&
            found++ == retained_occurrence) {
            return transition;
        }
    }
    return NULL;
}

static bool event_references(const struct mesh_sim_world *world,
                             enum mesh_sim_event_type type,
                             uint16_t object_index)
{
    for (size_t i = 0u; i < world->event_count; i++) {
        const struct mesh_sim_event *event = &world->events[i];

        if (event->pending && event->object_index == object_index &&
            event->type == type) {
            return true;
        }
    }
    return false;
}

static bool pending_transmission_may_reach_window(
    const struct mesh_sim_world *world,
    uint16_t window_index)
{
    const struct mesh_sim_rx_window *window = &world->rx_windows[window_index];

    for (size_t i = 0u; i < world->event_count; i++) {
        const struct mesh_sim_event *event = &world->events[i];
        const struct mesh_sim_transmission *tx;
        uint64_t delay_us;
        uint64_t arrival_start_us;
        uint64_t arrival_end_us;

        if (!event->pending || event->type != SIM_EVENT_TX_EVALUATE ||
            event->object_index >= world->transmission_count) {
            continue;
        }
        tx = &world->transmissions[event->object_index];
        if (!tx->valid || tx->node_index == window->node_index ||
            tx->channel != window->channel ||
            (tx->phy != window->phy && !window->observe_phy_activity) ||
            !world->reachable[tx->node_index][window->node_index]) {
            continue;
        }
        delay_us = world->propagation_us[tx->node_index][window->node_index] +
                   tx->fault_extra_delay_us[window->node_index];
        if (tx->start_us > UINT64_MAX - delay_us ||
            tx->end_us > UINT64_MAX - delay_us) {
            return true;
        }
        arrival_start_us = tx->start_us + delay_us;
        arrival_end_us = tx->end_us + delay_us;
        if (arrival_start_us < window->end_us &&
            window->start_us < arrival_end_us) {
            return true;
        }
    }
    return false;
}

static bool transmission_pending(const struct mesh_sim_world *world,
                                 uint16_t transmission_index)
{
    return event_references(world, SIM_EVENT_TX_START, transmission_index) ||
           event_references(world, SIM_EVENT_TX_END, transmission_index) ||
           event_references(world, SIM_EVENT_TX_EVALUATE, transmission_index);
}

static bool rx_window_pending(const struct mesh_sim_world *world,
                              uint16_t window_index)
{
    return event_references(world, SIM_EVENT_RX_START, window_index) ||
           event_references(world, SIM_EVENT_RX_END, window_index) ||
           pending_transmission_may_reach_window(world, window_index);
}

static void shift_event_indices(struct mesh_sim_world *world,
                                enum mesh_sim_event_type type_a,
                                enum mesh_sim_event_type type_b,
                                enum mesh_sim_event_type type_c)
{
    for (size_t i = 0u; i < world->event_count; i++) {
        struct mesh_sim_event *event = &world->events[i];

        if ((event->type == type_a || event->type == type_b ||
             event->type == type_c) && event->object_index > 0u) {
            event->object_index--;
        }
    }
}

int mesh_sim_telemetry_reserve_connection_event(struct mesh_sim_world *world,
                                                uint16_t *event_index)
{
    if (world->connection_event_count == MESH_SIM_MAX_CONNECTION_EVENTS) {
        for (size_t i = 0u; i < world->transmission_count; i++) {
            if (world->transmissions[i].connection_event_index == 0u &&
                transmission_pending(world, (uint16_t)i)) {
                return MESH_SIM_ERR_CAPACITY;
            }
        }
        for (size_t i = 0u; i < world->rx_window_count; i++) {
            if (world->rx_windows[i].connection_event_index == 0u &&
                rx_window_pending(world, (uint16_t)i)) {
                return MESH_SIM_ERR_CAPACITY;
            }
        }
        if (event_references(world, SIM_EVENT_CONNECTION_START, 0u) ||
            event_references(world, SIM_EVENT_CONNECTION_END, 0u)) {
            return MESH_SIM_ERR_CAPACITY;
        }
        memmove(&world->connection_events[0],
                &world->connection_events[1],
                (MESH_SIM_MAX_CONNECTION_EVENTS - 1u) *
                    sizeof(world->connection_events[0]));
        world->connection_event_count--;
        world->connection_event_dropped_count++;
        shift_event_indices(world,
                            SIM_EVENT_CONNECTION_START,
                            SIM_EVENT_CONNECTION_END,
                            SIM_EVENT_CONNECTION_START);
        for (size_t i = 0u; i < world->transmission_count; i++) {
            if (world->transmissions[i].connection_event_index == 0u) {
                world->transmissions[i].connection_event_index = UINT16_MAX;
            } else if (world->transmissions[i].connection_event_index !=
                       UINT16_MAX) {
                world->transmissions[i].connection_event_index--;
            }
        }
        for (size_t i = 0u; i < world->rx_window_count; i++) {
            if (world->rx_windows[i].connection_event_index == 0u) {
                world->rx_windows[i].connection_event_index = UINT16_MAX;
            } else if (world->rx_windows[i].connection_event_index !=
                       UINT16_MAX) {
                world->rx_windows[i].connection_event_index--;
            }
        }
    }
    *event_index = (uint16_t)world->connection_event_count++;
    world->connection_event_total_count++;
    return MESH_SIM_OK;
}

int mesh_sim_telemetry_reserve_rx_window(struct mesh_sim_world *world,
                                         uint16_t *window_index)
{
    if (world->rx_window_count == MESH_SIM_MAX_RX_WINDOWS) {
        if (rx_window_pending(world, 0u)) {
            return MESH_SIM_ERR_CAPACITY;
        }
        memmove(&world->rx_windows[0],
                &world->rx_windows[1],
                (MESH_SIM_MAX_RX_WINDOWS - 1u) * sizeof(world->rx_windows[0]));
        world->rx_window_count--;
        world->rx_window_dropped_count++;
        shift_event_indices(world,
                            SIM_EVENT_RX_START,
                            SIM_EVENT_RX_END,
                            SIM_EVENT_RX_START);
    }
    *window_index = (uint16_t)world->rx_window_count++;
    world->rx_window_total_count++;
    return MESH_SIM_OK;
}

int mesh_sim_telemetry_reserve_transmission(struct mesh_sim_world *world,
                                            uint16_t *transmission_index)
{
    if (world->transmission_count == MESH_SIM_MAX_TRANSMISSIONS) {
        if (transmission_pending(world, 0u)) {
            return MESH_SIM_ERR_CAPACITY;
        }
        memmove(&world->transmissions[0],
                &world->transmissions[1],
                (MESH_SIM_MAX_TRANSMISSIONS - 1u) *
                    sizeof(world->transmissions[0]));
        world->transmission_count--;
        world->transmission_dropped_count++;
        shift_event_indices(world,
                            SIM_EVENT_TX_START,
                            SIM_EVENT_TX_END,
                            SIM_EVENT_TX_EVALUATE);
    }
    *transmission_index = (uint16_t)world->transmission_count++;
    world->transmission_total_count++;
    return MESH_SIM_OK;
}

int mesh_sim_telemetry_reserve_reception(struct mesh_sim_world *world,
                                         uint16_t *reception_index)
{
    if (world->reception_count == MESH_SIM_MAX_RECEPTIONS) {
        memmove(&world->receptions[0],
                &world->receptions[1],
                (MESH_SIM_MAX_RECEPTIONS - 1u) * sizeof(world->receptions[0]));
        world->reception_count--;
        world->reception_dropped_count++;
    }
    *reception_index = (uint16_t)world->reception_count++;
    world->reception_total_count++;
    return MESH_SIM_OK;
}

static enum mesh_sim_snapshot_status telemetry_index(
    const struct mesh_sim_world *world,
    enum mesh_sim_telemetry_kind kind,
    uint64_t occurrence,
    size_t *index)
{
    struct mesh_sim_telemetry_snapshot snapshot;

    if (mesh_sim_telemetry_snapshot(world, kind, &snapshot) !=
        MESH_SIM_SNAPSHOT_OK || index == NULL) {
        return MESH_SIM_SNAPSHOT_ERR_ARG;
    }
    if (occurrence >= snapshot.total_count) {
        return MESH_SIM_SNAPSHOT_NOT_FOUND;
    }
    if (occurrence < snapshot.dropped_count) {
        return MESH_SIM_SNAPSHOT_TRUNCATED;
    }
    *index = (size_t)(occurrence - snapshot.dropped_count);
    return *index < snapshot.retained_count ? MESH_SIM_SNAPSHOT_OK :
                                             MESH_SIM_SNAPSHOT_TRUNCATED;
}

bool mesh_sim_telemetry_is_truncated(const struct mesh_sim_world *world,
                                     enum mesh_sim_telemetry_kind kind)
{
    struct mesh_sim_telemetry_snapshot snapshot;

    return mesh_sim_telemetry_snapshot(world, kind, &snapshot) ==
               MESH_SIM_SNAPSHOT_OK && snapshot.dropped_count > 0u;
}

enum mesh_sim_snapshot_status mesh_sim_telemetry_snapshot(
    const struct mesh_sim_world *world,
    enum mesh_sim_telemetry_kind kind,
    struct mesh_sim_telemetry_snapshot *snapshot)
{
    if (world == NULL || snapshot == NULL || kind >= MESH_SIM_TELEMETRY_COUNT) {
        return MESH_SIM_SNAPSHOT_ERR_ARG;
    }
    switch (kind) {
    case MESH_SIM_TELEMETRY_CONNECTION_EVENT:
        snapshot->total_count = world->connection_event_total_count;
        snapshot->dropped_count = world->connection_event_dropped_count;
        snapshot->retained_count = world->connection_event_count;
        break;
    case MESH_SIM_TELEMETRY_RX_WINDOW:
        snapshot->total_count = world->rx_window_total_count;
        snapshot->dropped_count = world->rx_window_dropped_count;
        snapshot->retained_count = world->rx_window_count;
        break;
    case MESH_SIM_TELEMETRY_TRANSMISSION:
        snapshot->total_count = world->transmission_total_count;
        snapshot->dropped_count = world->transmission_dropped_count;
        snapshot->retained_count = world->transmission_count;
        break;
    case MESH_SIM_TELEMETRY_RECEPTION:
        snapshot->total_count = world->reception_total_count;
        snapshot->dropped_count = world->reception_dropped_count;
        snapshot->retained_count = world->reception_count;
        break;
    default:
        return MESH_SIM_SNAPSHOT_ERR_ARG;
    }
    return MESH_SIM_SNAPSHOT_OK;
}

enum mesh_sim_snapshot_status mesh_sim_connection_event_snapshot(
    const struct mesh_sim_world *world,
    uint64_t occurrence,
    struct mesh_sim_connection_event *snapshot)
{
    size_t index;
    enum mesh_sim_snapshot_status status = telemetry_index(
        world, MESH_SIM_TELEMETRY_CONNECTION_EVENT, occurrence, &index);

    if (status == MESH_SIM_SNAPSHOT_OK && snapshot != NULL) {
        *snapshot = world->connection_events[index];
    }
    return snapshot == NULL ? MESH_SIM_SNAPSHOT_ERR_ARG : status;
}

enum mesh_sim_snapshot_status mesh_sim_rx_window_snapshot(
    const struct mesh_sim_world *world,
    uint64_t occurrence,
    struct mesh_sim_rx_window *snapshot)
{
    size_t index;
    enum mesh_sim_snapshot_status status = telemetry_index(
        world, MESH_SIM_TELEMETRY_RX_WINDOW, occurrence, &index);

    if (status == MESH_SIM_SNAPSHOT_OK && snapshot != NULL) {
        *snapshot = world->rx_windows[index];
    }
    return snapshot == NULL ? MESH_SIM_SNAPSHOT_ERR_ARG : status;
}

enum mesh_sim_snapshot_status mesh_sim_transmission_snapshot(
    const struct mesh_sim_world *world,
    uint64_t occurrence,
    struct mesh_sim_transmission *snapshot)
{
    size_t index;
    enum mesh_sim_snapshot_status status = telemetry_index(
        world, MESH_SIM_TELEMETRY_TRANSMISSION, occurrence, &index);

    if (status == MESH_SIM_SNAPSHOT_OK && snapshot != NULL) {
        *snapshot = world->transmissions[index];
    }
    return snapshot == NULL ? MESH_SIM_SNAPSHOT_ERR_ARG : status;
}

enum mesh_sim_snapshot_status mesh_sim_reception_snapshot(
    const struct mesh_sim_world *world,
    uint64_t occurrence,
    struct mesh_sim_reception *snapshot)
{
    size_t index;
    enum mesh_sim_snapshot_status status = telemetry_index(
        world, MESH_SIM_TELEMETRY_RECEPTION, occurrence, &index);

    if (status == MESH_SIM_SNAPSHOT_OK && snapshot != NULL) {
        *snapshot = world->receptions[index];
    }
    return snapshot == NULL ? MESH_SIM_SNAPSHOT_ERR_ARG : status;
}
