#include "mesh_sim_internal.h"

#include <string.h>

int mesh_sim_scheduler_schedule_priority(struct mesh_sim_world *world,
                                         enum mesh_sim_event_type type,
                                         uint64_t at_us,
                                         uint16_t object_index,
                                         uint8_t priority,
                                         uint32_t token)
{
    struct mesh_sim_event *event;

    if (world == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    if (at_us < world->now_us) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    if (world->event_count >= MESH_SIM_MAX_EVENTS) {
        return MESH_SIM_ERR_CAPACITY;
    }
    event = &world->events[world->event_count++];
    *event = (struct mesh_sim_event) {
        .time_us = at_us,
        .sequence = world->next_sequence++,
        .object_index = object_index,
        .token = token,
        .type = (uint8_t)type,
        .priority = priority,
        .pending = true,
    };
    return MESH_SIM_OK;
}

int mesh_sim_scheduler_schedule(struct mesh_sim_world *world,
                                enum mesh_sim_event_type type,
                                uint64_t at_us,
                                uint16_t object_index)
{
    return mesh_sim_scheduler_schedule_priority(world,
                                                type,
                                                at_us,
                                                object_index,
                                                (uint8_t)type,
                                                0u);
}

int mesh_sim_scheduler_reschedule_watchdog(struct mesh_sim_world *world,
                                           uint8_t node_index,
                                           uint64_t deadline_us,
                                           uint32_t generation)
{
    for (size_t i = 0u; i < world->event_count; i++) {
        struct mesh_sim_event *event = &world->events[i];

        if (event->pending && event->type == SIM_EVENT_WATCHDOG_EXPIRE &&
            event->object_index == node_index) {
            event->time_us = deadline_us;
            event->token = generation;
            return MESH_SIM_OK;
        }
    }
    return mesh_sim_scheduler_schedule_priority(world,
                                                SIM_EVENT_WATCHDOG_EXPIRE,
                                                deadline_us,
                                                node_index,
                                                SIM_EVENT_WATCHDOG_EXPIRE,
                                                generation);
}

static bool event_uses_role(const struct mesh_sim_world *world,
                            const struct mesh_sim_event *event,
                            uint8_t node_index)
{
    const struct mesh_sim_connection *connection;

    switch ((enum mesh_sim_event_type)event->type) {
    case SIM_EVENT_TX_START:
    case SIM_EVENT_TX_END:
    case SIM_EVENT_TX_EVALUATE:
        return event->object_index < world->transmission_count &&
               world->transmissions[event->object_index].node_index == node_index;
    case SIM_EVENT_RX_START:
    case SIM_EVENT_RX_END:
        return event->object_index < world->rx_window_count &&
               world->rx_windows[event->object_index].node_index == node_index;
    case SIM_EVENT_CONNECTION_START:
    case SIM_EVENT_CONNECTION_END:
        if (event->object_index >= world->connection_event_count) {
            return false;
        }
        if (world->connection_events[event->object_index].connection_index >=
            world->connection_count) {
            return false;
        }
        connection = &world->connections[
            world->connection_events[event->object_index].connection_index];
        return connection->node_a == node_index || connection->node_b == node_index;
    case SIM_EVENT_CONNECTION_REPAIR_START:
    case SIM_EVENT_CONNECTION_REPAIR_END:
        if (event->object_index >= world->connection_count) {
            return false;
        }
        connection = &world->connections[event->object_index];
        return connection->node_a == node_index || connection->node_b == node_index;
    case SIM_EVENT_RELAY_TICK:
    case SIM_EVENT_LOW_DUTY_START:
    case SIM_EVENT_RUNTIME_BOUNDARY:
    case SIM_EVENT_RUNTIME_RADIO_RELEASE:
    case SIM_EVENT_ROUTE_DISCOVERY_RETRY:
        return event->object_index == node_index;
    case SIM_EVENT_WATCHDOG_EXPIRE:
    case SIM_EVENT_TRACE_MARKER:
        return false;
    }
    return false;
}

static void invalidate_event_object(struct mesh_sim_world *world,
                                    const struct mesh_sim_event *event)
{
    switch ((enum mesh_sim_event_type)event->type) {
    case SIM_EVENT_TX_START:
    case SIM_EVENT_TX_END:
    case SIM_EVENT_TX_EVALUATE:
        if (event->object_index < world->transmission_count) {
            world->transmissions[event->object_index].valid = false;
        }
        break;
    case SIM_EVENT_RX_START:
    case SIM_EVENT_RX_END:
        if (event->object_index < world->rx_window_count) {
            world->rx_windows[event->object_index].valid = false;
        }
        break;
    case SIM_EVENT_CONNECTION_START:
    case SIM_EVENT_CONNECTION_END:
        if (event->object_index < world->connection_event_count) {
            world->connection_events[event->object_index].valid = false;
        }
        break;
    case SIM_EVENT_CONNECTION_REPAIR_START:
    case SIM_EVENT_CONNECTION_REPAIR_END:
    case SIM_EVENT_RELAY_TICK:
    case SIM_EVENT_LOW_DUTY_START:
    case SIM_EVENT_RUNTIME_BOUNDARY:
    case SIM_EVENT_RUNTIME_RADIO_RELEASE:
    case SIM_EVENT_ROUTE_DISCOVERY_RETRY:
    case SIM_EVENT_WATCHDOG_EXPIRE:
    case SIM_EVENT_TRACE_MARKER:
        break;
    }
}

void mesh_sim_scheduler_cancel_role_work(struct mesh_sim_world *world,
                                         uint8_t node_index)
{
    size_t i = 0u;

    if (world == NULL || node_index >= world->role_count) {
        return;
    }
    for (i = 0u; i < world->connection_count; i++) {
        struct mesh_sim_connection *connection = &world->connections[i];

        if (connection->node_a == node_index || connection->node_b == node_index) {
            connection->repair_pending = false;
        }
    }
    for (i = 0u; i < world->event_count;) {
        if (event_uses_role(world, &world->events[i], node_index)) {
            invalidate_event_object(world, &world->events[i]);
            if (i + 1u < world->event_count) {
                memmove(&world->events[i],
                        &world->events[i + 1u],
                        (world->event_count - i - 1u) *
                            sizeof(world->events[0]));
            }
            world->event_count--;
            continue;
        }
        i++;
    }
}

int mesh_sim_scheduler_next(const struct mesh_sim_world *world,
                            uint64_t end_us)
{
    size_t best = SIZE_MAX;

    for (size_t i = 0u; i < world->event_count; i++) {
        const struct mesh_sim_event *event = &world->events[i];

        if (!event->pending || event->time_us > end_us) {
            continue;
        }
        if (best == SIZE_MAX || event->time_us < world->events[best].time_us ||
            (event->time_us == world->events[best].time_us &&
             (event->priority < world->events[best].priority ||
              (event->priority == world->events[best].priority &&
               event->sequence < world->events[best].sequence)))) {
            best = i;
        }
    }
    return best == SIZE_MAX ? -1 : (int)best;
}

void mesh_sim_scheduler_pop(struct mesh_sim_world *world,
                            size_t event_index,
                            struct mesh_sim_event *event)
{
    *event = world->events[event_index];
    if (event_index + 1u < world->event_count) {
        memmove(&world->events[event_index],
                &world->events[event_index + 1u],
                (world->event_count - event_index - 1u) *
                    sizeof(world->events[0]));
    }
    world->event_count--;
}

int mesh_sim_schedule_trace_marker(struct mesh_sim_world *world,
                                   uint64_t at_us,
                                   uint8_t priority,
                                   uint16_t object_identity)
{
    return mesh_sim_scheduler_schedule_priority(world,
                                                SIM_EVENT_TRACE_MARKER,
                                                at_us,
                                                object_identity,
                                                priority,
                                                0u);
}

int mesh_sim_run_until(struct mesh_sim_world *world, uint64_t end_us)
{
    int event_index;

    if (world == NULL || end_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    if (world->last_error != MESH_SIM_OK) {
        return world->last_error;
    }
    while ((event_index = mesh_sim_scheduler_next(world, end_us)) >= 0) {
        struct mesh_sim_event event;
        int ret;

        mesh_sim_scheduler_pop(world, (size_t)event_index, &event);
        world->now_us = event.time_us;
        ret = mesh_sim_process_event(world, &event);
        if (ret != MESH_SIM_OK) {
            if (world->last_error == MESH_SIM_OK) {
                world->last_error = ret;
            }
            return ret;
        }
    }
    world->now_us = end_us;
    return MESH_SIM_OK;
}

int mesh_sim_run(struct mesh_sim_world *world)
{
    if (world == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    while (world->event_count > 0u) {
        uint64_t final_time = world->now_us;

        for (size_t i = 0u; i < world->event_count; i++) {
            if (world->events[i].pending && world->events[i].time_us > final_time) {
                final_time = world->events[i].time_us;
            }
        }
        if (mesh_sim_run_until(world, final_time) != MESH_SIM_OK) {
            return world->last_error;
        }
    }
    return MESH_SIM_OK;
}
