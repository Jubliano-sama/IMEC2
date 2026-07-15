#include "mesh_sim_internal.h"

#include "report.h"
#include "route.h"

#include <limits.h>
#include <string.h>

int mesh_sim_fail(struct mesh_sim_world *world, int status)
{
    if (world != NULL && world->last_error == MESH_SIM_OK) {
        world->last_error = status;
    }
    return status;
}

bool mesh_sim_node_index_valid(const struct mesh_sim_world *world,
                               uint8_t node_index)
{
    return world != NULL && node_index < world->role_count;
}

uint32_t mesh_sim_time_ms(uint64_t time_us)
{
    uint64_t value = time_us / 1000u;

    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

bool mesh_sim_has_peer_work(const struct mesh_sim_role_instance *node,
                            uint64_t peer_id)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            node->tx_queue[i].outbound.next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

bool mesh_sim_has_active_relay_to(const struct mesh_sim_role_instance *node,
                                  uint64_t peer_id)
{
    return mesh_relay_tx_active(&node->relay) &&
           node->relay.pending.next_hop_id == peer_id;
}

int mesh_sim_publish_connection_timing(
    struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection)
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

void mesh_sim_clear_connection_timing(
    struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection)
{
    mesh_relay_clear_channel9_timing(&world->roles[connection->node_a].relay,
                                     world->roles[connection->node_b].id);
    mesh_relay_clear_channel9_timing(&world->roles[connection->node_b].relay,
                                     world->roles[connection->node_a].id);
}

void mesh_sim_init(struct mesh_sim_world *world, uint32_t seed)
{
    if (world == NULL) {
        return;
    }
    memset(world, 0, sizeof(*world));
    world->rng_state = seed == 0u ? 0x6D2B79F5u : seed;
    world->channel9_tx_offset_us = MESH_SIM_SLOT_TX_OFFSET_US;
}

int mesh_sim_set_channel9_tx_offset_us(struct mesh_sim_world *world,
                                       uint32_t offset_us)
{
    if (world == NULL || offset_us == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    world->channel9_tx_offset_us = offset_us;
    return MESH_SIM_OK;
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

const struct mesh_sim_phy_profile *mesh_sim_phy_profile(enum mesh_sim_phy phy)
{
    static struct mesh_sim_phy_profile profiles[4];
    static bool initialized;
    enum dwm3000_timing_phy production_phy = mesh_sim_radio_timing_phy(phy);

    if (dwm3000_timing_phy_profile(production_phy) == NULL) {
        return NULL;
    }
    if (!initialized) {
        for (unsigned int i = 0u; i < 4u; i++) {
            enum dwm3000_timing_phy profile_phy = mesh_sim_radio_timing_phy((enum mesh_sim_phy)i);

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
    if (role == MESH_SIM_ROLE_GATEWAY && id != gateway_id) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    if (world->role_count >= MESH_SIM_MAX_ROLES) {
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].id == id ||
            (role == MESH_SIM_ROLE_GATEWAY &&
             world->roles[i].role == MESH_SIM_ROLE_GATEWAY)) {
            return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
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
    node->work_epoch = 1u;
    node->tx_queue_capacity = MESH_SIM_TX_QUEUE_CAPACITY;
    if (role != MESH_SIM_ROLE_CLICKER) {
        mesh_relay_init(&node->relay,
                        role == MESH_SIM_ROLE_GATEWAY ?
                        MESH_RELAY_ROLE_GATEWAY : MESH_RELAY_ROLE_ANCHOR,
                        id,
                        gateway_id,
                        route_epoch);
        if (role == MESH_SIM_ROLE_GATEWAY &&
            mesh_relay_attach_gateway_ack_store(
                &node->relay,
                &world->gateway_ack_store) != PROTO_OK) {
            world->role_count--;
            memset(node, 0, sizeof(*node));
            return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
        }
        node->relay_initialized = true;
        {
            const struct mesh_runtime_ops ops = {
                .schedule = mesh_sim_events_runtime_schedule_cb,
                .trace = mesh_sim_events_runtime_trace_cb,
                .ctx = node,
            };

            mesh_runtime_init(&node->runtime, &node->relay, id, &ops);
        }
    }
    return MESH_SIM_OK;
}

int mesh_sim_set_tx_queue_capacity(struct mesh_sim_world *world,
                                   uint8_t node_index,
                                   size_t capacity)
{
    struct mesh_sim_role_instance *node = mesh_sim_role(world, node_index);

    if (node == NULL || capacity == 0u ||
        capacity > MESH_SIM_TX_QUEUE_CAPACITY ||
        node->tx_queue_count > capacity) {
        return MESH_SIM_ERR_ARG;
    }
    node->tx_queue_capacity = capacity;
    return MESH_SIM_OK;
}

struct mesh_sim_role_instance *mesh_sim_role(struct mesh_sim_world *world,
                                             uint8_t node_index)
{
    if (!mesh_sim_node_index_valid(world, node_index)) {
        return NULL;
    }
    return &world->roles[node_index];
}

int mesh_sim_gateway_reject_next_semantic_deliveries(
    struct mesh_sim_world *world,
    uint8_t gateway_index,
    uint16_t count)
{
    struct mesh_sim_role_instance *gateway = mesh_sim_role(world, gateway_index);

    if (gateway == NULL || gateway->role != MESH_SIM_ROLE_GATEWAY) {
        return MESH_SIM_ERR_ARG;
    }
    gateway->gateway_semantic_rejections_remaining = count;
    return MESH_SIM_OK;
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
    if (!mesh_sim_node_index_valid(world, node_a) || !mesh_sim_node_index_valid(world, node_b) ||
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

int mesh_sim_set_route_request_flags(struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     uint8_t flags)
{
    if (!mesh_sim_node_index_valid(world, node_index) ||
        (flags & ~MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u) {
        return MESH_SIM_ERR_ARG;
    }

    world->roles[node_index].route_request_flags = flags;
    return MESH_SIM_OK;
}

int mesh_sim_set_directed_rx_failures(
    struct mesh_sim_world *world,
    uint8_t sender_index,
    uint8_t receiver_index,
    uint16_t failure_count,
    enum mesh_sim_rx_outcome outcome)
{
    if (!mesh_sim_node_index_valid(world, sender_index) ||
        !mesh_sim_node_index_valid(world, receiver_index) ||
        sender_index == receiver_index ||
        outcome == MESH_SIM_RX_DECODED ||
        outcome == MESH_SIM_RX_COLLISION ||
        outcome > MESH_SIM_RX_DECODE_ERROR) {
        return MESH_SIM_ERR_ARG;
    }
    world->directed_rx_failures[sender_index][receiver_index] = failure_count;
    world->directed_rx_failure_outcome[sender_index][receiver_index] = outcome;
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

    if (!mesh_sim_node_index_valid(world, node_index) ||
        !mesh_sim_node_index_valid(world, next_hop_index) || hop_count == 0u ||
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
        .last_seen_ms = mesh_sim_time_ms(world->now_us),
        .last_success_ms = mesh_sim_time_ms(world->now_us),
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

    if (!mesh_sim_node_index_valid(world, node_index) ||
        !mesh_sim_node_index_valid(world, next_hop_index) || target_id == 0u ||
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
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    *slot = (struct mesh_downlink_entry) {
        .target_id = target_id,
        .next_hop_id = world->roles[next_hop_index].id,
        .gateway_id = node->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = mesh_sim_time_ms(world->now_us),
        .hop_count = hop_count,
        .quality = world->link_quality[node_index][next_hop_index],
        .valid = true,
    };
    return MESH_SIM_OK;
}
