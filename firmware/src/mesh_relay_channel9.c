#include "mesh_relay.h"

#include "mesh_relay_channel9.h"

#include "mesh.h"

#include <string.h>

static bool id_is_unicast(uint64_t id)
{
    return id != MESH_BROADCAST_ID;
}

int relay_channel9_timing_index(const struct mesh_relay *relay,
                                uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == next_hop_id) {
            return (int)i;
        }
    }
    return -1;
}

static int free_event_timing_index(const struct mesh_relay *relay)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (!relay->event_timings[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

static enum mesh_relay_channel9_direction channel9_peer_direction(
    const struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    const struct route_candidate *upstream;
    bool matches_upstream = false;
    bool matches_downstream = false;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN;
    }

    upstream = route_selected(&relay->upstream);
    matches_upstream = upstream != NULL && upstream->next_hop_id == next_hop_id;
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            matches_downstream = true;
            break;
        }
    }

    if (matches_upstream && matches_downstream) {
        return MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS;
    }
    if (matches_upstream) {
        return MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM;
    }
    if (matches_downstream) {
        return MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM;
    }
    return MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN;
}

static bool channel9_direction_valid(enum mesh_relay_channel9_direction direction)
{
    return direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM ||
           direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM;
}

static enum mesh_relay_channel9_direction channel9_entry_direction(
    const struct mesh_relay *relay,
    const struct mesh_relay_event_timing_entry *entry)
{
    enum mesh_relay_channel9_direction direction;

    if (entry == NULL || !entry->valid) {
        return MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN;
    }
    direction = (enum mesh_relay_channel9_direction)entry->direction;
    if (channel9_direction_valid(direction)) {
        return direction;
    }
    return channel9_peer_direction(relay, entry->next_hop_id);
}

