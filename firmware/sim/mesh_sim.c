#include "mesh_sim.h"

#include "report.h"
#include "route.h"

#include <limits.h>
#include <string.h>

enum sim_event_type {
    SIM_EVENT_TX_END = 0,
    SIM_EVENT_TX_EVALUATE = 1,
    SIM_EVENT_RX_END = 2,
    SIM_EVENT_CONNECTION_END = 3,
    SIM_EVENT_CONNECTION_REPAIR_END = 4,
    SIM_EVENT_RELAY_TICK = 5,
    SIM_EVENT_CONNECTION_START = 6,
    SIM_EVENT_CONNECTION_REPAIR_START = 7,
    SIM_EVENT_RX_START = 8,
    SIM_EVENT_TX_START = 9,
    SIM_EVENT_LOW_DUTY_START = 10,
    SIM_EVENT_RUNTIME_BOUNDARY = 11,
    SIM_EVENT_RUNTIME_RADIO_RELEASE = 12,
    SIM_EVENT_WATCHDOG_EXPIRE = 13,
};

static enum dwm3000_timing_phy timing_phy(enum mesh_sim_phy phy)
{
    switch (phy) {
    case MESH_SIM_PHY_CHANNEL5_WAKE:
        return DWM3000_TIMING_PHY_CH5_WAKE;
    case MESH_SIM_PHY_CHANNEL5_RANGE:
        return DWM3000_TIMING_PHY_CH5_RANGE;
    case MESH_SIM_PHY_CHANNEL5_MESH_CONTROL:
        return DWM3000_TIMING_PHY_CH5_MESH_CONTROL;
    case MESH_SIM_PHY_CHANNEL9_MESH:
        return DWM3000_TIMING_PHY_CH9_MESH;
    default:
        return (enum dwm3000_timing_phy)-1;
    }
}

static int sim_fail(struct mesh_sim_world *world, int status)
{
    if (world != NULL && world->last_error == MESH_SIM_OK) {
        world->last_error = status;
    }
    return status;
}

static bool node_index_valid(const struct mesh_sim_world *world, uint8_t node_index)
{
    return world != NULL && node_index < world->role_count;
}

