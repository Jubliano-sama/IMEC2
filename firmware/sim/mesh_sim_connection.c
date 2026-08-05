#include "mesh_sim_internal.h"
#include "mesh_sim_interval.h"

#include <limits.h>
#include <string.h>

_Static_assert(PACKET_EXT_MAX_PAYLOAD_LEN <= UINT16_MAX,
               "simulated packet payload length must fit the protocol codec");

static bool connection_event_work_current(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection_event *event)
{
    const struct mesh_sim_connection *connection;

    if (!event->valid || event->connection_index >= world->connection_count) {
        return false;
    }
    connection = &world->connections[event->connection_index];
    return connection->node_a < world->role_count &&
           connection->node_b < world->role_count &&
           event->node_a_work_epoch ==
               world->roles[connection->node_a].work_epoch &&
           event->node_b_work_epoch ==
               world->roles[connection->node_b].work_epoch;
}

static bool connection_timings_compatible(
    const struct mesh_sim_connection *connection)
{
    const struct mesh_event_timing *a = &connection->timing_a;
    const struct mesh_event_timing *b = &connection->timing_b;

    return a->mesh_channel == b->mesh_channel &&
           a->event_interval_ms == b->event_interval_ms &&
           a->event_window_ms == b->event_window_ms &&
           a->guard_ms == b->guard_ms &&
           a->peer_clock_skew_estimate_ppm ==
               b->peer_clock_skew_estimate_ppm &&
           a->max_missed_events == b->max_missed_events &&
           a->supervision_timeout_ms == b->supervision_timeout_ms;
}

static struct mesh_event_owner *connection_owner_for_node(
    struct mesh_sim_connection *connection,
    uint8_t node_index)
{
    if (node_index == connection->node_a) {
        return &connection->owner_a;
    }
    if (node_index == connection->node_b) {
        return &connection->owner_b;
    }
    return NULL;
}

static struct mesh_event_timing *connection_timing_for_node(
    struct mesh_sim_connection *connection,
    uint8_t node_index)
{
    if (node_index == connection->node_a) {
        return &connection->timing_a;
    }
    if (node_index == connection->node_b) {
        return &connection->timing_b;
    }
    return NULL;
}

static uint8_t connection_peer_for_node(
    const struct mesh_sim_connection *connection,
    uint8_t node_index)
{
    if (node_index == connection->node_a) {
        return connection->node_b;
    }
    if (node_index == connection->node_b) {
        return connection->node_a;
    }
    return UINT8_MAX;
}

static int connection_begin_owners(struct mesh_sim_world *world,
                                   struct mesh_sim_connection *connection,
                                   uint32_t session_id,
                                   uint16_t proposal_sequence,
                                   uint8_t proposer,
                                   uint64_t proposer_boot_nonce)
{
    int ret = mesh_event_owner_begin_with_boot_nonce(
        &connection->owner_a, world->roles[connection->node_b].id,
        session_id, proposal_sequence, proposer == connection->node_b,
        proposer == connection->node_b ? proposer_boot_nonce : 0u);

    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_event_owner_begin_with_boot_nonce(
        &connection->owner_b, world->roles[connection->node_a].id,
        session_id, proposal_sequence, proposer == connection->node_a,
        proposer == connection->node_a ? proposer_boot_nonce : 0u);
    if (ret != PROTO_OK) {
        mesh_event_owner_abandon(&connection->owner_a);
    }
    return ret;
}

static bool connection_session_retained(const struct mesh_sim_world *world,
                                        uint32_t session_id)
{
    for (size_t i = 0u; i < world->connection_count; i++) {
        const struct mesh_sim_connection *connection = &world->connections[i];

        if (connection->repair_session_id == session_id ||
            mesh_event_owner_retains_session(&connection->owner_a,
                                             session_id) ||
            mesh_event_owner_retains_session(&connection->owner_b,
                                             session_id)) {
            return true;
        }
    }
    return false;
}

static uint16_t connection_next_control_sequence(
    struct mesh_sim_world *world,
    uint8_t proposer)
{
    struct mesh_sim_role_instance *node = &world->roles[proposer];

    node->event_control_seq++;
    if (node->event_control_seq == 0u) {
        node->event_control_seq = 1u;
    }
    return node->event_control_seq;
}

static uint32_t connection_new_session(struct mesh_sim_world *world,
                                       uint8_t proposer)
{
    struct mesh_sim_role_instance *node;
    uint32_t seed;

    if (!mesh_sim_node_index_valid(world, proposer)) {
        return 0u;
    }
    node = &world->roles[proposer];
    /*
     * Match the firmware's per-boot session allocator: seed once, then advance
     * serially. Consume the established simulator RNG draw on every call so
     * unrelated deterministic fault and scheduling streams do not shift.
     */
    seed = mesh_sim_random(world);
    if (node->event_operation_session_next == 0u) {
        node->event_operation_session_next = seed == 0u ? 1u : seed;
    }
    do {
        node->event_operation_session_next++;
        if (node->event_operation_session_next == 0u) {
            node->event_operation_session_next = 1u;
        }
    } while (connection_session_retained(
        world, node->event_operation_session_next));
    return node->event_operation_session_next;
}

static uint64_t connection_endpoints_ready_us(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection)
{
    uint64_t ready_us = world->now_us;

    for (size_t endpoint = 0u; endpoint < 2u; endpoint++) {
        uint8_t node_index = endpoint == 0u ? connection->node_a :
                                             connection->node_b;
        const struct dwm3000_runtime *runtime =
            &world->roles[node_index].dwm3000;

        if (runtime->cpu_busy_until_us > ready_us) {
            ready_us = runtime->cpu_busy_until_us;
        }
        if (runtime->spi_busy_until_us > ready_us) {
            ready_us = runtime->spi_busy_until_us;
        }
        if (runtime->radio_busy_until_us > ready_us) {
            ready_us = runtime->radio_busy_until_us;
        }
    }
    return ready_us;
}