static void channel9_guard_reset(struct mesh_relay_channel9_guard_status *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

bool relay_channel9_plan_misses_event(enum mesh_event_plan_action action)
{
    return mesh_event_plan_is_policy_deferral(action);
}

static uint32_t greatest_common_divisor(uint32_t a, uint32_t b)
{
    while (b != 0u) {
        uint32_t remainder = a % b;

        a = b;
        b = remainder;
    }
    return a;
}

static bool channel9_timing_shape_valid(const struct mesh_event_timing *timing)
{
    uint32_t reserved_ms;

    if (timing == NULL || timing->event_interval_ms == 0u ||
        timing->event_window_ms == 0u || timing->guard_ms == 0u) {
        return false;
    }
    reserved_ms = (uint32_t)timing->event_window_ms +
                  (2u * (uint32_t)timing->guard_ms);
    return reserved_ms < timing->event_interval_ms;
}

static bool channel9_timings_conflict(const struct mesh_event_timing *a,
                                      const struct mesh_event_timing *b)
{
    uint32_t cycle_ms;
    uint32_t a_reserved_ms;
    uint32_t b_reserved_ms;
    uint32_t a_start_ms;
    uint32_t b_start_ms;
    uint32_t delta_ms;

    if (!channel9_timing_shape_valid(a) || !channel9_timing_shape_valid(b)) {
        return true;
    }

    cycle_ms = greatest_common_divisor(a->event_interval_ms,
                                       b->event_interval_ms);
    a_reserved_ms = (uint32_t)a->event_window_ms +
                    (2u * (uint32_t)a->guard_ms);
    b_reserved_ms = (uint32_t)b->event_window_ms +
                    (2u * (uint32_t)b->guard_ms);
    if (a_reserved_ms + b_reserved_ms > cycle_ms) {
        return true;
    }

    a_start_ms = a->next_event_time_ms - a->guard_ms;
    b_start_ms = b->next_event_time_ms - b->guard_ms;
    delta_ms = (b_start_ms - a_start_ms) % cycle_ms;
    return delta_ms < a_reserved_ms ||
           (delta_ms != 0u && cycle_ms - delta_ms < b_reserved_ms);
}

int mesh_relay_set_channel9_timing(struct mesh_relay *relay,
                                   uint64_t next_hop_id,
                                   const struct mesh_event_timing *timing)
{
    enum mesh_relay_channel9_direction direction;
    int index;

    if (relay == NULL || timing == NULL || !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        timing->mesh_channel != MESH_EVENT_CHANNEL ||
        !channel9_timing_shape_valid(timing)) {
        return PROTO_ERR_ARG;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    direction = index >= 0 ?
        channel9_entry_direction(relay, &relay->event_timings[index]) :
        channel9_peer_direction(relay, next_hop_id);
    if (index < 0) {
        index = free_event_timing_index(relay);
    }
    if (index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    relay->event_timings[index].next_hop_id = next_hop_id;
    relay->event_timings[index].timing = *timing;
    relay->event_timings[index].direction = (uint8_t)direction;
    relay->event_timings[index].valid = true;
    route_set_channel9_timing_valid(&relay->upstream,
                                    next_hop_id,
                                    relay->gateway_id,
                                    true,
                                    timing->next_event_time_ms);
    return PROTO_OK;
}

int mesh_relay_check_channel9_timing_guarded_direction(
    struct mesh_relay *relay,
    uint64_t next_hop_id,
    const struct mesh_event_timing *timing,
    enum mesh_relay_channel9_direction direction,
    uint8_t max_active_peers,
    struct mesh_relay_channel9_guard_status *status)
{
    uint8_t active_peer_count = 0u;
    int index;

    channel9_guard_reset(status);
    if (relay == NULL || timing == NULL || !id_is_unicast(next_hop_id) ||
        max_active_peers == 0u) {
        return PROTO_ERR_ARG;
    }
    index = relay_channel9_timing_index(relay, next_hop_id);
    if (status != NULL) {
        status->direction = direction;
    }
    if (!channel9_direction_valid(direction)) {
        if (status != NULL) {
            status->reason = MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER;
        }
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];
        enum mesh_relay_channel9_direction entry_direction;

        if (!entry->valid) {
            continue;
        }

        active_peer_count++;
        entry_direction = channel9_entry_direction(relay, entry);
        if (!channel9_direction_valid(entry_direction)) {
            if (status != NULL) {
                status->reason = MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_ACTIVE_PEER;
                status->conflict_peer_id = entry->next_hop_id;
                status->conflict_direction = entry_direction;
                status->active_peer_count = active_peer_count;
            }
            return PROTO_ERR_MALFORMED;
        }
    }

    if (status != NULL) {
        status->active_peer_count = active_peer_count;
    }
    if (index < 0 && active_peer_count >= max_active_peers) {
        if (status != NULL) {
            status->reason = MESH_RELAY_CHANNEL9_GUARD_TOO_MANY_PEERS;
        }
        return PROTO_ERR_NO_SPACE;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];
        enum mesh_relay_channel9_direction entry_direction;

        if (!entry->valid || entry->next_hop_id == next_hop_id) {
            continue;
        }

        entry_direction = channel9_entry_direction(relay, entry);
        if (entry_direction == direction) {
            if (status != NULL) {
                status->reason = MESH_RELAY_CHANNEL9_GUARD_DIRECTION_BUSY;
                status->conflict_peer_id = entry->next_hop_id;
                status->conflict_direction = entry_direction;
            }
            return PROTO_ERR_BUSY;
        }
        if (channel9_timings_conflict(timing, &entry->timing)) {
            if (status != NULL) {
                status->reason = MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT;
                status->conflict_peer_id = entry->next_hop_id;
                status->conflict_direction = entry_direction;
            }
            return PROTO_ERR_BUSY;
        }
    }

    if (status != NULL) {
        status->reason = index >= 0 ? MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER :
                                     MESH_RELAY_CHANNEL9_GUARD_OK;
    }
    return PROTO_OK;
}

int mesh_relay_set_channel9_timing_guarded_direction(
    struct mesh_relay *relay,
    uint64_t next_hop_id,
    const struct mesh_event_timing *timing,
    enum mesh_relay_channel9_direction direction,
    uint8_t max_active_peers,
    struct mesh_relay_channel9_guard_status *status)
{
    int index;
    int ret = mesh_relay_check_channel9_timing_guarded_direction(
        relay,
        next_hop_id,
        timing,
        direction,
        max_active_peers,
        status);

    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_set_channel9_timing(relay, next_hop_id, timing);
    if (ret == PROTO_OK) {
        index = relay_channel9_timing_index(relay, next_hop_id);
        relay->event_timings[index].direction = (uint8_t)direction;
    }
    return ret;
}

int mesh_relay_set_channel9_timing_guarded(struct mesh_relay *relay,
                                           uint64_t next_hop_id,
                                           const struct mesh_event_timing *timing,
                                           uint8_t max_active_peers,
                                           struct mesh_relay_channel9_guard_status *status)
{
    enum mesh_relay_channel9_direction direction =
        MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN;
    int index;

    if (relay != NULL && id_is_unicast(next_hop_id)) {
        index = relay_channel9_timing_index(relay, next_hop_id);
        direction = index >= 0 ?
            channel9_entry_direction(relay, &relay->event_timings[index]) :
            channel9_peer_direction(relay, next_hop_id);
    }
    return mesh_relay_set_channel9_timing_guarded_direction(relay,
                                                            next_hop_id,
                                                            timing,
                                                            direction,
                                                            max_active_peers,
                                                            status);
}

void mesh_relay_clear_channel9_timing(struct mesh_relay *relay,
                                      uint64_t next_hop_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index >= 0) {
        memset(&relay->event_timings[index], 0,
               sizeof(relay->event_timings[index]));
        route_set_channel9_timing_valid(&relay->upstream,
                                        next_hop_id,
                                        relay->gateway_id,
                                        false,
                                        0u);
    }
}

void mesh_relay_abandon_transit_reservations(struct mesh_relay *relay)
{
    const struct route_candidate *upstream;
    uint64_t upstream_peer_id = 0u;

    if (relay == NULL) {
        return;
    }

    upstream = route_selected(&relay->upstream);
    if (upstream != NULL) {
        upstream_peer_id = upstream->next_hop_id;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *timing = &relay->event_timings[i];
        bool downstream_peer = false;

        if (!timing->valid || timing->next_hop_id == upstream_peer_id) {
            continue;
        }
        for (size_t j = 0u; j < mesh_relay_downlink_capacity(relay); j++) {
            const struct mesh_downlink_entry *downlink =
                mesh_relay_downlink_at(relay, j);

            if (downlink->valid &&
                downlink->next_hop_id == timing->next_hop_id) {
                downstream_peer = true;
                break;
            }
        }
        if (downstream_peer) {
            memset(timing, 0, sizeof(*timing));
        }
    }
}