static bool interval_overlaps(uint64_t a_start,
                              uint64_t a_end,
                              uint64_t b_start,
                              uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static uint32_t time_ms(uint64_t time_us)
{
    uint64_t value = time_us / 1000u;

    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static int add_transition(struct mesh_sim_world *world,
                          uint64_t time_us,
                          uint64_t node_id,
                          uint64_t peer_id,
                          enum mesh_sim_transition_kind kind,
                          uint8_t msg_type,
                          uint32_t detail)
{
    struct mesh_sim_transition *transition;

    if (world->transition_count >= MESH_SIM_MAX_TRANSITIONS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    transition = &world->transitions[world->transition_count++];
    *transition = (struct mesh_sim_transition) {
        .time_us = time_us,
        .node_id = node_id,
        .peer_id = peer_id,
        .kind = kind,
        .detail = detail,
        .msg_type = msg_type,
    };
    return MESH_SIM_OK;
}

static int schedule_event(struct mesh_sim_world *world,
                          enum sim_event_type type,
                          uint64_t at_us,
                          uint16_t object_index)
{
    struct mesh_sim_event *event;

    if (world == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    if (at_us < world->now_us) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    if (world->event_count >= MESH_SIM_MAX_EVENTS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    event = &world->events[world->event_count++];
    *event = (struct mesh_sim_event) {
        .time_us = at_us,
        .sequence = world->next_sequence++,
        .object_index = object_index,
        .type = (uint8_t)type,
        .priority = (uint8_t)type,
        .pending = true,
    };
    return MESH_SIM_OK;
}

static int runtime_schedule_cb(enum mesh_runtime_work_kind kind,
                               uint64_t token,
                               uint64_t at_us,
                               void *ctx)
{
    struct mesh_sim_role_instance *node = ctx;

    (void)kind;
    (void)token;
    if (node == NULL || node->world == NULL) {
        return MESH_RUNTIME_ERR_ARG;
    }
    return schedule_event(node->world,
                          SIM_EVENT_RUNTIME_BOUNDARY,
                          at_us,
                          node->node_index);
}

static void runtime_trace_cb(enum mesh_runtime_action_kind action,
                             uint64_t token,
                             uint64_t at_us,
                             void *ctx)
{
    struct mesh_sim_role_instance *node = ctx;
    enum mesh_sim_transition_kind transition;

    if (node == NULL || node->world == NULL) {
        return;
    }
    switch (action) {
    case MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND:
        transition = MESH_SIM_TRANSITION_RUNTIME_GATEWAY_COMMAND;
        break;
    case MESH_RUNTIME_ACTION_START_LOCAL_CLICK:
        transition = MESH_SIM_TRANSITION_RUNTIME_LOCAL_CLICK;
        break;
    case MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT:
        transition = MESH_SIM_TRANSITION_RUNTIME_EVENT_REPAIR;
        break;
    case MESH_RUNTIME_ACTION_RUN_TRANSIT:
        transition = MESH_SIM_TRANSITION_RUNTIME_TRANSIT;
        break;
    default:
        return;
    }
    (void)add_transition(node->world,
                         at_us,
                         node->id,
                         0u,
                         transition,
                         0u,
                         token > UINT32_MAX ? UINT32_MAX : (uint32_t)token);
}

static int next_event_index(const struct mesh_sim_world *world,
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

void mesh_sim_init(struct mesh_sim_world *world, uint32_t seed)
{
    if (world == NULL) {
        return;
    }
    memset(world, 0, sizeof(*world));
    world->rng_state = seed == 0u ? 0x6D2B79F5u : seed;
}

uint32_t mesh_sim_random(struct mesh_sim_world *world)
{
    uint32_t value;

    if (world == NULL) {
        return 0u;
    }
    value = world->rng_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    world->rng_state = value;
    return value;
}

int mesh_sim_watchdog_arm(struct mesh_sim_world *world,
                          uint8_t node_index,
                          uint64_t timeout_us,
                          enum mesh_sim_watchdog_action action)
{
    struct mesh_sim_role_instance *node;
    int ret;

    if (!node_index_valid(world, node_index) || timeout_us == 0u ||
        action > MESH_SIM_WATCHDOG_RESET_ROLE ||
        world->now_us > UINT64_MAX - timeout_us) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    memset(&node->watchdog, 0, sizeof(node->watchdog));
    node->watchdog.timeout_us = timeout_us;
    node->watchdog.last_feed_us = world->now_us;
    node->watchdog.deadline_us = world->now_us + timeout_us;
    node->watchdog.action = action;
    node->watchdog.armed = true;
    ret = add_transition(world, world->now_us, node->id, 0u,
                         MESH_SIM_TRANSITION_WATCHDOG_ARMED, 0u,
                         timeout_us > UINT32_MAX ? UINT32_MAX : (uint32_t)timeout_us);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_event(world, SIM_EVENT_WATCHDOG_EXPIRE,
                          node->watchdog.deadline_us, node_index);
}

int mesh_sim_watchdog_feed(struct mesh_sim_world *world, uint8_t node_index)
{
    struct mesh_sim_role_instance *node;
    int ret;

    if (!node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (!node->watchdog.armed || node->watchdog.expired) {
        return MESH_SIM_OK;
    }
    if (world->now_us > UINT64_MAX - node->watchdog.timeout_us) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    node->watchdog.last_feed_us = world->now_us;
    node->watchdog.deadline_us = world->now_us + node->watchdog.timeout_us;
    node->watchdog.feeds++;
    ret = add_transition(world, world->now_us, node->id, 0u,
                         MESH_SIM_TRANSITION_WATCHDOG_FED, 0u,
                         node->watchdog.feeds);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_event(world, SIM_EVENT_WATCHDOG_EXPIRE,
                          node->watchdog.deadline_us, node_index);
}

const struct mesh_sim_phy_profile *mesh_sim_phy_profile(enum mesh_sim_phy phy)
{
    static struct mesh_sim_phy_profile profiles[4];
    static bool initialized;
    enum dwm3000_timing_phy production_phy = timing_phy(phy);

    if (dwm3000_timing_phy_profile(production_phy) == NULL) {
        return NULL;
    }
    if (!initialized) {
        for (unsigned int i = 0u; i < 4u; i++) {
            enum dwm3000_timing_phy profile_phy = timing_phy((enum mesh_sim_phy)i);

            profiles[i].preamble_us = (uint32_t)dwm3000_timing_rctu_to_us_ceil(
                dwm3000_timing_preamble_rctu(profile_phy));
            profiles[i].sfd_us = (uint16_t)dwm3000_timing_rctu_to_us_ceil(
                dwm3000_timing_sfd_rctu(profile_phy));
            profiles[i].phr_us = (uint16_t)dwm3000_timing_rctu_to_us_ceil(
                (uint64_t)DWM3000_TIMING_PHR_BITS *
                DWM3000_TIMING_850K_CHIPS_PER_BIT_SYMBOL *
                DWM3000_TIMING_RCTU_PER_CHIP);
            profiles[i].bitrate_bps = 850000u;
        }
        initialized = true;
    }
    return &profiles[phy];
}

uint32_t mesh_sim_frame_duration_us(enum mesh_sim_phy phy, size_t frame_len)
{
    uint64_t total;

    if (mesh_sim_phy_profile(phy) == NULL || frame_len == 0u) {
        return 0u;
    }
    total = dwm3000_timing_airtime_us_ceil(timing_phy(phy), frame_len);
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

int mesh_sim_add_role(struct mesh_sim_world *world,
                      enum mesh_sim_role role,
                      uint64_t id,
                      uint64_t gateway_id,
                      uint32_t route_epoch,
                      uint8_t *node_index)
{
    struct mesh_sim_role_instance *node;

    if (world == NULL || node_index == NULL || id == 0u || gateway_id == 0u ||
        role < MESH_SIM_ROLE_CLICKER || role > MESH_SIM_ROLE_GATEWAY) {
        return MESH_SIM_ERR_ARG;
    }
    if (world->role_count >= MESH_SIM_MAX_ROLES) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].id == id) {
            return sim_fail(world, MESH_SIM_ERR_ARG);
        }
    }

    *node_index = (uint8_t)world->role_count;
    node = &world->roles[world->role_count++];
    memset(node, 0, sizeof(*node));
    node->role = role;
    node->radio_state = MESH_SIM_RADIO_SLEEP;
    dwm3000_runtime_init(&node->dwm3000);
    node->id = id;
    node->gateway_id = gateway_id;
    node->world = world;
    node->node_index = *node_index;
    if (role != MESH_SIM_ROLE_CLICKER) {
        mesh_relay_init(&node->relay,
                        role == MESH_SIM_ROLE_GATEWAY ?
                        MESH_RELAY_ROLE_GATEWAY : MESH_RELAY_ROLE_ANCHOR,
                        id,
                        gateway_id,
                        route_epoch);
        node->relay_initialized = true;
        {
            const struct mesh_runtime_ops ops = {
                .schedule = runtime_schedule_cb,
                .trace = runtime_trace_cb,
                .ctx = node,
            };

            mesh_runtime_init(&node->runtime, &node->relay, id, &ops);
        }
    }
    return MESH_SIM_OK;
}

struct mesh_sim_role_instance *mesh_sim_role(struct mesh_sim_world *world,
                                             uint8_t node_index)
{
    if (!node_index_valid(world, node_index)) {
        return NULL;
    }
    return &world->roles[node_index];
}

int mesh_sim_init_clicker_session(struct mesh_sim_world *world,
                                  uint8_t node_index,
                                  const struct uwb_clicker_config *config)
{
    struct mesh_sim_role_instance *node = mesh_sim_role(world, node_index);
    int ret;

    if (node == NULL || node->role != MESH_SIM_ROLE_CLICKER || config == NULL ||
        config->clicker_id != node->id) {
        return MESH_SIM_ERR_ARG;
    }
    ret = uwb_clicker_session_start(&node->clicker_session, config);
    if (ret == PROTO_OK) {
        node->clicker_initialized = true;
    }
    return ret;
}

int mesh_sim_init_anchor_session(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 const struct uwb_anchor_config *config)
{
    struct mesh_sim_role_instance *node = mesh_sim_role(world, node_index);
    int ret;

    if (node == NULL || node->role != MESH_SIM_ROLE_ANCHOR || config == NULL ||
        config->anchor_id != node->id) {
        return MESH_SIM_ERR_ARG;
    }
    ret = uwb_anchor_session_init(&node->anchor_session, config);
    if (ret == PROTO_OK) {
        node->anchor_initialized = true;
    }
    return ret;
}

int mesh_sim_set_link(struct mesh_sim_world *world,
                      uint8_t node_a,
                      uint8_t node_b,
                      uint8_t quality,
                      uint16_t propagation_us)
{
    if (!node_index_valid(world, node_a) || !node_index_valid(world, node_b) ||
        node_a == node_b || quality == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    world->reachable[node_a][node_b] = true;
    world->reachable[node_b][node_a] = true;
    world->link_quality[node_a][node_b] = quality;
    world->link_quality[node_b][node_a] = quality;
    world->propagation_us[node_a][node_b] = propagation_us;
    world->propagation_us[node_b][node_a] = propagation_us;
    world->propagation_rctu[node_a][node_b] =
        dwm3000_timing_us_to_rctu_ceil(propagation_us);
    world->propagation_rctu[node_b][node_a] =
        world->propagation_rctu[node_a][node_b];
    return MESH_SIM_OK;
}

int mesh_sim_install_route(struct mesh_sim_world *world,
                           uint8_t node_index,
                           uint8_t next_hop_index,
                           uint8_t hop_count,
                           uint32_t route_epoch)
{
    struct mesh_sim_role_instance *node;
    struct mesh_sim_role_instance *next_hop;
    struct route_candidate candidate;

    if (!node_index_valid(world, node_index) ||
        !node_index_valid(world, next_hop_index) || hop_count == 0u ||
        !world->reachable[node_index][next_hop_index]) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    next_hop = &world->roles[next_hop_index];
    if (!node->relay_initialized || node->role == MESH_SIM_ROLE_GATEWAY ||
        route_epoch != node->relay.upstream.current_epoch) {
        return MESH_SIM_ERR_ARG;
    }
    candidate = (struct route_candidate) {
        .next_hop_id = next_hop->id,
        .gateway_id = node->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = time_ms(world->now_us),
        .last_success_ms = time_ms(world->now_us),
        .route_cost = route_candidate_cost(hop_count,
                                           world->link_quality[node_index][next_hop_index]),
        .hop_count = hop_count,
        .link_quality = world->link_quality[node_index][next_hop_index],
        .valid = true,
    };
    return route_upsert_candidate(&node->relay.upstream, &candidate);
}

int mesh_sim_install_downlink(struct mesh_sim_world *world,
                              uint8_t node_index,
                              uint64_t target_id,
                              uint8_t next_hop_index,
                              uint8_t hop_count,
                              uint32_t route_epoch)
{
    struct mesh_sim_role_instance *node;
    struct mesh_downlink_entry *slot = NULL;

    if (!node_index_valid(world, node_index) ||
        !node_index_valid(world, next_hop_index) || target_id == 0u ||
        hop_count == 0u || !world->reachable[node_index][next_hop_index]) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (!node->relay_initialized) {
        return MESH_SIM_ERR_ARG;
    }
    for (size_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        if (node->relay.downlinks[i].valid &&
            node->relay.downlinks[i].target_id == target_id) {
            slot = &node->relay.downlinks[i];
            break;
        }
        if (slot == NULL && !node->relay.downlinks[i].valid) {
            slot = &node->relay.downlinks[i];
        }
    }
    if (slot == NULL) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    *slot = (struct mesh_downlink_entry) {
        .target_id = target_id,
        .next_hop_id = world->roles[next_hop_index].id,
        .gateway_id = node->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = time_ms(world->now_us),
        .hop_count = hop_count,
        .quality = world->link_quality[node_index][next_hop_index],
        .valid = true,
    };
    return MESH_SIM_OK;
}

static int queue_index_for_peer(const struct mesh_sim_role_instance *node,
                                uint64_t peer_id);

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
        time_ms(world->now_us));
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
    uint64_t start_us = world->now_us;
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
                interval_overlaps(start_us, end_us,
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
                interval_overlaps(start_us, end_us,
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
    bool a_has_work = queue_index_for_peer(node_a, node_b->id) >= 0 ||
                      (mesh_relay_tx_active(&node_a->relay) &&
                       node_a->relay.pending.next_hop_id == node_b->id);
    bool b_has_work = queue_index_for_peer(node_b, node_a->id) >= 0 ||
                      (mesh_relay_tx_active(&node_b->relay) &&
                       node_b->relay.pending.next_hop_id == node_a->id);

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
    struct mesh_sim_role_instance *node_a = &world->roles[connection->node_a];
    struct mesh_sim_role_instance *node_b = &world->roles[connection->node_b];
    int ret;

    ret = mesh_relay_set_channel9_timing(&node_a->relay,
                                         node_b->id,
                                         &connection->timing_a);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_set_channel9_timing(&node_b->relay,
                                          node_a->id,
                                          &connection->timing_b);
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

    if (!node_index_valid(world, node_a) || !node_index_valid(world, node_b) ||
        params == NULL || connection_index == NULL ||
        !world->reachable[node_a][node_b] ||
        !world->roles[node_a].relay_initialized ||
        !world->roles[node_b].relay_initialized) {
        return MESH_SIM_ERR_ARG;
    }
    if (world->connection_count >= MESH_SIM_MAX_CONNECTIONS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
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
    mesh_event_timing_set_local_first_slot_tx(&connection->timing_a,
                                              node_a_transmits_first);
    mesh_event_timing_set_local_first_slot_tx(&connection->timing_b,
                                              !node_a_transmits_first);
    connection->valid = true;
    return sync_connection_timing(world, connection);
}

static uint8_t skip_unstartable_events(
    struct mesh_event_timing *timing,
    struct mesh_event_diagnostics *diagnostics,
    uint64_t now_us)
{
    uint8_t skipped = mesh_event_skip_elapsed(timing,
                                              time_ms(now_us),
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
    ret = skip_connection_elapsed_events(&timing_a,
                                         &diagnostics_a,
                                         &timing_b,
                                         &diagnostics_b,
                                         world->now_us,
                                         &action->skipped_events);
    if (ret != MESH_SIM_OK) {
        return ret;
    }

    usable_a = mesh_event_timing_usable(&timing_a,
                                        time_ms(world->now_us));
    usable_b = mesh_event_timing_usable(&timing_b,
                                        time_ms(world->now_us));
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
        return sim_fail(world, ret);
    }
    if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    if (action.already_scheduled) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    {
        uint8_t skipped_events;

        ret = skip_connection_elapsed_events(&connection->timing_a,
                                             &connection->diagnostics_a,
                                             &connection->timing_b,
                                             &connection->diagnostics_b,
                                             world->now_us,
                                             &skipped_events);
        if (ret != MESH_SIM_OK || skipped_events != action.skipped_events) {
            return sim_fail(world,
                            ret == MESH_SIM_OK ? MESH_SIM_ERR_CONNECTION_PLAN :
                                                ret);
        }
        if (skipped_events > 0u) {
            ret = sync_connection_timing(world, connection);
            if (ret != PROTO_OK) {
                return sim_fail(world, ret);
            }
            ret = add_transition(
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

        ret = connection_repair_duration_us(
            world,
            connection,
            &duration_us,
            &connection->repair_control_frame_len);
        if (ret != MESH_SIM_OK || duration_us != action.end_us - action.start_us) {
            return sim_fail(world,
                            ret == MESH_SIM_OK ? MESH_SIM_ERR_CONNECTION_PLAN :
                                                ret);
        }
        connection->repair_start_us = action.start_us;
        connection->repair_end_us = action.end_us;
        connection->repair_requester =
            connection_repair_requester(world, connection);
        connection->repair_pending = true;
        ret = schedule_event(world,
                             SIM_EVENT_CONNECTION_REPAIR_START,
                             action.start_us,
                             connection_index);
        if (ret != MESH_SIM_OK) {
            connection->repair_pending = false;
            return ret;
        }
        ret = schedule_event(world,
                             SIM_EVENT_CONNECTION_REPAIR_END,
                             action.end_us,
                             connection_index);
        if (ret != MESH_SIM_OK) {
            connection->repair_pending = false;
        }
        return ret;
    }
    if (world->connection_event_count >= MESH_SIM_MAX_CONNECTION_EVENTS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    event_index = (uint16_t)world->connection_event_count;
    event = &world->connection_events[world->connection_event_count++];
    memset(event, 0, sizeof(*event));
    event->start_us = action.start_us;
    event->end_us = action.end_us;
    event->connection_index = connection_index;
    event->receiver_preempted = receiver_preempted;
    event->valid = true;
    return schedule_event(world,
                          SIM_EVENT_CONNECTION_START,
                          event->start_us,
                          event_index);
}

static uint8_t outbound_priority(const struct mesh_sim_role_instance *node,
                                 const struct mesh_outbound *outbound)
{
    if (outbound->packet.src_id == node->gateway_id &&
        outbound->packet.msg_type == MSG_COMMAND) {
        return 250u;
    }
    if (outbound->packet.src_id == node->id &&
        outbound->packet.msg_type == MSG_CLICK_REPORT) {
        return 240u;
    }
    if (outbound->packet.msg_type == MSG_GATEWAY_ACK) {
        return 225u;
    }
    if (outbound->packet.msg_type == MSG_MESH_HOP_ACK) {
        return 210u;
    }
    if (outbound->packet.src_id == node->id) {
        return 180u;
    }
    return 100u;
}

static int queue_outbound(struct mesh_sim_world *world,
                          uint8_t node_index,
                          const struct mesh_outbound *outbound,
                          bool needs_relay_start)
{
    struct mesh_sim_role_instance *node;
    struct mesh_sim_queued_tx *slot = NULL;

    if (!node_index_valid(world, node_index) || outbound == NULL ||
        outbound->payload_len != outbound->packet.payload_len ||
        outbound->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        outbound->next_hop_id == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (node->tx_queue_count >= MESH_SIM_TX_QUEUE_CAPACITY) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (!node->tx_queue[i].valid) {
            slot = &node->tx_queue[i];
            break;
        }
    }
    if (slot == NULL) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    *slot = (struct mesh_sim_queued_tx) {
        .outbound = *outbound,
        .enqueue_order = world->next_enqueue_order++,
        .priority = outbound_priority(node, outbound),
        .needs_relay_start = needs_relay_start,
        .valid = true,
    };
    node->tx_queue_count++;
    return add_transition(world,
                          world->now_us,
                          node->id,
                          outbound->next_hop_id,
                          MESH_SIM_TRANSITION_PACKET_QUEUED,
                          outbound->packet.msg_type,
                          slot->priority);
}

int mesh_sim_queue_originated(struct mesh_sim_world *world,
                              uint8_t node_index,
                              const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len)
{
    struct mesh_sim_role_instance *node;
    struct mesh_outbound outbound = {0};
    int ret;

    if (!node_index_valid(world, node_index) || packet == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (!node->relay_initialized || packet->src_id != node->id) {
        return MESH_SIM_ERR_ARG;
    }
    outbound.packet = *packet;
    if (payload_len > 0u) {
        memcpy(outbound.payload, payload, payload_len);
    }
    outbound.payload_len = (uint16_t)payload_len;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound.queued_at_ms = time_ms(world->now_us);
    ret = mesh_relay_select_next_hop(&node->relay,
                                     packet->dst_id,
                                     &outbound.next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return queue_outbound(world, node_index, &outbound, true);
}

static int radio_operation_conflicts(const struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     uint64_t start_us,
                                     uint64_t end_us)
{
    for (size_t i = 0u; i < world->rx_window_count; i++) {
        const struct mesh_sim_rx_window *window = &world->rx_windows[i];

        if (window->valid && window->node_index == node_index &&
            interval_overlaps(start_us, end_us, window->start_us, window->end_us)) {
            return MESH_SIM_ERR_RADIO_CONFLICT;
        }
    }
    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &world->transmissions[i];

        if (tx->valid && tx->node_index == node_index &&
            interval_overlaps(start_us, end_us, tx->start_us, tx->end_us)) {
            return MESH_SIM_ERR_RADIO_CONFLICT;
        }
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_rx(struct mesh_sim_world *world,
                         uint8_t node_index,
                         uint64_t start_us,
                         uint64_t end_us,
                         uint8_t channel,
                         enum mesh_sim_phy phy,
                         uint16_t *window_index)
{
    struct mesh_sim_rx_window *window;
    uint16_t index;
    int ret;

    if (!node_index_valid(world, node_index) || end_us <= start_us ||
        start_us < world->now_us || mesh_sim_phy_profile(phy) == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = radio_operation_conflicts(world, node_index, start_us, end_us);
    if (ret != MESH_SIM_OK) {
        return sim_fail(world, ret);
    }
    if (world->rx_window_count >= MESH_SIM_MAX_RX_WINDOWS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    index = (uint16_t)world->rx_window_count;
    window = &world->rx_windows[world->rx_window_count++];
    *window = (struct mesh_sim_rx_window) {
        .start_us = start_us,
        .end_us = end_us,
        .start_rctu = dwm3000_timing_us_to_rctu_floor(start_us),
        .end_rctu = dwm3000_timing_us_to_rctu_floor(end_us),
        .initial_end_rctu = dwm3000_timing_us_to_rctu_floor(end_us),
        .node_index = node_index,
        .channel = channel,
        .phy = phy,
        .continuous_operation = true,
        .valid = true,
    };
    if (window_index != NULL) {
        *window_index = index;
    }
    ret = schedule_event(world, SIM_EVENT_RX_START, start_us, index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_event(world, SIM_EVENT_RX_END, end_us, index);
}

int mesh_sim_schedule_rx_extend_on_activity(struct mesh_sim_world *world,
                                            uint8_t node_index,
                                            uint64_t start_us,
                                            uint64_t acquisition_end_us,
                                            uint32_t completion_us,
                                            uint8_t channel,
                                            enum mesh_sim_phy phy,
                                            uint16_t *window_index)
{
    uint16_t index;
    int ret;

    if (completion_us == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_sim_schedule_rx(world,
                               node_index,
                               start_us,
                               acquisition_end_us,
                               channel,
                               phy,
                               &index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->rx_windows[index].extend_on_activity = true;
    world->rx_windows[index].activity_completion_us = completion_us;
    if (window_index != NULL) {
        *window_index = index;
    }
    return MESH_SIM_OK;
}

int mesh_sim_start_anchor_low_duty(struct mesh_sim_world *world,
                                   uint8_t node_index,
                                   uint64_t work_start_us)
{
    if (!node_index_valid(world, node_index) ||
        world->roles[node_index].role != MESH_SIM_ROLE_ANCHOR ||
        work_start_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return schedule_event(world,
                          SIM_EVENT_LOW_DUTY_START,
                          work_start_us,
                          node_index);
}

int mesh_sim_runtime_submit(struct mesh_sim_world *world,
                            uint8_t node_index,
                            enum mesh_runtime_work_kind kind,
                            uint64_t token,
                            uint64_t ready_us)
{
    if (!node_index_valid(world, node_index) ||
        !world->roles[node_index].relay_initialized ||
        ready_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_runtime_submit(&world->roles[node_index].runtime,
                               kind,
                               token,
                               ready_us);
}

int mesh_sim_runtime_reserve_transit(struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     const struct mesh_outbound *outbound,
                                     uint64_t ready_us)
{
    if (!node_index_valid(world, node_index) || outbound == NULL ||
        !world->roles[node_index].relay_initialized ||
        ready_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_runtime_reserve_transit(&world->roles[node_index].runtime,
                                        outbound,
                                        ready_us);
}

int mesh_sim_runtime_claim_radio(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 enum mesh_runtime_radio_owner owner,
                                 uint64_t start_us,
                                 uint64_t end_us)
{
    int ret;

    if (!node_index_valid(world, node_index) || start_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_runtime_claim_radio(&world->roles[node_index].runtime,
                                   owner,
                                   start_us,
                                   end_us);
    if (ret != MESH_RUNTIME_OK) {
        return ret;
    }
    return schedule_event(world,
                          SIM_EVENT_RUNTIME_RADIO_RELEASE,
                          end_us,
                          node_index);
}

int mesh_sim_runtime_set_action_duration(
    struct mesh_sim_world *world,
    uint8_t node_index,
    enum mesh_runtime_work_kind kind,
    uint32_t duration_us)
{
    if (!node_index_valid(world, node_index) ||
        kind < MESH_RUNTIME_WORK_GATEWAY_COMMAND ||
        kind > MESH_RUNTIME_WORK_TRANSIT) {
        return MESH_SIM_ERR_ARG;
    }
    world->roles[node_index].runtime_action_duration_us[kind] = duration_us;
    return MESH_SIM_OK;
}

static enum mesh_runtime_radio_owner runtime_action_owner(
    enum mesh_runtime_action_kind action)
{
    switch (action) {
    case MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND:
        return MESH_RUNTIME_RADIO_GATEWAY_COMMAND;
    case MESH_RUNTIME_ACTION_START_LOCAL_CLICK:
        return MESH_RUNTIME_RADIO_DS_TWR;
    case MESH_RUNTIME_ACTION_RUN_TRANSIT:
        return MESH_RUNTIME_RADIO_TRANSIT;
    case MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT:
        return MESH_RUNTIME_RADIO_LOW_DUTY_SCAN;
    default:
        return MESH_RUNTIME_RADIO_NONE;
    }
}

static enum mesh_runtime_work_kind runtime_action_work_kind(
    enum mesh_runtime_action_kind action)
{
    switch (action) {
    case MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND:
        return MESH_RUNTIME_WORK_GATEWAY_COMMAND;
    case MESH_RUNTIME_ACTION_START_LOCAL_CLICK:
        return MESH_RUNTIME_WORK_LOCAL_CLICK;
    case MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT:
        return MESH_RUNTIME_WORK_EVENT_REPAIR;
    case MESH_RUNTIME_ACTION_RUN_TRANSIT:
    default:
        return MESH_RUNTIME_WORK_TRANSIT;
    }
}

static int process_runtime_boundary(struct mesh_sim_world *world,
                                    uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_runtime_action action;
    uint32_t duration_us;
    int ret;

    ret = mesh_runtime_run_boundary(&node->runtime, world->now_us, &action);
    if (ret != MESH_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    if (action.kind == MESH_RUNTIME_ACTION_WAIT_SAFE_BOUNDARY) {
        return schedule_event(world,
                              SIM_EVENT_RUNTIME_BOUNDARY,
                              action.runnable_at_us,
                              node_index);
    }
    if (action.kind == MESH_RUNTIME_ACTION_NONE) {
        return MESH_SIM_OK;
    }
    duration_us = node->runtime_action_duration_us[
        runtime_action_work_kind(action.kind)];
    if (duration_us == 0u) {
        return schedule_event(world,
                              SIM_EVENT_RUNTIME_BOUNDARY,
                              world->now_us,
                              node_index);
    }
    ret = mesh_runtime_claim_radio(&node->runtime,
                                   runtime_action_owner(action.kind),
                                   world->now_us,
                                   world->now_us + duration_us);
    if (ret != MESH_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    return schedule_event(world,
                          SIM_EVENT_RUNTIME_RADIO_RELEASE,
                          world->now_us + duration_us,
                          node_index);
}

static int process_anchor_low_duty_start(struct mesh_sim_world *world,
                                         uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct dwm3000_runtime_interval prepare;
    struct dwm3000_runtime_interval rx;
    uint32_t arm_us = dwm3000_runtime_spi_transfer_us(
        DWM3000_RUNTIME_SPI_FAST,
        DWM3000_RUNTIME_RX_ARM_TRANSFER_BYTES);
    uint16_t window_index;
    uint64_t rx_end_us;
    int ret;

    ret = dwm3000_runtime_prepare_phy(&node->dwm3000,
                                      DWM3000_TIMING_PHY_CH5_WAKE,
                                      world->now_us,
                                      &prepare);
    if (ret != DWM3000_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    if (prepare.end_us > UINT64_MAX - arm_us -
        MESH_RADIO_ANCHOR_SCAN_RX_US) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    rx_end_us = prepare.end_us + arm_us + MESH_RADIO_ANCHOR_SCAN_RX_US;
    ret = dwm3000_runtime_arm_rx(&node->dwm3000,
                                 prepare.end_us,
                                 rx_end_us,
                                 &rx);
    if (ret != DWM3000_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    ret = mesh_sim_schedule_rx_extend_on_activity(
        world,
        node_index,
        rx.start_us,
        rx.end_us,
        MESH_RADIO_ACTIVITY_COMPLETION_US,
        UWB_CHANNEL_WAKE_CONTACT,
        MESH_SIM_PHY_CHANNEL5_WAKE,
        &window_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->rx_windows[window_index].periodic_low_duty = true;
    return add_transition(world,
                          prepare.end_us,
                          node->id,
                          0u,
                          MESH_SIM_TRANSITION_RADIO_RECONFIGURED,
                          0u,
                          (uint32_t)(prepare.end_us - prepare.start_us));
}

static uint16_t max_propagation_for_tx(const struct mesh_sim_world *world,
                                       uint8_t node_index)
{
    uint16_t maximum = 0u;

    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->reachable[node_index][i] &&
            world->propagation_us[node_index][i] > maximum) {
            maximum = world->propagation_us[node_index][i];
        }
    }
    return maximum;
}

int mesh_sim_schedule_raw_tx(struct mesh_sim_world *world,
                             uint8_t node_index,
                             uint64_t start_us,
                             uint8_t channel,
                             enum mesh_sim_phy phy,
                             const uint8_t *frame,
                             size_t frame_len,
                             bool protocol_frame,
                             uint16_t *transmission_index)
{
    struct mesh_sim_transmission *tx;
    uint32_t duration_us;
    uint16_t index;
    int ret;

    if (!node_index_valid(world, node_index) || frame == NULL || frame_len == 0u ||
        frame_len > PACKET_EXT_MAX_LEN || start_us < world->now_us ||
        mesh_sim_phy_profile(phy) == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    duration_us = mesh_sim_frame_duration_us(phy, frame_len);
    if (duration_us == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    ret = radio_operation_conflicts(world,
                                    node_index,
                                    start_us,
                                    start_us + duration_us);
    if (ret != MESH_SIM_OK) {
        return sim_fail(world, ret);
    }
    if (world->transmission_count >= MESH_SIM_MAX_TRANSMISSIONS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    index = (uint16_t)world->transmission_count;
    tx = &world->transmissions[world->transmission_count++];
    memset(tx, 0, sizeof(*tx));
    tx->start_us = start_us;
    tx->start_rctu = dwm3000_timing_us_to_rctu_floor(start_us);
    tx->rmarker_rctu = tx->start_rctu +
                       dwm3000_timing_shr_rctu(timing_phy(phy));
    tx->end_rctu = tx->start_rctu +
                   dwm3000_timing_airtime_rctu(timing_phy(phy), frame_len);
    tx->end_us = dwm3000_timing_rctu_to_us_ceil(tx->end_rctu);
    tx->node_index = node_index;
    tx->channel = channel;
    tx->phy = phy;
    tx->frame_len = (uint16_t)frame_len;
    tx->connection_event_index = UINT16_MAX;
    tx->protocol_frame = protocol_frame;
    tx->valid = true;
    memcpy(tx->frame, frame, frame_len);
    if (transmission_index != NULL) {
        *transmission_index = index;
    }
    ret = schedule_event(world, SIM_EVENT_TX_START, tx->start_us, index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = schedule_event(world, SIM_EVENT_TX_END, tx->end_us, index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_event(world,
                          SIM_EVENT_TX_EVALUATE,
                          tx->end_us + max_propagation_for_tx(world, node_index),
                          index);
}

int mesh_sim_schedule_packet_tx(struct mesh_sim_world *world,
                                uint8_t node_index,
                                uint64_t start_us,
                                uint8_t channel,
                                enum mesh_sim_phy phy,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint16_t *transmission_index)
{
    uint8_t frame[PACKET_EXT_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (packet == NULL || packet->payload_len != payload_len ||
        (payload_len > 0u && payload == NULL)) {
        return MESH_SIM_ERR_ARG;
    }
    ret = proto_packet_encode(packet,
                              payload,
                              frame,
                              sizeof(frame),
                              &frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_sim_schedule_raw_tx(world,
                                    node_index,
                                    start_us,
                                    channel,
                                    phy,
                                    frame,
                                    frame_len,
                                    true,
                                    transmission_index);
}

int mesh_sim_outbound_radio(const struct mesh_outbound *outbound,
                            uint8_t *channel,
                            enum mesh_sim_phy *phy)
{
    if (outbound == NULL || channel == NULL || phy == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    if (outbound->radio_channel == UWB_CHANNEL_WAKE_CONTACT) {
        *channel = UWB_CHANNEL_WAKE_CONTACT;
        *phy = MESH_SIM_PHY_CHANNEL5_MESH_CONTROL;
        return MESH_SIM_OK;
    }
    if (outbound->radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        *channel = UWB_CHANNEL_MESH_PAYLOAD;
        *phy = MESH_SIM_PHY_CHANNEL9_MESH;
        return MESH_SIM_OK;
    }
    return MESH_SIM_ERR_ARG;
}

static int schedule_outbound_tx_on_radio(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t start_us,
    const struct mesh_outbound *outbound,
    uint8_t channel,
    enum mesh_sim_phy phy,
    uint16_t connection_event_index,
    uint16_t *scheduled_transmission_index)
{
    uint16_t transmission_index;
    int ret;

    ret = mesh_sim_schedule_packet_tx(world,
                                      node_index,
                                      start_us,
                                      channel,
                                      phy,
                                      &outbound->packet,
                                      outbound->payload,
                                      outbound->payload_len,
                                      &transmission_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->transmissions[transmission_index].has_outbound = true;
    world->transmissions[transmission_index].outbound = *outbound;
    world->transmissions[transmission_index].connection_event_index =
        connection_event_index;
    if (scheduled_transmission_index != NULL) {
        *scheduled_transmission_index = transmission_index;
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_outbound_tx(struct mesh_sim_world *world,
                                  uint8_t node_index,
                                  uint64_t start_us,
                                  const struct mesh_outbound *outbound,
                                  uint16_t *transmission_index)
{
    enum mesh_sim_phy phy;
    uint8_t channel;
    int ret;

    ret = mesh_sim_outbound_radio(outbound, &channel, &phy);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_outbound_tx_on_radio(world,
                                         node_index,
                                         start_us,
                                         outbound,
                                         channel,
                                         phy,
                                         UINT16_MAX,
                                         transmission_index);
}

int mesh_sim_override_next_relay_random(struct mesh_sim_world *world,
                                        uint8_t node_index,
                                        uint32_t random_value)
{
    struct mesh_sim_role_instance *node;

    if (!node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    node->next_relay_random = random_value;
    node->next_relay_random_valid = true;
    return MESH_SIM_OK;
}

static int queue_index_for_peer(const struct mesh_sim_role_instance *node,
                                uint64_t peer_id)
{
    size_t best = SIZE_MAX;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *entry = &node->tx_queue[i];

        if (!entry->valid || entry->outbound.next_hop_id != peer_id) {
            continue;
        }
        if (best == SIZE_MAX || entry->priority > node->tx_queue[best].priority ||
            (entry->priority == node->tx_queue[best].priority &&
             entry->enqueue_order < node->tx_queue[best].enqueue_order)) {
            best = i;
        }
    }
    return best == SIZE_MAX ? -1 : (int)best;
}

static void remove_queue_entry(struct mesh_sim_role_instance *node, size_t index)
{
    memset(&node->tx_queue[index], 0, sizeof(node->tx_queue[index]));
    if (node->tx_queue_count > 0u) {
        node->tx_queue_count--;
    }
}

static int process_connection_repair_start(struct mesh_sim_world *world,
                                           uint16_t connection_index)
{
    struct mesh_sim_connection *connection;
    struct mesh_sim_role_instance *node_a;
    struct mesh_sim_role_instance *node_b;
    uint64_t requester_id;
    uint64_t peer_id;
    uint64_t duration_us;
    uint32_t detail;
    int ret;

    if (connection_index >= world->connection_count) {
        return sim_fail(world, MESH_SIM_ERR_ARG);
    }
    connection = &world->connections[connection_index];
    if (!connection->valid || !connection->repair_pending ||
        connection->repair_start_us != world->now_us ||
        connection->repair_end_us <= world->now_us ||
        !world->reachable[connection->node_a][connection->node_b]) {
        return sim_fail(world, MESH_SIM_ERR_ROUTE_REQUIRED);
    }
    node_a = &world->roles[connection->node_a];
    node_b = &world->roles[connection->node_b];
    if (!mesh_runtime_radio_safe(&node_a->runtime, world->now_us) ||
        !mesh_runtime_radio_safe(&node_b->runtime, world->now_us) ||
        (node_a->radio_state != MESH_SIM_RADIO_SLEEP &&
         node_a->radio_state != MESH_SIM_RADIO_IDLE) ||
        (node_b->radio_state != MESH_SIM_RADIO_SLEEP &&
         node_b->radio_state != MESH_SIM_RADIO_IDLE)) {
        return sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
    }

    ret = mesh_runtime_claim_radio(&node_a->runtime,
                                   MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                   world->now_us,
                                   connection->repair_end_us);
    if (ret != MESH_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    ret = mesh_runtime_claim_radio(&node_b->runtime,
                                   MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                   world->now_us,
                                   connection->repair_end_us);
    if (ret != MESH_RUNTIME_OK) {
        node_a->runtime.radio_owner = MESH_RUNTIME_RADIO_NONE;
        node_a->runtime.radio_busy_until_us = world->now_us;
        return sim_fail(world, ret);
    }
    node_a->radio_state = MESH_SIM_RADIO_IDLE;
    node_b->radio_state = MESH_SIM_RADIO_IDLE;
    requester_id = world->roles[connection->repair_requester].id;
    peer_id = connection->repair_requester == connection->node_a ?
              node_b->id : node_a->id;
    duration_us = connection->repair_end_us - connection->repair_start_us;
    detail = ((uint32_t)connection->repair_control_frame_len << 16) |
             (uint32_t)(duration_us / 1000u +
                        (duration_us % 1000u == 0u ? 0u : 1u));
    return add_transition(world,
                          world->now_us,
                          requester_id,
                          peer_id,
                          MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED,
                          MSG_MESH_EVENT_PROPOSE,
                          detail);
}

static int process_connection_repair_end(struct mesh_sim_world *world,
                                         uint16_t connection_index)
{
    struct mesh_sim_connection *connection;
    struct mesh_sim_role_instance *node_a;
    struct mesh_sim_role_instance *node_b;
    struct mesh_event_params params;
    uint64_t first_event_ms;
    uint64_t requester_id;
    uint64_t peer_id;
    bool node_a_first;
    int ret;

    if (connection_index >= world->connection_count) {
        return sim_fail(world, MESH_SIM_ERR_ARG);
    }
    connection = &world->connections[connection_index];
    if (!connection->valid || !connection->repair_pending ||
        connection->repair_end_us != world->now_us ||
        !world->reachable[connection->node_a][connection->node_b]) {
        return sim_fail(world, MESH_SIM_ERR_ROUTE_REQUIRED);
    }
    node_a = &world->roles[connection->node_a];
    node_b = &world->roles[connection->node_b];
    if (node_a->runtime.radio_owner != MESH_RUNTIME_RADIO_CHANNEL9_EVENT ||
        node_b->runtime.radio_owner != MESH_RUNTIME_RADIO_CHANNEL9_EVENT) {
        return sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
    }
    ret = mesh_runtime_release_radio(&node_a->runtime,
                                     MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                     world->now_us);
    if (ret != MESH_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    ret = mesh_runtime_release_radio(&node_b->runtime,
                                     MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
                                     world->now_us);
    if (ret != MESH_RUNTIME_OK) {
        return sim_fail(world, ret);
    }
    node_a->radio_state = MESH_SIM_RADIO_SLEEP;
    node_b->radio_state = MESH_SIM_RADIO_SLEEP;

    first_event_ms = world->now_us / 1000u +
                     (world->now_us % 1000u == 0u ? 0u : 1u);
    if (first_event_ms >
        UINT32_MAX - MESH_RADIO_EVENT_FIRST_DELAY_MS) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    first_event_ms += MESH_RADIO_EVENT_FIRST_DELAY_MS;
    params = (struct mesh_event_params) {
        .event_interval_ms = connection->timing_a.event_interval_ms,
        .event_window_ms = connection->timing_a.event_window_ms,
        .first_event_time_ms = (uint32_t)first_event_ms,
        .guard_ms = connection->timing_a.guard_ms,
        .peer_clock_skew_estimate_ppm =
            connection->timing_a.peer_clock_skew_estimate_ppm,
        .max_missed_events = connection->timing_a.max_missed_events,
        .supervision_timeout_ms =
            connection->timing_a.supervision_timeout_ms,
    };
    ret = mesh_event_timing_negotiate(&connection->timing_a, &params, true);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    ret = mesh_event_timing_negotiate(&connection->timing_b, &params, true);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    node_a_first = connection->repair_requester == connection->node_a;
    mesh_event_timing_set_local_first_slot_tx(&connection->timing_a,
                                              node_a_first);
    mesh_event_timing_set_local_first_slot_tx(&connection->timing_b,
                                              !node_a_first);
    ret = sync_connection_timing(world, connection);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    connection->repair_pending = false;
    connection->completed_repairs++;
    requester_id = world->roles[connection->repair_requester].id;
    peer_id = connection->repair_requester == connection->node_a ?
              node_b->id : node_a->id;
    ret = add_transition(world,
                         world->now_us,
                         requester_id,
                         peer_id,
                         MESH_SIM_TRANSITION_CONNECTION_REPAIRED,
                         MSG_MESH_EVENT_ACCEPT,
                         connection->completed_repairs);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_watchdog_feed(world, connection->node_a);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_watchdog_feed(world, connection->node_b);
}

static int process_connection_start(struct mesh_sim_world *world,
                                    uint16_t connection_event_index)
{
    struct mesh_sim_connection_event *event;
    struct mesh_sim_connection *connection;
    struct mesh_sim_role_instance *sender;
    struct mesh_sim_role_instance *receiver;
    struct mesh_channel5_requirements requirements = {0};
    struct mesh_event_plan plan_a;
    struct mesh_event_plan plan_b;
    struct mesh_event_plan tx_plan;
    struct mesh_outbound outbound;
    bool a_tx;
    bool b_tx;
    int queue_index;
    int ret;

    if (connection_event_index >= world->connection_event_count) {
        return sim_fail(world, MESH_SIM_ERR_ARG);
    }
    event = &world->connection_events[connection_event_index];
    connection = &world->connections[event->connection_index];
    a_tx = mesh_event_timing_local_tx_slot(&connection->timing_a);
    b_tx = mesh_event_timing_local_tx_slot(&connection->timing_b);
    if (a_tx == b_tx) {
        return sim_fail(world, MESH_SIM_ERR_SLOT_DIRECTION);
    }
    event->sender_index = a_tx ? connection->node_a : connection->node_b;
    event->receiver_index = a_tx ? connection->node_b : connection->node_a;
    sender = &world->roles[event->sender_index];
    receiver = &world->roles[event->receiver_index];

    /* A live local operation owns the radio through its safe boundary. */
    if (!event->receiver_preempted &&
        !mesh_runtime_radio_safe(&receiver->runtime, event->start_us)) {
        event->receiver_preempted = true;
    }

    ret = mesh_event_plan_channel9(&connection->timing_a,
                                   &requirements,
                                   time_ms(world->now_us),
                                   &plan_a);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    ret = mesh_event_plan_channel9(&connection->timing_b,
                                   &requirements,
                                   time_ms(world->now_us),
                                   &plan_b);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    if ((plan_a.action != MESH_EVENT_PLAN_START &&
         plan_a.action != MESH_EVENT_PLAN_CLIP) ||
        (plan_b.action != MESH_EVENT_PLAN_START &&
         plan_b.action != MESH_EVENT_PLAN_CLIP) ||
        plan_a.start_ms != plan_b.start_ms || plan_a.end_ms != plan_b.end_ms ||
        (uint64_t)plan_a.start_ms * 1000u != event->start_us) {
        return sim_fail(world, MESH_SIM_ERR_CONNECTION_PLAN);
    }
    event->end_us = (uint64_t)plan_a.end_ms * 1000u;
    if (!event->receiver_preempted) {
        ret = mesh_sim_schedule_rx(world,
                                   event->receiver_index,
                                   event->start_us,
                                   event->end_us,
                                   UWB_CHANNEL_MESH_PAYLOAD,
                                   MESH_SIM_PHY_CHANNEL9_MESH,
                                   NULL);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    } else {
        ret = add_transition(world,
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

    queue_index = queue_index_for_peer(sender, receiver->id);
    if (queue_index >= 0) {
        struct mesh_sim_queued_tx queued = sender->tx_queue[queue_index];
        uint64_t tx_start_us = event->start_us + MESH_SIM_SLOT_TX_OFFSET_US;
        uint32_t duration_us;

        outbound = queued.outbound;
        if (queued.needs_relay_start) {
            ret = mesh_relay_start_channel9_tx(&sender->relay,
                                               &queued.outbound.packet,
                                               queued.outbound.payload,
                                               queued.outbound.payload_len,
                                               &requirements,
                                               time_ms(event->start_us),
                                               &tx_plan,
                                               &outbound);
            if (ret != PROTO_OK) {
                return sim_fail(world, ret);
            }
            if ((tx_plan.action != MESH_EVENT_PLAN_START &&
                 tx_plan.action != MESH_EVENT_PLAN_CLIP) ||
                tx_plan.start_ms != plan_a.start_ms ||
                tx_plan.end_ms != plan_a.end_ms) {
                return sim_fail(world, MESH_SIM_ERR_SENDER_PLAN);
            }
        }
        duration_us = mesh_sim_frame_duration_us(
            MESH_SIM_PHY_CHANNEL9_MESH,
            proto_packet_encoded_len(outbound.payload_len));
        if (duration_us == 0u || tx_start_us + duration_us > event->end_us) {
            return sim_fail(world, MESH_SIM_ERR_FRAME_TOO_LONG);
        }
        ret = schedule_outbound_tx_on_radio(world,
                                            event->sender_index,
                                            tx_start_us,
                                            &outbound,
                                            UWB_CHANNEL_MESH_PAYLOAD,
                                            MESH_SIM_PHY_CHANNEL9_MESH,
                                            connection_event_index,
                                            NULL);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        event->had_packet = true;
        remove_queue_entry(sender, (size_t)queue_index);
    }

    ret = add_transition(world,
                         event->start_us,
                         sender->id,
                         receiver->id,
                         MESH_SIM_TRANSITION_CONNECTION_EVENT,
                         event->had_packet ?
                         (queue_index >= 0 ? outbound.packet.msg_type : 0u) : 0u,
                         event->connection_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_event(world,
                          SIM_EVENT_CONNECTION_END,
                          event->end_us,
                          connection_event_index);
}

static int process_connection_end(struct mesh_sim_world *world,
                                  uint16_t connection_event_index)
{
    struct mesh_sim_connection_event *event =
        &world->connection_events[connection_event_index];
    struct mesh_sim_connection *connection =
        &world->connections[event->connection_index];
    struct mesh_event_timing *sender_timing;
    struct mesh_event_timing *receiver_timing;
    struct mesh_event_diagnostics *receiver_diagnostics;
    uint32_t event_start_ms = time_ms(event->start_us);
    int ret;

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
        return sim_fail(world, ret);
    }
    ret = mesh_sim_watchdog_feed(world, event->sender_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_watchdog_feed(world, event->receiver_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return MESH_SIM_OK;
}

static int append_delivery(struct mesh_sim_world *world,
                           uint8_t node_index,
                           uint64_t previous_hop_id,
                           const struct proto_packet *packet,
                           const uint8_t *payload,
                           size_t payload_len)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_sim_delivery *delivery;

    if (node->delivery_count >= MESH_SIM_DELIVERY_CAPACITY) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    delivery = &node->deliveries[node->delivery_count++];
    memset(delivery, 0, sizeof(*delivery));
    delivery->delivered_at_us = world->now_us;
    delivery->previous_hop_id = previous_hop_id;
    delivery->packet = *packet;
    delivery->payload_len = (uint16_t)payload_len;
    if (payload_len > 0u) {
        memcpy(delivery->payload, payload, payload_len);
    }
    return add_transition(world,
                          world->now_us,
                          node->id,
                          previous_hop_id,
                          MESH_SIM_TRANSITION_PACKET_DELIVERED,
                          packet->msg_type,
                          packet->ttl);
}

static uint64_t relay_action_safe_start_us(
    const struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound)
{
    const struct mesh_sim_role_instance *node = &world->roles[node_index];
    enum mesh_sim_phy phy;
    uint64_t start_us;
    uint32_t duration_us;
    uint8_t channel;
    bool moved;

    if (mesh_sim_outbound_radio(outbound, &channel, &phy) != MESH_SIM_OK) {
        return UINT64_MAX;
    }
    (void)channel;
    duration_us = mesh_sim_frame_duration_us(
        phy, proto_packet_encoded_len(outbound->payload_len));
    if (duration_us == 0u) {
        return UINT64_MAX;
    }
    start_us = (uint64_t)outbound->earliest_tx_ms * 1000u;
    if (start_us < world->now_us) {
        start_us = world->now_us;
    }

    do {
        uint64_t end_us;

        moved = false;
        if (node->runtime.radio_busy_until_us > start_us) {
            start_us = node->runtime.radio_busy_until_us;
            moved = true;
        }
        if (start_us > UINT64_MAX - duration_us) {
            return UINT64_MAX;
        }
        end_us = start_us + duration_us;
        for (size_t i = 0u; i < world->rx_window_count; i++) {
            const struct mesh_sim_rx_window *window = &world->rx_windows[i];

            if (window->valid && window->node_index == node_index &&
                interval_overlaps(start_us, end_us,
                                  window->start_us, window->end_us) &&
                window->end_us > start_us) {
                start_us = window->end_us;
                moved = true;
            }
        }
        for (size_t i = 0u; i < world->transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world->transmissions[i];

            if (tx->valid && tx->node_index == node_index &&
                interval_overlaps(start_us, end_us, tx->start_us, tx->end_us) &&
                tx->end_us > start_us) {
                start_us = tx->end_us;
                moved = true;
            }
        }
    } while (moved);

    return start_us;
}

static int schedule_relay_action_outbound(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound)
{
    uint64_t start_us;
    int ret;

    start_us = relay_action_safe_start_us(world, node_index, outbound);
    if (start_us == UINT64_MAX) {
        return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    ret = mesh_sim_schedule_outbound_tx(world,
                                        node_index,
                                        start_us,
                                        outbound,
                                        NULL);
    if (ret != MESH_SIM_OK) {
        return sim_fail(world, ret);
    }
    return add_transition(world,
                          world->now_us,
                          world->roles[node_index].id,
                          outbound->next_hop_id,
                          MESH_SIM_TRANSITION_PACKET_QUEUED,
                          outbound->packet.msg_type,
                          outbound_priority(&world->roles[node_index], outbound));
}

static int process_relay_actions(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t previous_hop_id,
                                 const struct proto_packet *received_packet,
                                 const uint8_t *received_payload,
                                 size_t received_payload_len,
                                 const struct mesh_relay_result *result)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    uint32_t unsupported;
    int ret;

    if ((result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        node->route_discovery_requests++;
        (void)add_transition(world,
                             world->now_us,
                             node->id,
                             node->gateway_id,
                             MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                             received_packet == NULL ? 0u : received_packet->msg_type,
                             result->status);
        return sim_fail(world, MESH_SIM_ERR_ROUTE_REQUIRED);
    }
    if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
        received_packet != NULL) {
        ret = append_delivery(world,
                              node_index,
                              previous_hop_id,
                              received_packet,
                              received_payload,
                              received_payload_len);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_FORWARD) != 0u) {
        ret = queue_outbound(world, node_index, &result->forward, true);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) != 0u) {
        ret = queue_outbound(world, node_index, &result->gateway_ack, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u) {
        ret = queue_outbound(world, node_index, &result->hop_ack, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) != 0u) {
        ret = schedule_relay_action_outbound(world,
                                             node_index,
                                             &result->route_reply_ack);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) != 0u) {
        ret = schedule_relay_action_outbound(world,
                                             node_index,
                                             &result->route_request);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u) {
        ret = schedule_relay_action_outbound(world,
                                             node_index,
                                             &result->route_reply);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_RETRANSMIT) != 0u) {
        ret = queue_outbound(world, node_index, &result->retransmit, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = add_transition(world,
                             world->now_us,
                             node->id,
                             result->retransmit.next_hop_id,
                             MESH_SIM_TRANSITION_RETRY_READY,
                             result->retransmit.packet.msg_type,
                             result->retransmit.packet.message_age_ms);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) != 0u) {
        ret = add_transition(world,
                             world->now_us,
                             node->id,
                             node->gateway_id,
                             MESH_SIM_TRANSITION_GATEWAY_ACKED,
                             received_packet == NULL ? 0u : received_packet->msg_type,
                             0u);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) != 0u) {
        ret = add_transition(world,
                             world->now_us,
                             node->id,
                             previous_hop_id,
                             MESH_SIM_TRANSITION_HOP_PROGRESS,
                             received_packet == NULL ? 0u : received_packet->msg_type,
                             0u);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    unsupported = result->actions &
                  (MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV |
                   MESH_RELAY_ACTION_SEND_RELAY_BUSY |
                   MESH_RELAY_ACTION_SEND_RESULT_BUSY |
                   MESH_RELAY_ACTION_SEND_RESULT_GRANT);
    if (unsupported != 0u) {
        return sim_fail(world, MESH_SIM_ERR_UNSUPPORTED_ACTION);
    }
    return MESH_SIM_OK;
}

static int dispatch_packet(struct mesh_sim_world *world,
                           uint8_t receiver_index,
                           uint8_t sender_index,
                           const struct proto_packet *packet,
                           const uint8_t *payload,
                           size_t payload_len)
{
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    struct mesh_relay_result result;
    uint32_t random_value;
    int ret;

    if (!receiver->relay_initialized) {
        return MESH_SIM_OK;
    }
    if (receiver->next_relay_random_valid) {
        random_value = receiver->next_relay_random;
        receiver->next_relay_random_valid = false;
    } else {
        random_value = mesh_sim_random(world);
    }
    ret = mesh_relay_handle_rx_with_random(
        &receiver->relay,
        packet,
        payload,
        payload_len,
        world->roles[sender_index].id,
        world->link_quality[sender_index][receiver_index],
        time_ms(world->now_us),
        random_value,
        &result);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    return process_relay_actions(world,
                                 receiver_index,
                                 world->roles[sender_index].id,
                                 packet,
                                 payload,
                                 payload_len,
                                 &result);
}

static bool transmission_collides_at(const struct mesh_sim_world *world,
                                     size_t tx_index,
                                     uint8_t receiver_index,
                                     uint64_t arrival_start,
                                     uint64_t arrival_end,
                                     const struct mesh_sim_rx_window *window)
{
    const struct mesh_sim_transmission *target = &world->transmissions[tx_index];

    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *other = &world->transmissions[i];
        uint64_t other_start;
        uint64_t other_end;

        if (i == tx_index || !other->valid ||
            other->channel != target->channel ||
            !world->reachable[other->node_index][receiver_index]) {
            continue;
        }
        other_start = other->start_rctu +
                      world->propagation_rctu[other->node_index][receiver_index];
        other_end = other->end_rctu +
                    world->propagation_rctu[other->node_index][receiver_index];
        if (interval_overlaps(arrival_start, arrival_end, other_start, other_end) &&
            interval_overlaps(window->start_rctu, window->end_rctu,
                              other_start, other_end)) {
            return true;
        }
    }
    return false;
}

static enum mesh_sim_rx_outcome partial_outcome(
    enum mesh_sim_phy phy,
    uint64_t arrival_start_rctu,
    uint64_t preamble_end_rctu,
    uint64_t sfd_end_rctu,
    const struct mesh_sim_rx_window *window)
{
    uint64_t overlap_start = window->start_rctu > arrival_start_rctu ?
                             window->start_rctu : arrival_start_rctu;
    uint64_t overlap_end = window->end_rctu < preamble_end_rctu ?
                           window->end_rctu : preamble_end_rctu;
    uint64_t preamble_overlap = overlap_end > overlap_start ?
                                overlap_end - overlap_start : 0u;
    uint64_t detect_min = dwm3000_timing_pac_rctu(timing_phy(phy));

    if (preamble_overlap < detect_min) {
        return MESH_SIM_RX_PREAMBLE_ONLY;
    }
    if (window->end_rctu < sfd_end_rctu) {
        return MESH_SIM_RX_SFD_TIMEOUT;
    }
    return MESH_SIM_RX_FRAME_TIMEOUT;
}

static bool rx_extension_conflicts(const struct mesh_sim_world *world,
                                   size_t window_index,
                                   uint64_t old_end_us,
                                   uint64_t new_end_us);

static enum mesh_sim_radio_state post_operation_radio_state(
    const struct mesh_sim_role_instance *node,
    uint64_t now_us)
{
    if (node != NULL && node->runtime.radio_owner != MESH_RUNTIME_RADIO_NONE &&
        now_us < node->runtime.radio_busy_until_us) {
        return MESH_SIM_RADIO_IDLE;
    }
    return MESH_SIM_RADIO_SLEEP;
}

static int hold_channel5_for_wake_claim(
    struct mesh_sim_world *world,
    uint8_t receiver_index,
    struct mesh_sim_rx_window *window,
    const struct uwb_wake_claim_frame *claim)
{
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    uint64_t wake_hold_us;
    uint64_t ownership_us;
    uint64_t wake_hold_end_us;
    uint64_t ownership_end_us;
    size_t window_index;
    int ret;

    if (window == NULL || claim == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    wake_hold_us = (uint64_t)claim->wake_train_ends_in_ms * 1000u;
    ownership_us = (uint64_t)claim->claimed_duration_ms * 1000u;
    if (world->now_us > UINT64_MAX - wake_hold_us ||
        world->now_us > UINT64_MAX - ownership_us) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    wake_hold_end_us = world->now_us + wake_hold_us;
    ownership_end_us = world->now_us + ownership_us;
    if (ownership_end_us < wake_hold_end_us) {
        ownership_end_us = wake_hold_end_us;
    }
    if (ownership_end_us <= world->now_us) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }

    if (receiver->runtime.radio_owner == MESH_RUNTIME_RADIO_DS_TWR) {
        if (ownership_end_us > receiver->runtime.radio_busy_until_us) {
            receiver->runtime.radio_busy_until_us = ownership_end_us;
            ret = schedule_event(world,
                                 SIM_EVENT_RUNTIME_RADIO_RELEASE,
                                 ownership_end_us,
                                 receiver_index);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    } else {
        ret = mesh_sim_runtime_claim_radio(world,
                                           receiver_index,
                                           MESH_RUNTIME_RADIO_DS_TWR,
                                           world->now_us,
                                           ownership_end_us);
        if (ret != MESH_SIM_OK) {
            return sim_fail(world, ret);
        }
    }

    window_index = (size_t)(window - world->rx_windows);
    if (wake_hold_end_us > window->end_us) {
        if (rx_extension_conflicts(world,
                                   window_index,
                                   window->end_us,
                                   wake_hold_end_us)) {
            return sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
        }
        if (window->periodic_low_duty) {
            ret = dwm3000_runtime_extend_rx(&receiver->dwm3000,
                                            wake_hold_end_us);
            if (ret != DWM3000_RUNTIME_OK) {
                return sim_fail(world, ret);
            }
        }
        window->end_us = wake_hold_end_us;
        window->end_rctu = dwm3000_timing_us_to_rctu_ceil(wake_hold_end_us);
        ret = schedule_event(world,
                             SIM_EVENT_RX_END,
                             wake_hold_end_us,
                             (uint16_t)window_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    window->wake_claim_handoff = true;
    receiver->resume_low_duty_after_ds_twr = window->periodic_low_duty;
    return add_transition(world,
                          world->now_us,
                          receiver->id,
                          claim->clicker_id,
                          MESH_SIM_TRANSITION_WAKE_CLAIM_OWNED,
                          claim->attempt_index,
                          claim->claimed_duration_ms);
}

static int append_reception(struct mesh_sim_world *world,
                            size_t tx_index,
                            uint8_t receiver_index,
                            struct mesh_sim_rx_window *window,
                            enum mesh_sim_rx_outcome outcome,
                            uint64_t arrival_start,
                            uint64_t arrival_end)
{
    struct mesh_sim_transmission *tx = &world->transmissions[tx_index];
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    struct mesh_sim_reception *reception;
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    enum mesh_sim_transition_kind transition_kind;
    int ret = PROTO_OK;

    if (world->reception_count >= MESH_SIM_MAX_RECEPTIONS) {
        return sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    reception = &world->receptions[world->reception_count++];
    memset(reception, 0, sizeof(*reception));
    reception->start_rctu = arrival_start;
    reception->end_rctu = arrival_end;
    reception->start_us = dwm3000_timing_rctu_to_us_floor(arrival_start);
    reception->end_us = dwm3000_timing_rctu_to_us_ceil(arrival_end);
    reception->source_id = world->roles[tx->node_index].id;
    reception->receiver_id = receiver->id;
    reception->outcome = outcome;
    reception->phy = tx->phy;
    reception->channel = tx->channel;
    reception->protocol_status = PROTO_ERR_BAD_LENGTH;

    if (outcome == MESH_SIM_RX_DECODED && tx->protocol_frame) {
        ret = proto_packet_decode(tx->frame,
                                  tx->frame_len,
                                  &reception->packet,
                                  &decoded_payload,
                                  &decoded_payload_len);
        reception->protocol_status = ret;
        if (ret != PROTO_OK || decoded_payload_len > sizeof(reception->payload)) {
            reception->outcome = MESH_SIM_RX_DECODE_ERROR;
            outcome = MESH_SIM_RX_DECODE_ERROR;
        } else {
            reception->payload_len = (uint16_t)decoded_payload_len;
            if (decoded_payload_len > 0u) {
                memcpy(reception->payload, decoded_payload, decoded_payload_len);
            }
        }
    } else if (outcome == MESH_SIM_RX_DECODED) {
        reception->protocol_status = PROTO_OK;
        if (receiver->anchor_initialized &&
            tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            struct uwb_wake_claim_frame claim;
            enum uwb_anchor_claim_decision decision;

            ret = uwb_decode_wake_claim(tx->frame, tx->frame_len, &claim);
            if (ret == PROTO_OK) {
                ret = uwb_anchor_accept_wake_claim(&receiver->anchor_session,
                                                   &claim,
                                                   time_ms(world->now_us),
                                                   &decision);
                if (ret == PROTO_OK &&
                    (decision == UWB_ANCHOR_CLAIM_ACCEPTED ||
                     decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY)) {
                    ret = hold_channel5_for_wake_claim(world,
                                                       receiver_index,
                                                       window,
                                                       &claim);
                }
            }
            if (ret != PROTO_OK) {
                reception->protocol_status = ret;
                reception->outcome = MESH_SIM_RX_DECODE_ERROR;
                outcome = MESH_SIM_RX_DECODE_ERROR;
            }
        }
    }

    if (outcome == MESH_SIM_RX_DECODED) {
        receiver->decoded_frames++;
        transition_kind = MESH_SIM_TRANSITION_RX_DECODED;
    } else if (outcome == MESH_SIM_RX_COLLISION) {
        receiver->collision_frames++;
        transition_kind = MESH_SIM_TRANSITION_RX_COLLISION;
        if (receiver->anchor_initialized &&
            tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            uwb_anchor_note_wake_decode_failure(&receiver->anchor_session,
                                                UWB_WAKE_DECODE_CRC_FAILURE);
        }
    } else {
        receiver->partial_frames++;
        transition_kind = MESH_SIM_TRANSITION_RX_PARTIAL;
        if (receiver->anchor_initialized &&
            tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            enum uwb_wake_decode_failure failure = UWB_WAKE_DECODE_FRAME_TIMEOUT;

            if (outcome == MESH_SIM_RX_PREAMBLE_ONLY) {
                failure = UWB_WAKE_DECODE_PREAMBLE_ONLY;
            } else if (outcome == MESH_SIM_RX_SFD_TIMEOUT) {
                failure = UWB_WAKE_DECODE_SFD_TIMEOUT;
            }
            uwb_anchor_note_wake_decode_failure(&receiver->anchor_session, failure);
        }
    }

    ret = add_transition(world,
                         world->now_us,
                         receiver->id,
                         world->roles[tx->node_index].id,
                         transition_kind,
                         reception->packet.msg_type,
                         (uint32_t)outcome);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (outcome == MESH_SIM_RX_DECODED && tx->protocol_frame) {
        if (tx->connection_event_index != UINT16_MAX &&
            tx->connection_event_index < world->connection_event_count &&
            world->connection_events[tx->connection_event_index].receiver_index ==
            receiver_index) {
            world->connection_events[tx->connection_event_index].decoded = true;
        }
        return dispatch_packet(world,
                               receiver_index,
                               tx->node_index,
                               &reception->packet,
                               reception->payload,
                               reception->payload_len);
    }
    (void)window;
    return MESH_SIM_OK;
}

static int evaluate_transmission(struct mesh_sim_world *world, size_t tx_index)
{
    struct mesh_sim_transmission *tx = &world->transmissions[tx_index];
    enum dwm3000_timing_phy production_phy = timing_phy(tx->phy);

    for (uint8_t receiver_index = 0u;
         receiver_index < world->role_count;
         receiver_index++) {
        uint64_t propagation;
        uint64_t arrival_start;
        uint64_t arrival_end;
        uint64_t preamble_end;
        uint64_t sfd_end;

        if (receiver_index == tx->node_index ||
            !world->reachable[tx->node_index][receiver_index]) {
            continue;
        }
        propagation = world->propagation_rctu[tx->node_index][receiver_index];
        arrival_start = tx->start_rctu + propagation;
        arrival_end = tx->end_rctu + propagation;
        preamble_end = arrival_start +
                       dwm3000_timing_preamble_rctu(production_phy);
        sfd_end = preamble_end + dwm3000_timing_sfd_rctu(production_phy);

        for (size_t i = 0u; i < world->rx_window_count; i++) {
            struct mesh_sim_rx_window *window = &world->rx_windows[i];
            uint64_t preamble_overlap_start;
            uint64_t preamble_overlap_end;
            enum mesh_sim_rx_outcome outcome;
            bool complete;
            int ret;

            if (!window->valid || window->node_index != receiver_index ||
                window->channel != tx->channel || window->phy != tx->phy) {
                continue;
            }
            preamble_overlap_start = window->start_rctu > arrival_start ?
                                     window->start_rctu : arrival_start;
            preamble_overlap_end = window->end_rctu < preamble_end ?
                                   window->end_rctu : preamble_end;
            if (preamble_overlap_end <= preamble_overlap_start) {
                continue;
            }
            if (preamble_overlap_end - preamble_overlap_start >=
                dwm3000_timing_pac_rctu(production_phy)) {
                window->preamble_detected = true;
            }
            complete = window->start_rctu <= arrival_start &&
                       window->end_rctu >= arrival_end;
            if (transmission_collides_at(world,
                                         tx_index,
                                         receiver_index,
                                         arrival_start,
                                         arrival_end,
                                         window)) {
                outcome = MESH_SIM_RX_COLLISION;
            } else if (complete) {
                outcome = MESH_SIM_RX_DECODED;
            } else {
                outcome = partial_outcome(tx->phy,
                                          arrival_start,
                                          preamble_end,
                                          sfd_end,
                                          window);
            }
            ret = append_reception(world,
                                   tx_index,
                                   receiver_index,
                                   window,
                                   outcome,
                                   arrival_start,
                                   arrival_end);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
            break;
        }
    }
    return MESH_SIM_OK;
}

static int process_relay_tick(struct mesh_sim_world *world, uint8_t node_index)
{
    struct mesh_sim_role_instance *node;
    struct mesh_relay_result result;
    int ret;

    if (!node_index_valid(world, node_index)) {
        return sim_fail(world, MESH_SIM_ERR_ARG);
    }
    node = &world->roles[node_index];
    if (!node->relay_initialized) {
        return sim_fail(world, MESH_SIM_ERR_ARG);
    }
    ret = mesh_relay_tick_with_random(&node->relay,
                                      time_ms(world->now_us),
                                      mesh_sim_random(world),
                                      &result);
    if (ret != PROTO_OK) {
        return sim_fail(world, ret);
    }
    ret = process_relay_actions(world,
                                node_index,
                                0u,
                                NULL,
                                NULL,
                                0u,
                                &result);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_watchdog_feed(world, node_index);
}

static bool rx_extension_conflicts(const struct mesh_sim_world *world,
                                   size_t window_index,
                                   uint64_t old_end_us,
                                   uint64_t new_end_us)
{
    const struct mesh_sim_rx_window *target = &world->rx_windows[window_index];

    for (size_t i = 0u; i < world->rx_window_count; i++) {
        const struct mesh_sim_rx_window *other = &world->rx_windows[i];

        if (i != window_index && other->valid &&
            other->node_index == target->node_index &&
            interval_overlaps(old_end_us,
                              new_end_us,
                              other->start_us,
                              other->end_us)) {
            return true;
        }
    }
    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *other = &world->transmissions[i];

        if (other->valid && other->node_index == target->node_index &&
            interval_overlaps(old_end_us,
                              new_end_us,
                              other->start_us,
                              other->end_us)) {
            return true;
        }
    }
    return false;
}

static int note_preamble_at_tx_start(struct mesh_sim_world *world,
                                     const struct mesh_sim_transmission *tx)
{
    enum dwm3000_timing_phy production_phy = timing_phy(tx->phy);
    uint64_t preamble_rctu = dwm3000_timing_preamble_rctu(production_phy);
    uint64_t detect_rctu = dwm3000_timing_pac_rctu(production_phy);

    for (size_t i = 0u; i < world->rx_window_count; i++) {
        struct mesh_sim_rx_window *window = &world->rx_windows[i];
        uint64_t arrival_start;
        uint64_t preamble_end;
        uint64_t detection_time;

        if (!window->valid || window->node_index == tx->node_index ||
            !world->reachable[tx->node_index][window->node_index] ||
            window->channel != tx->channel || window->phy != tx->phy) {
            continue;
        }
        arrival_start = tx->start_rctu +
                        world->propagation_rctu[tx->node_index][window->node_index];
        preamble_end = arrival_start + preamble_rctu;
        if (interval_overlaps(window->start_rctu,
                              window->end_rctu,
                              arrival_start,
                              preamble_end)) {
            uint64_t overlap_start = window->start_rctu > arrival_start ?
                                     window->start_rctu : arrival_start;

            detection_time = overlap_start + detect_rctu;
            if (detection_time <= window->end_rctu) {
                window->preamble_detected = true;
                if (window->extend_on_activity) {
                    uint64_t extension_rctu = dwm3000_timing_us_to_rctu_ceil(
                        window->activity_completion_us);
                    uint64_t new_end_rctu = detection_time + extension_rctu;

                    if (new_end_rctu > window->end_rctu) {
                        uint64_t old_end_us = window->end_us;
                        uint64_t new_end_us =
                            dwm3000_timing_rctu_to_us_ceil(new_end_rctu);
                        int ret;

                        if (rx_extension_conflicts(world,
                                                   i,
                                                   old_end_us,
                                                   new_end_us)) {
                            return sim_fail(world,
                                            MESH_SIM_ERR_RADIO_CONFLICT);
                        }
                        if (window->periodic_low_duty) {
                            ret = dwm3000_runtime_extend_rx(
                                &world->roles[window->node_index].dwm3000,
                                new_end_us);
                            if (ret != DWM3000_RUNTIME_OK) {
                                return sim_fail(world, ret);
                            }
                        }
                        window->end_rctu = new_end_rctu;
                        window->end_us = new_end_us;
                        ret = schedule_event(world,
                                             SIM_EVENT_RX_END,
                                             new_end_us,
                                             (uint16_t)i);
                        if (ret != MESH_SIM_OK) {
                            return ret;
                        }
                        ret = add_transition(
                            world,
                            dwm3000_timing_rctu_to_us_ceil(detection_time),
                            world->roles[window->node_index].id,
                            world->roles[tx->node_index].id,
                            MESH_SIM_TRANSITION_RX_WINDOW_EXTENDED,
                            0u,
                            window->activity_completion_us);
                        if (ret != MESH_SIM_OK) {
                            return ret;
                        }
                    }
                }
            }
        }
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_relay_tick(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t at_us)
{
    if (!node_index_valid(world, node_index) || at_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return schedule_event(world, SIM_EVENT_RELAY_TICK, at_us, node_index);
}

static int process_watchdog_expiry(struct mesh_sim_world *world,
                                   uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_sim_watchdog *watchdog = &node->watchdog;
    uint32_t detail;
    int ret;

    if (!watchdog->armed || watchdog->expired ||
        world->now_us != watchdog->deadline_us) {
        return MESH_SIM_OK;
    }
    watchdog->expired = true;
    watchdog->expirations++;
    watchdog->expired_radio_owner = node->runtime.radio_owner;
    watchdog->expired_radio_state = node->radio_state;
    watchdog->expired_pending_state = node->relay.pending.state;
    watchdog->expired_queue_count = node->tx_queue_count > UINT16_MAX ?
                                    UINT16_MAX : (uint16_t)node->tx_queue_count;
    detail = ((uint32_t)watchdog->expired_queue_count << 16) |
             ((uint32_t)watchdog->expired_pending_state << 8) |
             (uint32_t)watchdog->expired_radio_state;
    ret = add_transition(world, world->now_us, node->id,
                         (uint64_t)watchdog->expired_radio_owner,
                         MESH_SIM_TRANSITION_WATCHDOG_EXPIRED, 0u, detail);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    watchdog->armed = false;
    if (watchdog->action == MESH_SIM_WATCHDOG_FAIL) {
        return sim_fail(world, MESH_SIM_ERR_WATCHDOG);
    }

    node->radio_state = MESH_SIM_RADIO_SLEEP;
    node->runtime.radio_owner = MESH_RUNTIME_RADIO_NONE;
    node->runtime.radio_busy_until_us = world->now_us;
    memset(node->tx_queue, 0, sizeof(node->tx_queue));
    node->tx_queue_count = 0u;
    node->relay.pending.state = MESH_RELAY_TX_IDLE;
    watchdog->resets++;
    return add_transition(world, world->now_us, node->id, 0u,
                          MESH_SIM_TRANSITION_WATCHDOG_RESET, 0u,
                          watchdog->resets);
}

static int process_event(struct mesh_sim_world *world,
                         const struct mesh_sim_event *event)
{
    struct mesh_sim_role_instance *node;

    switch ((enum sim_event_type)event->type) {
    case SIM_EVENT_TX_END: {
        struct mesh_sim_transmission *tx =
            &world->transmissions[event->object_index];

        node = &world->roles[tx->node_index];
        if (node->radio_state != MESH_SIM_RADIO_TX) {
            return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
        }
        node->radio_state = post_operation_radio_state(node, world->now_us);
        if (tx->has_outbound) {
            mesh_relay_note_tx_sent(&node->relay,
                                    &tx->outbound,
                                    time_ms(world->now_us));
            if (tx->connection_event_index != UINT16_MAX &&
                tx->connection_event_index < world->connection_event_count) {
                struct mesh_sim_connection_event *connection_event =
                    &world->connection_events[tx->connection_event_index];

                mesh_relay_note_channel9_tx(
                    &node->relay,
                    world->roles[connection_event->receiver_index].id,
                    time_ms(connection_event->start_us));
            }
        }
        return add_transition(world,
                              world->now_us,
                              node->id,
                              tx->has_outbound ? tx->outbound.next_hop_id : 0u,
                              MESH_SIM_TRANSITION_TX_END,
                              tx->has_outbound ? tx->outbound.packet.msg_type : 0u,
                              tx->frame_len);
    }
    case SIM_EVENT_TX_EVALUATE:
        return evaluate_transmission(world, event->object_index);
    case SIM_EVENT_RX_END: {
        struct mesh_sim_rx_window *window = &world->rx_windows[event->object_index];
        struct dwm3000_runtime_interval sleep_interval;
        int ret;

        if (world->now_us < window->end_us) {
            return MESH_SIM_OK;
        }
        node = &world->roles[window->node_index];
        if (node->radio_state != MESH_SIM_RADIO_RX) {
            return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
        }
        node->radio_state = post_operation_radio_state(node, world->now_us);
        if (node->anchor_initialized &&
            window->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
            !window->wake_claim_handoff) {
            uint64_t duration = window->end_us - window->start_us;

            uwb_anchor_note_idle_scan(&node->anchor_session,
                                      0u,
                                      0u,
                                      duration > UINT32_MAX ? UINT32_MAX :
                                      (uint32_t)duration,
                                      window->preamble_detected);
        }
        if (window->periodic_low_duty) {
            ret = dwm3000_runtime_finish_rx(&node->dwm3000, world->now_us);
            if (ret != DWM3000_RUNTIME_OK) {
                return sim_fail(world, ret);
            }
            if (window->wake_claim_handoff) {
                return add_transition(world,
                                      world->now_us,
                                      node->id,
                                      0u,
                                      MESH_SIM_TRANSITION_RX_END,
                                      0u,
                                      event->object_index);
            }
            ret = dwm3000_runtime_enter_retained_sleep(&node->dwm3000,
                                                       world->now_us,
                                                       &sleep_interval);
            if (ret != DWM3000_RUNTIME_OK) {
                return sim_fail(world, ret);
            }
        }
        ret = add_transition(world,
                             world->now_us,
                             node->id,
                             0u,
                             MESH_SIM_TRANSITION_RX_END,
                             0u,
                             event->object_index);
        if (ret != MESH_SIM_OK || !window->periodic_low_duty) {
            return ret;
        }
        ret = schedule_event(
            world,
            SIM_EVENT_LOW_DUTY_START,
            sleep_interval.end_us +
                (uint64_t)MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS * 1000u,
            window->node_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        return add_transition(
            world,
            sleep_interval.end_us,
            node->id,
            0u,
            MESH_SIM_TRANSITION_LOW_DUTY_RESCHEDULED,
            0u,
            MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS);
    }
    case SIM_EVENT_CONNECTION_END:
        return process_connection_end(world, event->object_index);
    case SIM_EVENT_CONNECTION_REPAIR_END:
        return process_connection_repair_end(world, event->object_index);
    case SIM_EVENT_RELAY_TICK:
        return process_relay_tick(world, (uint8_t)event->object_index);
    case SIM_EVENT_CONNECTION_START:
        return process_connection_start(world, event->object_index);
    case SIM_EVENT_CONNECTION_REPAIR_START:
        return process_connection_repair_start(world, event->object_index);
    case SIM_EVENT_RX_START: {
        struct mesh_sim_rx_window *window = &world->rx_windows[event->object_index];

        node = &world->roles[window->node_index];
        if (node->radio_state != MESH_SIM_RADIO_SLEEP &&
            node->radio_state != MESH_SIM_RADIO_IDLE) {
            return sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
        }
        node->radio_state = MESH_SIM_RADIO_RX;
        return add_transition(world,
                              world->now_us,
                              node->id,
                              0u,
                              MESH_SIM_TRANSITION_RX_START,
                              0u,
                              event->object_index);
    }
    case SIM_EVENT_TX_START: {
        struct mesh_sim_transmission *tx =
            &world->transmissions[event->object_index];
        int ret;

        node = &world->roles[tx->node_index];
        if (node->radio_state != MESH_SIM_RADIO_SLEEP &&
            node->radio_state != MESH_SIM_RADIO_IDLE) {
            return sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
        }
        node->radio_state = MESH_SIM_RADIO_TX;
        ret = note_preamble_at_tx_start(world, tx);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        return add_transition(world,
                              world->now_us,
                              node->id,
                              tx->has_outbound ? tx->outbound.next_hop_id : 0u,
                              MESH_SIM_TRANSITION_TX_START,
                              tx->has_outbound ? tx->outbound.packet.msg_type : 0u,
                              tx->frame_len);
    }
    case SIM_EVENT_LOW_DUTY_START:
        return process_anchor_low_duty_start(
            world,
            (uint8_t)event->object_index);
    case SIM_EVENT_RUNTIME_BOUNDARY:
        return process_runtime_boundary(world,
                                        (uint8_t)event->object_index);
    case SIM_EVENT_RUNTIME_RADIO_RELEASE: {
        enum mesh_runtime_radio_owner owner;
        struct dwm3000_runtime_interval sleep_interval;
        int ret;

        node = &world->roles[event->object_index];
        owner = node->runtime.radio_owner;
        if (owner == MESH_RUNTIME_RADIO_NONE ||
            world->now_us < node->runtime.radio_busy_until_us) {
            return MESH_SIM_OK;
        }
        ret = mesh_runtime_release_radio(&node->runtime,
                                         owner,
                                         world->now_us);
        if (ret != MESH_RUNTIME_OK) {
            return sim_fail(world, ret);
        }
        ret = add_transition(world,
                             world->now_us,
                             node->id,
                             0u,
                             MESH_SIM_TRANSITION_RUNTIME_RADIO_RELEASED,
                             0u,
                             (uint32_t)owner);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        if (owner == MESH_RUNTIME_RADIO_DS_TWR &&
            node->resume_low_duty_after_ds_twr) {
            if (node->radio_state != MESH_SIM_RADIO_IDLE &&
                node->radio_state != MESH_SIM_RADIO_SLEEP) {
                return sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
            }
            node->radio_state = MESH_SIM_RADIO_SLEEP;
            ret = dwm3000_runtime_enter_retained_sleep(&node->dwm3000,
                                                       world->now_us,
                                                       &sleep_interval);
            if (ret != DWM3000_RUNTIME_OK) {
                return sim_fail(world, ret);
            }
            node->resume_low_duty_after_ds_twr = false;
            ret = schedule_event(
                world,
                SIM_EVENT_LOW_DUTY_START,
                sleep_interval.end_us +
                    (uint64_t)MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS * 1000u,
                event->object_index);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
            ret = add_transition(world,
                                 world->now_us,
                                 node->id,
                                 0u,
                                 MESH_SIM_TRANSITION_DS_TWR_RELEASED,
                                 0u,
                                 0u);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
        ret = mesh_sim_watchdog_feed(world, (uint8_t)event->object_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        return schedule_event(world,
                              SIM_EVENT_RUNTIME_BOUNDARY,
                              world->now_us,
                              event->object_index);
    }
    case SIM_EVENT_WATCHDOG_EXPIRE:
        return process_watchdog_expiry(world, (uint8_t)event->object_index);
    default:
        return sim_fail(world, MESH_SIM_ERR_ARG);
    }
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
    while ((event_index = next_event_index(world, end_us)) >= 0) {
        struct mesh_sim_event event = world->events[event_index];
        int ret;

        world->events[event_index].pending = false;
        world->now_us = event.time_us;
        ret = process_event(world, &event);
        if (ret != MESH_SIM_OK) {
            return sim_fail(world, ret);
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
    while (true) {
        uint64_t final_time = 0u;
        bool have_pending = false;

        for (size_t i = 0u; i < world->event_count; i++) {
            if (world->events[i].pending) {
                have_pending = true;
                if (world->events[i].time_us > final_time) {
                    final_time = world->events[i].time_us;
                }
            }
        }
        if (!have_pending) {
            return MESH_SIM_OK;
        }
        if (final_time < world->now_us) {
            return sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
        }
        if (mesh_sim_run_until(world, final_time) != MESH_SIM_OK) {
            return world->last_error;
        }
    }
}

size_t mesh_sim_count_transitions(const struct mesh_sim_world *world,
                                  enum mesh_sim_transition_kind kind,
                                  uint64_t node_id)
{
    size_t count = 0u;

    if (world == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < world->transition_count; i++) {
        if (world->transitions[i].kind == kind &&
            (node_id == 0u || world->transitions[i].node_id == node_id)) {
            count++;
        }
    }
    return count;
}

const struct mesh_sim_transition *mesh_sim_find_transition(
    const struct mesh_sim_world *world,
    enum mesh_sim_transition_kind kind,
    uint64_t node_id,
    size_t occurrence)
{
    size_t found = 0u;

    if (world == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < world->transition_count; i++) {
        const struct mesh_sim_transition *transition = &world->transitions[i];

        if (transition->kind != kind ||
            (node_id != 0u && transition->node_id != node_id)) {
            continue;
        }
        if (found++ == occurrence) {
            return transition;
        }
    }
    return NULL;
}