static int connection_radio_policy_deferred(
    const struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t start_us,
    uint64_t end_us,
    bool *deferred)
{
    int ret;

    if (deferred == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_sim_radio_operation_conflicts(world,
                                             node_index,
                                             start_us,
                                             end_us);
    if (ret == MESH_SIM_ERR_RADIO_CONFLICT) {
        *deferred = true;
        return MESH_SIM_OK;
    }
    if (ret == MESH_SIM_OK) {
        *deferred = false;
    }
    return ret;
}

static int connection_control_frame_len(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection,
    size_t *frame_len)
{
    uint8_t payload[128];
    struct proto_packet packet;
    size_t payload_len = 0u;
    int ret;

    if (world == NULL || connection == NULL || frame_len == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_append_event_timing_tlvs_at(
        payload,
        sizeof(payload),
        &payload_len,
        &connection->timing_a,
        mesh_sim_time_ms(world->now_us));
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = tlv_append_u64(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_EVENT_BOOT_NONCE,
                         world->roles[connection->node_a].event_boot_nonce);
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = mesh_init_event_control(
        &packet,
        MSG_MESH_EVENT_PROPOSE,
        world->roles[connection->node_a].id,
        world->roles[connection->node_b].id,
        1u,
        1u,
        (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    *frame_len = proto_packet_encoded_len(packet.payload_len);
    return *frame_len == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

static int connection_repair_duration_us(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection,
    uint64_t *duration_us,
    uint16_t *control_frame_len)
{
    uint64_t total;
    uint64_t per_frame_spi_us;
    uint32_t airtime_us;
    size_t frame_len;
    int ret;

    if (duration_us == NULL || control_frame_len == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = connection_control_frame_len(world, connection, &frame_len);
    if (ret != PROTO_OK || frame_len > UINT16_MAX) {
        return ret == PROTO_OK ? MESH_SIM_ERR_FRAME_TOO_LONG : ret;
    }
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        frame_len);
    if (airtime_us == 0u) {
        return MESH_SIM_ERR_FRAME_TOO_LONG;
    }

    per_frame_spi_us =
        dwm3000_runtime_spi_transfer_us(
            DWM3000_RUNTIME_SPI_FAST,
            frame_len + DWM3000_RUNTIME_FRAME_IO_HEADER_BYTES) +
        dwm3000_runtime_spi_transfer_us(
            DWM3000_RUNTIME_SPI_FAST,
            DWM3000_RUNTIME_TX_START_TRANSFER_BYTES) +
        dwm3000_runtime_spi_transfer_us(
            DWM3000_RUNTIME_SPI_FAST,
            DWM3000_RUNTIME_RX_ARM_TRANSFER_BYTES) +
        dwm3000_runtime_spi_transfer_us(
            DWM3000_RUNTIME_SPI_FAST,
            DWM3000_RUNTIME_STATUS_TRANSFER_BYTES) +
        dwm3000_runtime_spi_transfer_us(
            DWM3000_RUNTIME_SPI_FAST,
            frame_len + DWM3000_RUNTIME_FRAME_IO_HEADER_BYTES);
    total = (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u +
            (uint64_t)MESH_RADIO_EVENT_ACCEPT_DELAY_MS * 1000u +
            2u * ((uint64_t)airtime_us + per_frame_spi_us);
    if (total == 0u) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    *duration_us = total;
    *control_frame_len = (uint16_t)frame_len;
    return MESH_SIM_OK;
}

static uint64_t connection_repair_safe_start(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection,
    uint64_t duration_us)
{
    uint64_t start_us = connection_endpoints_ready_us(world, connection);
    bool moved;

    do {
        uint64_t end_us;

        moved = false;
        for (size_t endpoint = 0u; endpoint < 2u; endpoint++) {
            uint8_t node_index = endpoint == 0u ? connection->node_a :
                                                 connection->node_b;
            const struct mesh_sim_role_instance *node =
                &world->roles[node_index];

            if (node->runtime.radio_owner != MESH_RUNTIME_RADIO_NONE &&
                node->runtime.radio_busy_until_us > start_us) {
                start_us = node->runtime.radio_busy_until_us;
                moved = true;
            }
        }
        if (start_us > UINT64_MAX - duration_us) {
            return UINT64_MAX;
        }
        end_us = start_us + duration_us;
        for (size_t i = 0u; i < world->rx_window_count; i++) {
            const struct mesh_sim_rx_window *window = &world->rx_windows[i];

            if (window->valid &&
                (window->node_index == connection->node_a ||
                 window->node_index == connection->node_b) &&
                mesh_sim_interval_overlaps(start_us, end_us,
                                           window->start_us, window->end_us) &&
                window->end_us > start_us) {
                start_us = window->end_us;
                moved = true;
            }
        }
        for (size_t i = 0u; i < world->transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world->transmissions[i];

            if (tx->valid &&
                (tx->node_index == connection->node_a ||
                 tx->node_index == connection->node_b) &&
                mesh_sim_interval_overlaps(start_us, end_us,
                                           tx->start_us, tx->end_us) &&
                tx->end_us > start_us) {
                start_us = tx->end_us;
                moved = true;
            }
        }
    } while (moved);
    return start_us;
}

static uint8_t connection_repair_requester(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection)
{
    const struct mesh_sim_role_instance *node_a =
        &world->roles[connection->node_a];
    const struct mesh_sim_role_instance *node_b =
        &world->roles[connection->node_b];
    bool a_has_work = mesh_sim_has_peer_work(node_a, node_b->id) ||
                      mesh_sim_has_active_relay_to(node_a, node_b->id);
    bool b_has_work = mesh_sim_has_peer_work(node_b, node_a->id) ||
                      mesh_sim_has_active_relay_to(node_b, node_a->id);

    if (!a_has_work && !b_has_work) {
        return UINT8_MAX;
    }
    if (a_has_work != b_has_work) {
        return a_has_work ? connection->node_a : connection->node_b;
    }
    return connection->node_a;
}

static int sync_connection_timing(struct mesh_sim_world *world,
                                  struct mesh_sim_connection *connection)
{
    return mesh_sim_publish_connection_timing(world, connection);
}

static void retire_connection_for_repair(
    struct mesh_sim_world *world,
    struct mesh_sim_connection *connection)
{
    mesh_sim_clear_connection_timing(world, connection);
    mesh_event_owner_abandon(&connection->owner_a);
    mesh_event_owner_abandon(&connection->owner_b);
    connection->timing_a.route_fresh = false;
    connection->timing_a.timing_fresh = false;
    connection->timing_a.fallback_required = true;
    connection->timing_b.route_fresh = false;
    connection->timing_b.timing_fresh = false;
    connection->timing_b.fallback_required = true;
    connection->establishing = true;
}

int mesh_sim_add_connection(struct mesh_sim_world *world,
                            uint8_t node_a,
                            uint8_t node_b,
                            const struct mesh_event_params *params,
                            bool node_a_transmits_first,
                            uint16_t *connection_index)
{
    struct mesh_sim_connection *connection;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_a) || !mesh_sim_node_index_valid(world, node_b) ||
        params == NULL || connection_index == NULL ||
        !world->reachable[node_a][node_b] ||
        !world->roles[node_a].relay_initialized ||
        !world->roles[node_b].relay_initialized) {
        return MESH_SIM_ERR_ARG;
    }
    if (world->connection_count >= MESH_SIM_MAX_CONNECTIONS) {
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    *connection_index = (uint16_t)world->connection_count;
    connection = &world->connections[world->connection_count++];
    memset(connection, 0, sizeof(*connection));
    connection->node_a = node_a;
    connection->node_b = node_b;
    ret = mesh_event_timing_negotiate(&connection->timing_a, params, true);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_event_timing_negotiate(&connection->timing_b, params, true);
    if (ret != PROTO_OK) {
        return ret;
    }
    connection->repair_session_id = connection_new_session(world, node_a);
    connection->timing_a.event_counter = connection->repair_session_id;
    connection->timing_b.event_counter = connection->repair_session_id;
    mesh_event_timing_set_local_first_slot_tx(&connection->timing_a,
                                              node_a_transmits_first);
    mesh_event_timing_set_local_first_slot_tx(&connection->timing_b,
                                              !node_a_transmits_first);
    /* Preserve the established deterministic RNG stream while modeling the
     * production node's monotonic event-control sequence. */
    (void)mesh_sim_random(world);
    connection->repair_seq = connection_next_control_sequence(world, node_a);
    ret = connection_begin_owners(world, connection,
                                  connection->repair_session_id,
                                  connection->repair_seq,
                                  node_a,
                                  world->roles[node_a].event_boot_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    connection->valid = true;
    return sync_connection_timing(world, connection);
}

static int schedule_connection_repair_exchange(
    struct mesh_sim_world *world,
    uint16_t connection_index,
    uint8_t requester,
    uint64_t start_us,
    uint64_t end_us,
    uint16_t control_frame_len)
{
    struct mesh_sim_connection *connection;
    int ret;

    if (connection_index >= world->connection_count ||
        end_us <= start_us) {
        return MESH_SIM_ERR_ARG;
    }
    connection = &world->connections[connection_index];
    if (requester != connection->node_a && requester != connection->node_b) {
        return MESH_SIM_ERR_ARG;
    }
    connection->repair_start_us = start_us;
    connection->repair_end_us = end_us;
    connection->repair_requester = requester;
    connection->repair_control_frame_len = control_frame_len;
    connection->repair_session_id = connection_new_session(world, requester);
    (void)mesh_sim_random(world);
    connection->repair_seq = connection_next_control_sequence(world,
                                                               requester);
    connection->repair_propose_decoded = false;
    connection->repair_accept_decoded = false;
    connection->repair_pending = true;
    ret = mesh_sim_scheduler_schedule(world,
                         SIM_EVENT_CONNECTION_REPAIR_START,
                         start_us,
                         connection_index);
    if (ret != MESH_SIM_OK) {
        connection->repair_pending = false;
        return ret;
    }
    ret = mesh_sim_scheduler_schedule(world,
                         SIM_EVENT_CONNECTION_REPAIR_END,
                         end_us,
                         connection_index);
    if (ret != MESH_SIM_OK) {
        connection->repair_pending = false;
    }
    return ret;
}

int mesh_sim_add_connection_over_radio(struct mesh_sim_world *world,
                                       uint8_t node_a,
                                       uint8_t node_b,
                                       const struct mesh_event_params *params,
                                       bool node_a_transmits_first,
                                       uint16_t *connection_index)
{
    struct mesh_sim_connection *connection;
    uint64_t duration_us;
    uint64_t start_us;
    uint16_t control_frame_len;
    int ret;

    ret = mesh_sim_add_connection(world,
                                  node_a,
                                  node_b,
                                  params,
                                  node_a_transmits_first,
                                  connection_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    connection = &world->connections[*connection_index];
    retire_connection_for_repair(world, connection);
    ret = connection_repair_duration_us(world,
                                        connection,
                                        &duration_us,
                                        &control_frame_len);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    start_us = connection_repair_safe_start(world, connection, duration_us);
    if (start_us == UINT64_MAX || start_us > UINT64_MAX - duration_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    return schedule_connection_repair_exchange(world,
                                               *connection_index,
                                               node_a,
                                               start_us,
                                               start_us + duration_us,
                                               control_frame_len);
}

int mesh_sim_renegotiate_connection_over_radio(
    struct mesh_sim_world *world,
    uint16_t connection_index,
    uint8_t requester)
{
    struct mesh_sim_connection *connection;
    uint64_t duration_us;
    uint64_t start_us;
    uint16_t control_frame_len;
    int ret;

    if (world == NULL || connection_index >= world->connection_count) {
        return MESH_SIM_ERR_ARG;
    }
    connection = &world->connections[connection_index];
    if (!connection->valid || connection->repair_pending ||
        (requester != connection->node_a &&
         requester != connection->node_b)) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }

    retire_connection_for_repair(world, connection);

    ret = connection_repair_duration_us(world, connection, &duration_us,
                                        &control_frame_len);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    start_us = connection_repair_safe_start(world, connection, duration_us);
    if (start_us == UINT64_MAX || start_us > UINT64_MAX - duration_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    return schedule_connection_repair_exchange(
        world, connection_index, requester, start_us, start_us + duration_us,
        control_frame_len);
}

static uint8_t skip_unstartable_events(
    struct mesh_event_timing *timing,
    struct mesh_event_diagnostics *diagnostics,
    uint64_t now_us)
{
    uint8_t skipped = mesh_event_skip_elapsed(timing,
                                              mesh_sim_time_ms(now_us),
                                              diagnostics);

    while (skipped < UINT8_MAX &&
           (uint64_t)timing->next_event_time_ms * 1000u < now_us) {
        if (mesh_event_timing_local_rx_slot(timing)) {
            mesh_event_note_missed(timing, diagnostics);
        } else {
            timing->next_event_time_ms += timing->event_interval_ms;
            timing->event_counter++;
        }
        skipped++;
    }
    return skipped;
}

static int skip_connection_elapsed_events(
    struct mesh_event_timing *timing_a,
    struct mesh_event_diagnostics *diagnostics_a,
    struct mesh_event_timing *timing_b,
    struct mesh_event_diagnostics *diagnostics_b,
    uint64_t now_us,
    uint8_t *skipped_events)
{
    uint8_t skipped_a;
    uint8_t skipped_b;

    if (timing_a == NULL || timing_b == NULL || skipped_events == NULL ||
        timing_a->next_event_time_ms != timing_b->next_event_time_ms) {
        return MESH_SIM_ERR_CONNECTION_PLAN;
    }
    skipped_a = skip_unstartable_events(timing_a, diagnostics_a, now_us);
    skipped_b = skip_unstartable_events(timing_b, diagnostics_b, now_us);
    if (skipped_a != skipped_b ||
        timing_a->next_event_time_ms != timing_b->next_event_time_ms) {
        return MESH_SIM_ERR_CONNECTION_PLAN;
    }
    *skipped_events = skipped_a;
    return MESH_SIM_OK;
}

int mesh_sim_connection_next_action(const struct mesh_sim_world *world,
                                    uint16_t connection_index,
                                    struct mesh_sim_connection_action *action)
{
    const struct mesh_sim_connection *connection;
    struct mesh_event_timing timing_a;
    struct mesh_event_timing timing_b;
    struct mesh_event_diagnostics diagnostics_a;
    struct mesh_event_diagnostics diagnostics_b;
    uint64_t duration_us;
    uint64_t endpoints_ready_us;
    uint64_t start_us;
    uint16_t control_frame_len;
    bool usable_a;
    bool usable_b;
    int ret;

    if (world == NULL || action == NULL ||
        connection_index >= world->connection_count) {
        return MESH_SIM_ERR_ARG;
    }
    connection = &world->connections[connection_index];
    memset(action, 0, sizeof(*action));
    if (!connection->valid) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    if (connection->repair_pending) {
        if (connection->repair_end_us < world->now_us) {
            return MESH_SIM_ERR_EVENT_ORDER;
        }
        action->start_us = world->now_us > connection->repair_start_us ?
                           world->now_us : connection->repair_start_us;
        action->end_us = connection->repair_end_us;
        action->kind = MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR;
        action->already_scheduled = true;
        return MESH_SIM_OK;
    }
    if (!connection_timings_compatible(connection)) {
        return MESH_SIM_ERR_CONNECTION_PLAN;
    }
    timing_a = connection->timing_a;
    timing_b = connection->timing_b;
    diagnostics_a = connection->diagnostics_a;
    diagnostics_b = connection->diagnostics_b;
    endpoints_ready_us = connection_endpoints_ready_us(world, connection);
    ret = skip_connection_elapsed_events(&timing_a,
                                         &diagnostics_a,
                                         &timing_b,
                                         &diagnostics_b,
                                         endpoints_ready_us,
                                         &action->skipped_events);
    if (ret != MESH_SIM_OK) {
        return ret;
    }

    usable_a = mesh_event_timing_usable(&timing_a,
                                        mesh_sim_time_ms(endpoints_ready_us));
    usable_b = mesh_event_timing_usable(&timing_b,
                                        mesh_sim_time_ms(endpoints_ready_us));
    if (usable_a && usable_b) {
        action->start_us =
            (uint64_t)timing_a.next_event_time_ms * 1000u;
        if (action->start_us > UINT64_MAX -
            (uint64_t)timing_a.event_window_ms * 1000u) {
            return MESH_SIM_ERR_EVENT_ORDER;
        }
        action->end_us = action->start_us +
                         (uint64_t)timing_a.event_window_ms * 1000u;
        action->kind = MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT;
        return MESH_SIM_OK;
    }
    if (connection_repair_requester(world, connection) == UINT8_MAX) {
        action->start_us = UINT64_MAX;
        action->end_us = UINT64_MAX;
        action->kind = MESH_SIM_CONNECTION_ACTION_NONE;
        return MESH_SIM_OK;
    }

    ret = connection_repair_duration_us(world,
                                        connection,
                                        &duration_us,
                                        &control_frame_len);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    (void)control_frame_len;
    start_us = connection_repair_safe_start(world, connection, duration_us);
    if (start_us == UINT64_MAX || start_us > UINT64_MAX - duration_us) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    action->start_us = start_us;
    action->end_us = start_us + duration_us;
    action->kind = MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR;
    return MESH_SIM_OK;
}

uint32_t mesh_sim_connection_next_event_ms(const struct mesh_sim_world *world,
                                           uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    uint64_t start_ms;

    if (mesh_sim_connection_next_action(world,
                                        connection_index,
                                        &action) != MESH_SIM_OK) {
        return 0u;
    }
    if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return 0u;
    }
    start_ms = action.start_us / 1000u +
               (action.start_us % 1000u == 0u ? 0u : 1u);
    return start_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)start_ms;
}

int mesh_sim_expire_connection_ownership(struct mesh_sim_world *world,
                                         uint16_t connection_index,
                                         uint8_t *expired_endpoints)
{
    struct mesh_sim_connection *connection;
    uint32_t now_ms;
    uint8_t expired = 0u;

    if (world == NULL || connection_index >= world->connection_count) {
        return MESH_SIM_ERR_ARG;
    }
    connection = &world->connections[connection_index];
    now_ms = mesh_sim_time_ms(world->now_us);
    if (connection->owner_a.active && connection->owner_b.active &&
        (!mesh_event_timing_usable(&connection->timing_a, now_ms) ||
         !mesh_event_timing_usable(&connection->timing_b, now_ms))) {
        mesh_event_owner_abandon(&connection->owner_a);
        mesh_event_owner_abandon(&connection->owner_b);
        connection->timing_a.route_fresh = false;
        connection->timing_a.timing_fresh = false;
        connection->timing_a.fallback_required = true;
        connection->timing_b.route_fresh = false;
        connection->timing_b.timing_fresh = false;
        connection->timing_b.fallback_required = true;
        mesh_sim_clear_connection_timing(world, connection);
        world->roles[connection->node_a].radio_state =
            mesh_sim_radio_post_operation_state(
                &world->roles[connection->node_a], world->now_us);
        world->roles[connection->node_b].radio_state =
            mesh_sim_radio_post_operation_state(
                &world->roles[connection->node_b], world->now_us);
        expired = 2u;
    }
    for (size_t endpoint = 0u; endpoint < 2u; endpoint++) {
        uint8_t node_index = endpoint == 0u ? connection->node_a :
                                             connection->node_b;
        uint8_t peer_index = endpoint == 0u ? connection->node_b :
                                             connection->node_a;
        struct mesh_event_timing *timing = endpoint == 0u ?
            &connection->timing_a : &connection->timing_b;
        struct mesh_event_owner *owner = endpoint == 0u ?
            &connection->owner_a : &connection->owner_b;

        if (!owner->active || mesh_event_timing_usable(timing, now_ms)) {
            continue;
        }
        mesh_event_owner_abandon(owner);
        timing->route_fresh = false;
        timing->timing_fresh = false;
        timing->fallback_required = true;
        mesh_relay_clear_channel9_timing(&world->roles[node_index].relay,
                                         world->roles[peer_index].id);
        world->roles[node_index].radio_state =
            mesh_sim_radio_post_operation_state(&world->roles[node_index],
                                                world->now_us);
        expired++;
    }
    if (expired_endpoints != NULL) {
        *expired_endpoints = expired;
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_next_connection_event(struct mesh_sim_world *world,
                                            uint16_t connection_index,
                                            bool receiver_preempted)
{
    struct mesh_sim_connection *connection;
    struct mesh_sim_connection_event *event;
    struct mesh_sim_connection_action action;
    uint16_t event_index;
    int ret;

    if (world == NULL || connection_index >= world->connection_count) {
        return MESH_SIM_ERR_ARG;
    }
    connection = &world->connections[connection_index];
    ret = mesh_sim_connection_next_action(world, connection_index, &action);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    if (action.already_scheduled) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    {
        uint8_t skipped_events;

        uint64_t endpoints_ready_us =
            connection_endpoints_ready_us(world, connection);

        ret = skip_connection_elapsed_events(&connection->timing_a,
                                             &connection->diagnostics_a,
                                             &connection->timing_b,
                                             &connection->diagnostics_b,
                                             endpoints_ready_us,
                                             &skipped_events);
        if (ret != MESH_SIM_OK || skipped_events != action.skipped_events) {
            return mesh_sim_fail(world,
                            ret == MESH_SIM_OK ? MESH_SIM_ERR_CONNECTION_PLAN :
                                                ret);
        }
        if (skipped_events > 0u) {
            ret = sync_connection_timing(world, connection);
            if (ret != PROTO_OK) {
                return mesh_sim_fail(world, ret);
            }
            ret = mesh_sim_trace_add(
                world,
                world->now_us,
                world->roles[connection->node_a].id,
                world->roles[connection->node_b].id,
                MESH_SIM_TRANSITION_CONNECTION_EVENTS_SKIPPED,
                0u,
                skipped_events);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    if (action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR) {
        uint64_t duration_us;
        uint8_t requester = connection_repair_requester(world, connection);

        ret = connection_repair_duration_us(
            world,
            connection,
            &duration_us,
            &connection->repair_control_frame_len);
        if (ret != MESH_SIM_OK || duration_us != action.end_us - action.start_us) {
            return mesh_sim_fail(world,
                            ret == MESH_SIM_OK ? MESH_SIM_ERR_CONNECTION_PLAN :
                                                ret);
        }
        if (requester == UINT8_MAX) {
            return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
        }
        return schedule_connection_repair_exchange(
            world,
            connection_index,
            requester,
            action.start_us,
            action.end_us,
            connection->repair_control_frame_len);
    }
    ret = mesh_sim_telemetry_reserve_connection_event(world, &event_index);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    event = &world->connection_events[event_index];
    memset(event, 0, sizeof(*event));
    event->start_us = action.start_us;
    event->end_us = action.end_us;
    event->connection_index = connection_index;
    event->node_a_work_epoch = world->roles[connection->node_a].work_epoch;
    event->node_b_work_epoch = world->roles[connection->node_b].work_epoch;
    event->receiver_preempted = receiver_preempted;
    {
        bool a_tx = mesh_event_timing_local_tx_slot(&connection->timing_a);
        bool b_tx = mesh_event_timing_local_tx_slot(&connection->timing_b);

        if (a_tx == b_tx) {
            return mesh_sim_fail(world, MESH_SIM_ERR_SLOT_DIRECTION);
        }
        event->sender_index = a_tx ? connection->node_a : connection->node_b;
        event->receiver_index = a_tx ? connection->node_b : connection->node_a;
        ret = connection_radio_policy_deferred(
            world,
            event->sender_index,
            event->start_us,
            event->end_us,
            &event->sender_policy_deferred);
        if (ret != MESH_SIM_OK) {
            return mesh_sim_fail(world, ret);
        }
        if (!event->receiver_preempted) {
            ret = connection_radio_policy_deferred(
                world,
                event->receiver_index,
                event->start_us,
                event->end_us,
                &event->receiver_policy_deferred);
            if (ret != MESH_SIM_OK) {
                return mesh_sim_fail(world, ret);
            }
        }
    }
    event->valid = true;
    return mesh_sim_scheduler_schedule(world,
                          SIM_EVENT_CONNECTION_START,
                          event->start_us,
                          event_index);
}

static int build_connection_control_packet(
    const struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection,
    uint8_t msg_type,
    uint8_t sender_index,
    const struct mesh_event_timing *timing,
    uint32_t timing_reference_ms,
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len)
{
    uint16_t seq;
    uint8_t peer_index;
    int ret;

    if (world == NULL || connection == NULL || timing == NULL ||
        packet == NULL || payload == NULL || payload_len == NULL ||
        (msg_type != MSG_MESH_EVENT_PROPOSE &&
         msg_type != MSG_MESH_EVENT_ACCEPT)) {
        return MESH_SIM_ERR_ARG;
    }
    if (sender_index == connection->node_a) {
        peer_index = connection->node_b;
    } else if (sender_index == connection->node_b) {
        peer_index = connection->node_a;
    } else {
        return MESH_SIM_ERR_ARG;
    }
    if (connection->repair_session_id == 0u ||
        timing->event_counter != connection->repair_session_id ||
        mesh_event_timing_local_tx_slot(timing) !=
            (msg_type == MSG_MESH_EVENT_PROPOSE)) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    *payload_len = 0u;
    ret = mesh_append_event_timing_tlvs_at(payload,
                                           payload_cap,
                                           payload_len,
                                           timing,
                                           timing_reference_ms);
    if (ret != PROTO_OK || *payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    if (msg_type == MSG_MESH_EVENT_PROPOSE) {
        ret = tlv_append_u64(payload,
                             payload_cap,
                             payload_len,
                             TLV_MESH_EVENT_BOOT_NONCE,
                             world->roles[sender_index].event_boot_nonce);
        if (ret != PROTO_OK || *payload_len > UINT8_MAX) {
            return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
        }
    }
    /* Canonical ACCEPT echoes the proposal's complete packet identity. */
    seq = connection->repair_seq;
    return mesh_init_event_control(packet,
                                   msg_type,
                                   world->roles[sender_index].id,
                                   world->roles[peer_index].id,
                                   connection->repair_session_id,
                                   seq,
                                   (uint8_t)*payload_len);
}

static int schedule_connection_accept(struct mesh_sim_world *world,
                                      uint16_t connection_index,
                                      uint8_t sender_index)
{
    struct mesh_sim_connection *connection =
        &world->connections[connection_index];
    struct proto_packet packet;
    uint8_t payload[128];
    uint64_t tx_start_us;
    uint32_t duration_us;
    size_t payload_len;
    int ret;

    if (world->now_us > UINT64_MAX -
        (uint64_t)MESH_RADIO_EVENT_ACCEPT_DELAY_MS * 1000u) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    tx_start_us = world->now_us +
                  (uint64_t)MESH_RADIO_EVENT_ACCEPT_DELAY_MS * 1000u;
    ret = build_connection_control_packet(
        world,
        connection,
        MSG_MESH_EVENT_ACCEPT,
        sender_index,
        &connection->repair_peer_timing,
        mesh_sim_time_ms(tx_start_us),
        &packet,
        payload,
        sizeof(payload),
        &payload_len);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    duration_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        proto_packet_encoded_len((uint16_t)payload_len));
    if (duration_us == 0u || tx_start_us > UINT64_MAX - duration_us ||
        tx_start_us + duration_us > connection->repair_end_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_DEADLINE);
    }
    return mesh_sim_schedule_packet_tx(world,
                                       sender_index,
                                       tx_start_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                       &packet,
                                       payload,
                                       payload_len,
                                       NULL);
}

static int apply_connection_control_to_endpoint(
    struct mesh_sim_world *world,
    struct mesh_sim_connection *connection,
    uint8_t node_index,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct mesh_event_timing *update_timing,
    bool local_control)
{
    struct mesh_event_owner *owner =
        connection_owner_for_node(connection, node_index);
    struct mesh_event_timing *timing =
        connection_timing_for_node(connection, node_index);
    uint8_t peer_index = connection_peer_for_node(connection, node_index);
    int ret;

    if (owner == NULL || timing == NULL || peer_index == UINT8_MAX) {
        return MESH_SIM_ERR_ARG;
    }
    ret = local_control ?
        mesh_event_owner_commit_local(owner, world->roles[node_index].id,
                                      packet, payload, payload_len) :
        mesh_event_owner_commit(owner, world->roles[node_index].id,
                                world->roles[peer_index].id, packet, payload,
                                payload_len);
    if (ret != PROTO_OK) {
        return MESH_SIM_OK;
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        timing->route_fresh = false;
        timing->timing_fresh = false;
        timing->fallback_required = true;
        mesh_relay_clear_channel9_timing(&world->roles[node_index].relay,
                                         world->roles[peer_index].id);
        if (world->roles[node_index].radio_state != MESH_SIM_RADIO_RX &&
            world->roles[node_index].radio_state != MESH_SIM_RADIO_TX) {
            world->roles[node_index].radio_state = MESH_SIM_RADIO_SLEEP;
        }
        return MESH_SIM_OK;
    }
    if (update_timing == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    *timing = *update_timing;
    if (!local_control) {
        timing->local_tx_on_even_events =
            !timing->local_tx_on_even_events;
    }
    ret = mesh_relay_set_channel9_timing(&world->roles[node_index].relay,
                                         world->roles[peer_index].id,
                                         timing);
    return ret == PROTO_OK ? MESH_SIM_OK : ret;
}

static int process_owned_connection_control(
    struct mesh_sim_world *world,
    struct mesh_sim_connection *connection,
    uint8_t receiver_index,
    uint8_t sender_index,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct mesh_event_owner *receiver_owner =
        connection_owner_for_node(connection, receiver_index);
    struct mesh_event_owner *sender_owner =
        connection_owner_for_node(connection, sender_index);
    struct mesh_event_timing timing;
    enum mesh_event_owner_decision receiver_decision;
    enum mesh_event_owner_decision sender_decision;
    uint32_t detail;
    int ret;

    receiver_decision = mesh_event_owner_classify(
        receiver_owner, world->roles[receiver_index].id,
        world->roles[sender_index].id, packet, payload, payload_len);
    sender_decision = mesh_event_owner_classify_local(
        sender_owner, world->roles[sender_index].id, packet, payload,
        payload_len);
    if (packet->msg_type == MSG_MESH_EVENT_UPDATE &&
        (receiver_decision == MESH_EVENT_OWNER_APPLY ||
         sender_decision == MESH_EVENT_OWNER_APPLY)) {
        ret = mesh_event_timing_from_tlvs_at(
            &timing, payload, payload_len, mesh_sim_time_ms(world->now_us),
            true);
        if (ret != PROTO_OK) {
            receiver_decision = MESH_EVENT_OWNER_INVALID;
            sender_decision = MESH_EVENT_OWNER_INVALID;
        }
    }

    if (sender_decision == MESH_EVENT_OWNER_APPLY) {
        ret = apply_connection_control_to_endpoint(
            world, connection, sender_index, packet, payload, payload_len,
            packet->msg_type == MSG_MESH_EVENT_UPDATE ? &timing : NULL, true);
        if (ret != MESH_SIM_OK) {
            return mesh_sim_fail(world, ret);
        }
    }
    if (receiver_decision == MESH_EVENT_OWNER_APPLY) {
        ret = apply_connection_control_to_endpoint(
            world, connection, receiver_index, packet, payload, payload_len,
            packet->msg_type == MSG_MESH_EVENT_UPDATE ? &timing : NULL,
            false);
        if (ret != MESH_SIM_OK) {
            return mesh_sim_fail(world, ret);
        }
    }

    detail = ((uint32_t)receiver_decision << 16) |
             ((uint32_t)sender_decision << 8) |
             (uint32_t)packet->msg_type;
    return mesh_sim_trace_add_packet(
        world, world->now_us, world->roles[receiver_index].id,
        world->roles[sender_index].id, MESH_SIM_TRANSITION_SCHEDULER_MARKER,
        packet, detail);
}

int mesh_sim_connection_process_local_control_packet(
    struct mesh_sim_world *world,
    uint8_t sender_index,
    uint32_t control_time_ms,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct mesh_sim_connection *connection = NULL;
    struct mesh_event_owner *owner;
    struct mesh_event_timing timing;
    uint8_t peer_index;
    enum mesh_event_owner_decision decision;
    int ret;

    if (world == NULL || packet == NULL ||
        (packet->msg_type != MSG_MESH_EVENT_UPDATE &&
         packet->msg_type != MSG_MESH_EVENT_END)) {
        return MESH_SIM_OK;
    }
    for (size_t i = 0u; i < world->connection_count; i++) {
        struct mesh_sim_connection *candidate = &world->connections[i];

        if (candidate->valid && !candidate->repair_pending &&
            (candidate->node_a == sender_index ||
             candidate->node_b == sender_index)) {
            peer_index = connection_peer_for_node(candidate, sender_index);
            if (peer_index < world->role_count &&
                packet->dst_id == world->roles[peer_index].id) {
                connection = candidate;
                break;
            }
        }
    }
    if (connection == NULL || packet->src_id != world->roles[sender_index].id) {
        return MESH_SIM_OK;
    }
    owner = connection_owner_for_node(connection, sender_index);
    decision = mesh_event_owner_classify_local(
        owner, world->roles[sender_index].id, packet, payload, payload_len);
    if (decision != MESH_EVENT_OWNER_APPLY) {
        return MESH_SIM_OK;
    }
    if (packet->msg_type == MSG_MESH_EVENT_UPDATE) {
        ret = mesh_event_timing_from_tlvs_at(&timing, payload, payload_len,
                                             control_time_ms, true);
        if (ret != PROTO_OK) {
            return MESH_SIM_OK;
        }
    }
    ret = apply_connection_control_to_endpoint(
        world, connection, sender_index, packet, payload, payload_len,
        packet->msg_type == MSG_MESH_EVENT_UPDATE ? &timing : NULL, true);
    return ret == MESH_SIM_OK ? MESH_SIM_OK : mesh_sim_fail(world, ret);
}

int mesh_sim_connection_process_control_packet(
    struct mesh_sim_world *world,
    uint8_t receiver_index,
    uint8_t sender_index,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool *handled)
{
    struct mesh_sim_connection *connection = NULL;
    struct mesh_event_timing timing;
    uint16_t expected_seq;
    int ret;

    *handled = false;
    if (packet->msg_type == MSG_MESH_EVENT_UPDATE ||
        packet->msg_type == MSG_MESH_EVENT_END) {
        *handled = true;
        for (size_t i = 0u; i < world->connection_count; i++) {
            struct mesh_sim_connection *candidate = &world->connections[i];

            if (candidate->valid && !candidate->repair_pending &&
                ((candidate->node_a == sender_index &&
                  candidate->node_b == receiver_index) ||
                 (candidate->node_b == sender_index &&
                  candidate->node_a == receiver_index))) {
                return process_owned_connection_control(
                    world, candidate, receiver_index, sender_index, packet,
                    payload, payload_len);
            }
        }
        return MESH_SIM_OK;
    }
    if (packet->msg_type != MSG_MESH_EVENT_PROPOSE &&
        packet->msg_type != MSG_MESH_EVENT_ACCEPT) {
        return MESH_SIM_OK;
    }
    *handled = true;
    for (size_t i = 0u; i < world->connection_count; i++) {
        struct mesh_sim_connection *candidate = &world->connections[i];
        uint8_t peer = candidate->repair_requester == candidate->node_a ?
                       candidate->node_b : candidate->node_a;

        if (candidate->repair_pending &&
            ((sender_index == candidate->repair_requester &&
              receiver_index == peer) ||
             (sender_index == peer &&
              receiver_index == candidate->repair_requester))) {
            connection = candidate;
            break;
        }
    }
    if (connection == NULL ||
        packet->src_id != world->roles[sender_index].id ||
        packet->dst_id != world->roles[receiver_index].id ||
        packet->session_id != connection->repair_session_id) {
        return MESH_SIM_OK;
    }
    expected_seq = connection->repair_seq;
    if (packet->seq != expected_seq) {
        return MESH_SIM_OK;
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        struct mesh_event_owner *owner = connection_owner_for_node(
            connection, receiver_index);
        enum mesh_event_owner_decision decision =
            mesh_event_owner_classify_proposal(
                owner,
                world->roles[receiver_index].id,
                world->roles[sender_index].id,
                packet,
                payload,
                payload_len);

        /* Classify before decoding timing or scheduling ACCEPT. An exact
         * duplicate is still allowed to reach the existing decoded guard,
         * while missing/zero/malformed incarnations and stale conflicts are
         * inert and cannot mutate repair timing or owner state. */
        if (decision != MESH_EVENT_OWNER_APPLY &&
            decision != MESH_EVENT_OWNER_DUPLICATE) {
            return MESH_SIM_OK;
        }
    }
    ret = mesh_event_timing_from_tlvs_at(&timing,
                                         payload,
                                         payload_len,
                                         mesh_sim_time_ms(world->now_us),
                                         true);
    if (ret != PROTO_OK) {
        return MESH_SIM_OK;
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        if (sender_index != connection->repair_requester ||
            connection->repair_propose_decoded) {
            return MESH_SIM_OK;
        }
        mesh_event_timing_set_local_first_slot_tx(&timing, false);
        connection->repair_peer_timing = timing;
        connection->repair_propose_decoded = true;
        return schedule_connection_accept(
            world,
            (uint16_t)(connection - world->connections),
            receiver_index);
    }
    if (!connection->repair_propose_decoded ||
        receiver_index != connection->repair_requester ||
        connection->repair_accept_decoded) {
        return MESH_SIM_OK;
    }
    mesh_event_timing_set_local_first_slot_tx(&timing, true);
    connection->repair_requester_timing = timing;
    connection->repair_peer_timing = timing;
    mesh_event_timing_set_local_first_slot_tx(
        &connection->repair_peer_timing,
        false);
    connection->repair_accept_decoded = true;
    return MESH_SIM_OK;
}

int mesh_sim_connection_process_repair_start(struct mesh_sim_world *world,
                                             uint16_t connection_index,
                                             const struct mesh_sim_connection_callbacks *callbacks)
{
    struct mesh_sim_connection *connection;
    struct mesh_sim_role_instance *node_a;
    struct mesh_sim_role_instance *node_b;
    struct mesh_event_params params;
    struct proto_packet proposal;
    uint8_t proposal_payload[128];
    uint8_t peer_index;
    uint64_t first_event_ms;
    uint64_t proposal_tx_us;
    uint64_t proposal_rx_end_us;
    uint64_t requester_id;
    uint64_t peer_id;
    uint64_t duration_us;
    uint32_t proposal_airtime_us;
    uint32_t detail;
    size_t proposal_payload_len;
    size_t proposal_frame_len;
    int ret;

    if (connection_index >= world->connection_count) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    connection = &world->connections[connection_index];
    if (!connection->valid || !connection->repair_pending ||
        connection->repair_start_us != world->now_us ||
        connection->repair_end_us <= world->now_us ||
        !world->reachable[connection->node_a][connection->node_b]) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ROUTE_REQUIRED);
    }
    node_a = &world->roles[connection->node_a];
    node_b = &world->roles[connection->node_b];
    if (!mesh_runtime_radio_safe(&node_a->runtime, world->now_us) ||
        !mesh_runtime_radio_safe(&node_b->runtime, world->now_us) ||
        (node_a->radio_state != MESH_SIM_RADIO_SLEEP &&
         node_a->radio_state != MESH_SIM_RADIO_IDLE) ||
        (node_b->radio_state != MESH_SIM_RADIO_SLEEP &&
         node_b->radio_state != MESH_SIM_RADIO_IDLE)) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
    }

    ret = mesh_runtime_claim_radio(&node_a->runtime,
                                   MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                   world->now_us,
                                   connection->repair_end_us);
    if (ret != MESH_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_runtime_claim_radio(&node_b->runtime,
                                   MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                   world->now_us,
                                   connection->repair_end_us);
    if (ret != MESH_RUNTIME_OK) {
        node_a->runtime.radio_owner = MESH_RUNTIME_RADIO_NONE;
        node_a->runtime.radio_busy_until_us = world->now_us;
        return mesh_sim_fail(world, ret);
    }
    node_a->radio_state = MESH_SIM_RADIO_IDLE;
    node_b->radio_state = MESH_SIM_RADIO_IDLE;
    peer_index = connection->repair_requester == connection->node_a ?
                 connection->node_b : connection->node_a;
    first_event_ms = connection->repair_end_us / 1000u +
                     (connection->repair_end_us % 1000u == 0u ? 0u : 1u);
    if (first_event_ms > UINT32_MAX - MESH_RADIO_EVENT_FIRST_DELAY_MS) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    params = (struct mesh_event_params) {
        .event_interval_ms = connection->timing_a.event_interval_ms,
        .event_window_ms = connection->timing_a.event_window_ms,
        .first_event_time_ms =
            (uint32_t)first_event_ms + MESH_RADIO_EVENT_FIRST_DELAY_MS,
        .guard_ms = connection->timing_a.guard_ms,
        .peer_clock_skew_estimate_ppm =
            connection->timing_a.peer_clock_skew_estimate_ppm,
        .max_missed_events = connection->timing_a.max_missed_events,
        .supervision_timeout_ms =
            connection->timing_a.supervision_timeout_ms,
    };
    ret = mesh_event_timing_negotiate(&connection->repair_requester_timing,
                                      &params,
                                      true);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (!mesh_event_timing_bind_proposal_session(
            &connection->repair_requester_timing,
            connection->repair_session_id)) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    connection->repair_peer_timing = connection->repair_requester_timing;
    mesh_event_timing_set_local_first_slot_tx(&connection->repair_peer_timing,
                                              false);
    if (connection->repair_start_us > UINT64_MAX -
        (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    proposal_tx_us = connection->repair_start_us +
                     (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u;
    ret = build_connection_control_packet(
        world,
        connection,
        MSG_MESH_EVENT_PROPOSE,
        connection->repair_requester,
        &connection->repair_requester_timing,
        mesh_sim_time_ms(proposal_tx_us),
        &proposal,
        proposal_payload,
        sizeof(proposal_payload),
        &proposal_payload_len);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    proposal_frame_len =
        proto_packet_encoded_len((uint16_t)proposal_payload_len);
    proposal_airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        proposal_frame_len);
    if (proposal_frame_len == 0u || proposal_airtime_us == 0u ||
        proposal_tx_us > UINT64_MAX - proposal_airtime_us -
                         mesh_sim_radio_max_propagation_for_tx(
                             world,
                             connection->repair_requester) - 1u) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    proposal_rx_end_us = proposal_tx_us + proposal_airtime_us +
                         mesh_sim_radio_max_propagation_for_tx(
                             world,
                             connection->repair_requester) + 1u;
    if (proposal_rx_end_us >= connection->repair_end_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_DEADLINE);
    }
    ret = mesh_sim_schedule_rx(world,
                               peer_index,
                               connection->repair_start_us,
                               proposal_rx_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                               NULL);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_schedule_rx(world,
                               connection->repair_requester,
                               proposal_rx_end_us,
                               connection->repair_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                               NULL);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_schedule_packet_tx(world,
                                      connection->repair_requester,
                                      proposal_tx_us,
                                      UWB_CHANNEL_WAKE_CONTACT,
                                      MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                      &proposal,
                                      proposal_payload,
                                      proposal_payload_len,
                                      NULL);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    requester_id = world->roles[connection->repair_requester].id;
    peer_id = connection->repair_requester == connection->node_a ?
              node_b->id : node_a->id;
    duration_us = connection->repair_end_us - connection->repair_start_us;
    detail = ((uint32_t)connection->repair_control_frame_len << 16) |
             (uint32_t)(duration_us / 1000u +
                        (duration_us % 1000u == 0u ? 0u : 1u));
    callbacks->worker_started(world, connection->node_a);
    callbacks->worker_started(world, connection->node_b);
    return mesh_sim_trace_add(world,
                          world->now_us,
                          requester_id,
                          peer_id,
                          MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED,
                          MSG_MESH_EVENT_PROPOSE,
                          detail);
}

int mesh_sim_connection_process_repair_end(struct mesh_sim_world *world,
                                           uint16_t connection_index,
                                           const struct mesh_sim_connection_callbacks *callbacks)
{
    struct mesh_sim_connection *connection;
    struct mesh_sim_role_instance *node_a;
    struct mesh_sim_role_instance *node_b;
    uint64_t requester_id;
    uint64_t peer_id;
    uint8_t expired_endpoints;
    int ret;

    if (connection_index >= world->connection_count) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    connection = &world->connections[connection_index];
    if (!connection->valid || !connection->repair_pending ||
        connection->repair_end_us != world->now_us ||
        !world->reachable[connection->node_a][connection->node_b]) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ROUTE_REQUIRED);
    }
    /*
     * Resolve supervision at the actual repair boundary. A reset endpoint's
     * peer may still retain a usable live owner and must reject that early
     * replacement, while a connection whose supervision expired at both
     * endpoints must retire the old operation before either side proposes.
     */
    ret = mesh_sim_expire_connection_ownership(
        world, connection_index, &expired_endpoints);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    node_a = &world->roles[connection->node_a];
    node_b = &world->roles[connection->node_b];
    if (node_a->runtime.radio_owner != MESH_RUNTIME_RADIO_CHANNEL9_EVENT ||
        node_b->runtime.radio_owner != MESH_RUNTIME_RADIO_CHANNEL9_EVENT) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
    }
    ret = mesh_runtime_release_radio(&node_a->runtime,
                                     MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                     world->now_us);
    if (ret != MESH_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_runtime_release_radio(&node_b->runtime,
                                     MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                     world->now_us);
    if (ret != MESH_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    node_a->radio_state = MESH_SIM_RADIO_SLEEP;
    node_b->radio_state = MESH_SIM_RADIO_SLEEP;
    connection->repair_pending = false;
    requester_id = world->roles[connection->repair_requester].id;
    peer_id = connection->repair_requester == connection->node_a ?
              node_b->id : node_a->id;
    if (!connection->repair_propose_decoded ||
        !connection->repair_accept_decoded) {
        ret = mesh_sim_trace_add(
            world,
            world->now_us,
            requester_id,
            peer_id,
            MESH_SIM_TRANSITION_CONNECTION_PREEMPTED,
            MSG_MESH_EVENT_ACCEPT,
            (connection->repair_propose_decoded ? 1u : 0u) |
                (connection->repair_accept_decoded ? 2u : 0u));
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = callbacks->worker_completed(world, connection->node_a, true);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        return callbacks->worker_completed(world, connection->node_b, true);
    }
    if (connection->repair_requester == connection->node_a) {
        connection->timing_a = connection->repair_requester_timing;
        connection->timing_b = connection->repair_peer_timing;
    } else {
        connection->timing_b = connection->repair_requester_timing;
        connection->timing_a = connection->repair_peer_timing;
    }
    ret = sync_connection_timing(world, connection);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = connection_begin_owners(world, connection,
                                  connection->repair_session_id,
                                  connection->repair_seq,
                                  connection->repair_requester,
                                  world->roles[connection->repair_requester].event_boot_nonce);
    if (ret != PROTO_OK) {
        mesh_sim_clear_connection_timing(world, connection);
        return mesh_sim_fail(world, ret);
    }
    connection->establishing = false;
    connection->completed_repairs++;
    ret = mesh_sim_trace_add(world,
                         world->now_us,
                         requester_id,
                         peer_id,
                         MESH_SIM_TRANSITION_CONNECTION_REPAIRED,
                         MSG_MESH_EVENT_ACCEPT,
                         connection->completed_repairs);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = callbacks->worker_completed(world, connection->node_a, true);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return callbacks->worker_completed(world, connection->node_b, true);
}

int mesh_sim_connection_process_start(struct mesh_sim_world *world,
                                      uint16_t connection_event_index,
                                      const struct mesh_sim_connection_callbacks *callbacks,
                                      mesh_sim_connection_tx_ready_fn tx_ready)
{
    struct mesh_sim_connection_event *event;
    struct mesh_sim_connection *connection;
    struct mesh_sim_role_instance *sender;
    struct mesh_sim_role_instance *receiver;
    struct mesh_channel5_requirements requirements = {0};
    struct mesh_event_plan plan_a;
    struct mesh_event_plan plan_b;
    struct mesh_sim_connection_tx_result tx_result = {0};
    uint64_t tx_start_us;
    bool a_tx;
    bool b_tx;
    int ret;

    if (connection_event_index >= world->connection_event_count) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    event = &world->connection_events[connection_event_index];
    if (!connection_event_work_current(world, event)) {
        return MESH_SIM_OK;
    }
    connection = &world->connections[event->connection_index];
    a_tx = mesh_event_timing_local_tx_slot(&connection->timing_a);
    b_tx = mesh_event_timing_local_tx_slot(&connection->timing_b);
    if (a_tx == b_tx) {
        return mesh_sim_fail(world, MESH_SIM_ERR_SLOT_DIRECTION);
    }
    event->sender_index = a_tx ? connection->node_a : connection->node_b;
    event->receiver_index = a_tx ? connection->node_b : connection->node_a;
    sender = &world->roles[event->sender_index];
    receiver = &world->roles[event->receiver_index];

    /*
     * Channel-5 control and another already-planned radio owner take
     * precedence over this Channel-9 turn.  The conflict may have been
     * scheduled after this connection event, so repeat the policy check at
     * the actual boundary before starting a receiver worker.
     */
    ret = connection_radio_policy_deferred(
        world,
        event->sender_index,
        event->start_us,
        event->end_us,
        &event->sender_policy_deferred);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (!event->receiver_preempted) {
        ret = connection_radio_policy_deferred(
            world,
            event->receiver_index,
            event->start_us,
            event->end_us,
            &event->receiver_policy_deferred);
        if (ret != MESH_SIM_OK) {
            return mesh_sim_fail(world, ret);
        }
    }
    if (!event->receiver_policy_deferred) {
        callbacks->worker_started(world, event->receiver_index);
    }

    /* A live local operation owns the radio through its safe boundary. */
    if (!event->receiver_preempted && !event->receiver_policy_deferred &&
        !mesh_runtime_radio_safe(&receiver->runtime, event->start_us)) {
        event->receiver_preempted = true;
    }

    ret = mesh_event_plan_channel9(&connection->timing_a,
                                   &requirements,
                                   mesh_sim_time_ms(world->now_us),
                                   &plan_a);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_event_plan_channel9(&connection->timing_b,
                                   &requirements,
                                   mesh_sim_time_ms(world->now_us),
                                   &plan_b);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    if ((plan_a.action != MESH_EVENT_PLAN_START &&
         plan_a.action != MESH_EVENT_PLAN_CLIP) ||
        (plan_b.action != MESH_EVENT_PLAN_START &&
         plan_b.action != MESH_EVENT_PLAN_CLIP) ||
        plan_a.start_ms != plan_b.start_ms || plan_a.end_ms != plan_b.end_ms ||
        (uint64_t)plan_a.start_ms * 1000u != event->start_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_CONNECTION_PLAN);
    }
    event->end_us = (uint64_t)plan_a.end_ms * 1000u;
    if (event->start_us > UINT64_MAX - world->channel9_tx_offset_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    tx_start_us = event->start_us + world->channel9_tx_offset_us;
    if (tx_start_us >= event->end_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_DEADLINE);
    }
    if (!event->receiver_preempted && !event->receiver_policy_deferred) {
        ret = mesh_sim_radio_schedule_channel9_runtime_rx(world,
                                           event->receiver_index,
                                           event->end_us,
                                           tx_start_us,
                                           connection_event_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    } else {
        ret = mesh_sim_trace_add(world,
                             event->start_us,
                             receiver->id,
                             sender->id,
                             MESH_SIM_TRANSITION_CONNECTION_PREEMPTED,
                             0u,
                             event->connection_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    if (!event->sender_policy_deferred) {
        ret = tx_ready(world,
                       event,
                       &requirements,
                       &plan_a,
                       tx_start_us,
                       &tx_result);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    } else {
        ret = mesh_sim_trace_add(world,
                                 event->start_us,
                                 sender->id,
                                 receiver->id,
                                 MESH_SIM_TRANSITION_CONNECTION_PREEMPTED,
                                 0u,
                                 event->connection_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    event->had_packet = tx_result.had_packet;
    if (event->had_packet) {
        callbacks->worker_started(world, event->sender_index);
    }

    ret = mesh_sim_trace_add(world,
                         event->start_us,
                         sender->id,
                         receiver->id,
                         MESH_SIM_TRANSITION_CONNECTION_EVENT,
                          event->had_packet ? tx_result.msg_type : 0u,
                         event->connection_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_scheduler_schedule(world,
                          SIM_EVENT_CONNECTION_END,
                          event->end_us,
                          connection_event_index);
}

int mesh_sim_connection_process_end(struct mesh_sim_world *world,
                                    uint16_t connection_event_index)
{
    struct mesh_sim_connection_event *event =
        &world->connection_events[connection_event_index];
    struct mesh_sim_connection *connection;
    struct mesh_event_timing *sender_timing;
    struct mesh_event_timing *receiver_timing;
    struct mesh_event_diagnostics *receiver_diagnostics;
    uint32_t event_start_ms = mesh_sim_time_ms(event->start_us);
    int ret;

    if (!connection_event_work_current(world, event)) {
        return MESH_SIM_OK;
    }
    connection = &world->connections[event->connection_index];

    if (event->sender_index == connection->node_a) {
        sender_timing = &connection->timing_a;
        receiver_timing = &connection->timing_b;
        receiver_diagnostics = &connection->diagnostics_b;
    } else {
        sender_timing = &connection->timing_b;
        receiver_timing = &connection->timing_a;
        receiver_diagnostics = &connection->diagnostics_a;
    }
    mesh_event_note_local_tx(sender_timing, event_start_ms);
    if (event->decoded) {
        mesh_event_note_success(receiver_timing, event_start_ms);
    } else {
        mesh_event_note_missed(receiver_timing, receiver_diagnostics);
    }
    connection->completed_events++;
    ret = sync_connection_timing(world, connection);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    return MESH_SIM_OK;
}
