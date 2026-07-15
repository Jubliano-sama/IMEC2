#include "mesh_sim_internal.h"
#include "mesh_sim_interval.h"

#include <limits.h>
#include <string.h>

static int schedule_relay_action_outbound(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound);
static int start_route_discovery_for_waiting(struct mesh_sim_world *world,
                                             uint8_t node_index);
int mesh_sim_relay_queue_index_for_peer(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id);
void mesh_sim_relay_remove_queue_entry(struct mesh_sim_role_instance *node,
                                       size_t index);

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

    if (!mesh_sim_node_index_valid(world, node_index) || outbound == NULL ||
        outbound->payload_len != outbound->packet.payload_len ||
        outbound->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        outbound->next_hop_id == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (node->tx_queue_capacity == 0u ||
        node->tx_queue_count >= node->tx_queue_capacity) {
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (!node->tx_queue[i].valid) {
            slot = &node->tx_queue[i];
            break;
        }
    }
    if (slot == NULL) {
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    *slot = (struct mesh_sim_queued_tx) {
        .outbound = *outbound,
        .enqueue_order = world->next_enqueue_order++,
        .priority = outbound_priority(node, outbound),
        .needs_relay_start = needs_relay_start,
        .valid = true,
    };
    node->tx_queue_count++;
    return mesh_sim_trace_add(world,
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

    if (!mesh_sim_node_index_valid(world, node_index) || packet == NULL ||
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
    outbound.queued_at_ms = mesh_sim_time_ms(world->now_us);
    ret = mesh_relay_select_next_hop(&node->relay,
                                     packet->dst_id,
                                     &outbound.next_hop_id);
    if (ret != PROTO_OK) {
        if (node->route_waiting_valid) {
            return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
        }
        node->route_waiting_outbound = outbound;
        node->route_waiting_valid = true;
        ret = start_route_discovery_for_waiting(world, node_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        return MESH_SIM_OK;
    }
    return queue_outbound(world, node_index, &outbound, true);
}

int mesh_sim_override_next_relay_random(struct mesh_sim_world *world,
                                        uint8_t node_index,
                                        uint32_t random_value)
{
    struct mesh_sim_role_instance *node;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    node->next_relay_random = random_value;
    node->next_relay_random_valid = true;
    return MESH_SIM_OK;
}

int mesh_sim_direct_gateway_start_queued_tx(struct mesh_sim_world *world,
                                            uint8_t sender_index,
                                            uint64_t air_start_us,
                                            uint64_t tx_deadline_us,
                                            uint16_t *transmission_index)
{
    struct mesh_sim_role_instance *sender;
    struct mesh_sim_queued_tx queued;
    struct mesh_outbound outbound;
    int queue_index;
    int ret;

    if (world == NULL || !mesh_sim_node_index_valid(world, sender_index) ||
        air_start_us < world->now_us || tx_deadline_us <= air_start_us) {
        return MESH_SIM_ERR_ARG;
    }
    sender = &world->roles[sender_index];
    queue_index = mesh_sim_relay_queue_index_for_peer(sender,
                                                       sender->gateway_id);
    if (queue_index < 0) {
        return MESH_SIM_ERR_ROUTE_REQUIRED;
    }
    queued = sender->tx_queue[queue_index];
    if (queued.needs_relay_start) {
        ret = mesh_relay_start_tx(&sender->relay,
                                  &queued.outbound.packet,
                                  queued.outbound.payload,
                                  queued.outbound.payload_len,
                                  mesh_sim_time_ms(air_start_us),
                                  &outbound);
        if (ret != PROTO_OK) {
            return mesh_sim_fail(world, ret);
        }
    } else {
        if (!mesh_relay_tx_active(&sender->relay) ||
            queued.outbound.packet.msg_type == MSG_GATEWAY_ACK ||
            queued.outbound.packet.src_id != sender->id ||
            queued.outbound.next_hop_id != sender->gateway_id ||
            sender->relay.pending.packet.src_id !=
                queued.outbound.packet.src_id ||
            sender->relay.pending.packet.dst_id !=
                queued.outbound.packet.dst_id ||
            sender->relay.pending.packet.session_id !=
                queued.outbound.packet.session_id ||
            sender->relay.pending.packet.seq != queued.outbound.packet.seq) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        outbound = queued.outbound;
    }
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    sender->relay.pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    ret = mesh_sim_radio_schedule_channel9_runtime_tx(world,
                                                       sender_index,
                                                       air_start_us,
                                                       tx_deadline_us,
                                                       &outbound,
                                                       UINT16_MAX);
    if (ret != MESH_SIM_OK) {
        if (queued.needs_relay_start) {
            mesh_relay_cancel_tx(&sender->relay);
        }
        return ret;
    }
    if (transmission_index != NULL) {
        *transmission_index = (uint16_t)(world->transmission_count - 1u);
    }
    mesh_sim_relay_remove_queue_entry(sender, (size_t)queue_index);
    return MESH_SIM_OK;
}

int mesh_sim_direct_gateway_schedule_ack(struct mesh_sim_world *world,
                                         uint8_t gateway_index,
                                         uint8_t sender_index,
                                         uint64_t air_start_us,
                                         uint64_t ack_window_end_us,
                                         uint16_t *transmission_index)
{
    struct mesh_sim_role_instance *gateway;
    struct mesh_sim_role_instance *sender;
    struct mesh_sim_queued_tx queued;
    struct dwm3000_runtime sender_runtime_before;
    struct dwm3000_runtime gateway_runtime_before;
    size_t event_count_before;
    size_t rx_window_count_before;
    size_t transmission_count_before;
    int queue_index;
    int ret;

    if (world == NULL || !mesh_sim_node_index_valid(world, gateway_index) ||
        !mesh_sim_node_index_valid(world, sender_index) ||
        world->roles[gateway_index].role != MESH_SIM_ROLE_GATEWAY ||
        air_start_us < world->now_us || ack_window_end_us <= air_start_us) {
        return MESH_SIM_ERR_ARG;
    }
    gateway = &world->roles[gateway_index];
    sender = &world->roles[sender_index];
    queue_index = -1;
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *candidate = &gateway->tx_queue[i];

        if (!candidate->valid || candidate->needs_relay_start ||
            candidate->outbound.next_hop_id != sender->id ||
            candidate->outbound.packet.msg_type != MSG_GATEWAY_ACK) {
            continue;
        }
        if (queue_index < 0 || candidate->enqueue_order <
                gateway->tx_queue[queue_index].enqueue_order) {
            queue_index = (int)i;
        }
    }
    if (queue_index < 0) {
        return MESH_SIM_ERR_ROUTE_REQUIRED;
    }
    queued = gateway->tx_queue[queue_index];
    sender_runtime_before = sender->dwm3000;
    gateway_runtime_before = gateway->dwm3000;
    event_count_before = world->event_count;
    rx_window_count_before = world->rx_window_count;
    transmission_count_before = world->transmission_count;
    ret = mesh_sim_radio_schedule_channel9_runtime_rx(
        world, sender_index, ack_window_end_us, air_start_us, UINT16_MAX);
    if (ret != MESH_SIM_OK) {
        sender->dwm3000 = sender_runtime_before;
        gateway->dwm3000 = gateway_runtime_before;
        while (world->event_count > event_count_before) {
            world->event_count--;
            memset(&world->events[world->event_count], 0,
                   sizeof(world->events[world->event_count]));
        }
        while (world->rx_window_count > rx_window_count_before) {
            world->rx_window_count--;
            memset(&world->rx_windows[world->rx_window_count], 0,
                   sizeof(world->rx_windows[world->rx_window_count]));
        }
        while (world->transmission_count > transmission_count_before) {
            world->transmission_count--;
            memset(&world->transmissions[world->transmission_count], 0,
                   sizeof(world->transmissions[world->transmission_count]));
        }
        return ret;
    }
    ret = mesh_sim_radio_schedule_channel9_runtime_tx(world,
                                                       gateway_index,
                                                       air_start_us,
                                                       ack_window_end_us,
                                                       &queued.outbound,
                                                       UINT16_MAX);
    if (ret != MESH_SIM_OK) {
        sender->dwm3000 = sender_runtime_before;
        gateway->dwm3000 = gateway_runtime_before;
        while (world->event_count > event_count_before) {
            world->event_count--;
            memset(&world->events[world->event_count], 0,
                   sizeof(world->events[world->event_count]));
        }
        while (world->rx_window_count > rx_window_count_before) {
            world->rx_window_count--;
            memset(&world->rx_windows[world->rx_window_count], 0,
                   sizeof(world->rx_windows[world->rx_window_count]));
        }
        while (world->transmission_count > transmission_count_before) {
            world->transmission_count--;
            memset(&world->transmissions[world->transmission_count], 0,
                   sizeof(world->transmissions[world->transmission_count]));
        }
        return ret;
    }
    if (transmission_index != NULL) {
        *transmission_index = (uint16_t)(world->transmission_count - 1u);
    }
    mesh_sim_relay_remove_queue_entry(gateway, (size_t)queue_index);
    return MESH_SIM_OK;
}

int mesh_sim_relay_queue_index_for_peer(const struct mesh_sim_role_instance *node,
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

void mesh_sim_relay_remove_queue_entry(struct mesh_sim_role_instance *node,
                                       size_t index)
{
    memset(&node->tx_queue[index], 0, sizeof(node->tx_queue[index]));
    if (node->tx_queue_count > 0u) {
        node->tx_queue_count--;
    }
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
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
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
    return mesh_sim_trace_add(world,
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
                mesh_sim_interval_overlaps(start_us, end_us,
                                           window->start_us, window->end_us) &&
                window->end_us > start_us) {
                start_us = window->end_us;
                moved = true;
            }
        }
        for (size_t i = 0u; i < world->transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world->transmissions[i];

            if (tx->valid && tx->node_index == node_index &&
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

static int schedule_relay_action_outbound(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound)
{
    uint64_t start_us;
    int ret;

    start_us = relay_action_safe_start_us(world, node_index, outbound);
    if (start_us == UINT64_MAX) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    ret = mesh_sim_schedule_outbound_tx(world,
                                        node_index,
                                        start_us,
                                        outbound,
                                        NULL);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    return mesh_sim_trace_add(world,
                          world->now_us,
                          world->roles[node_index].id,
                          outbound->next_hop_id,
                          MESH_SIM_TRANSITION_PACKET_QUEUED,
                          outbound->packet.msg_type,
                          outbound_priority(&world->roles[node_index], outbound));
}

static int start_route_discovery_for_waiting(struct mesh_sim_world *world,
                                             uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_outbound route_request;
    uint32_t now_ms = mesh_sim_time_ms(world->now_us);
    uint64_t retry_at_us;
    int ret;

    if (!node->route_waiting_valid) {
        return MESH_SIM_OK;
    }
    ret = mesh_relay_select_next_hop(
        &node->relay,
        node->route_waiting_outbound.packet.dst_id,
        &node->route_waiting_outbound.next_hop_id);
    if (ret == PROTO_OK) {
        ret = queue_outbound(world,
                             node_index,
                             &node->route_waiting_outbound,
                             true);
        if (ret == MESH_SIM_OK) {
            node->route_waiting_valid = false;
            memset(&node->route_waiting_outbound,
                   0,
                   sizeof(node->route_waiting_outbound));
        }
        return ret;
    }
    ret = mesh_relay_prepare_route_request_with_timing_flags(
        &node->relay,
        node->route_waiting_outbound.packet.dst_id,
        NULL,
        0u,
        node->route_request_flags,
        0u,
        now_ms,
        mesh_sim_random(world),
        &route_request);
    if (ret == PROTO_ERR_BUSY) {
        retry_at_us = (uint64_t)node->relay.route_discovery.next_request_ms *
                      1000u;
        if (retry_at_us < world->now_us) {
            retry_at_us = world->now_us;
        }
        return mesh_sim_scheduler_schedule(world,
                              SIM_EVENT_ROUTE_DISCOVERY_RETRY,
                              retry_at_us,
                              node_index);
    }
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    node->route_discovery_requests++;
    ret = mesh_sim_trace_add(world,
                         world->now_us,
                         node->id,
                         node->gateway_id,
                         MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                         node->route_waiting_outbound.packet.msg_type,
                         route_request.packet.ttl);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = schedule_relay_action_outbound(world, node_index, &route_request);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    retry_at_us = (uint64_t)node->relay.route_discovery.next_request_ms * 1000u;
    if (retry_at_us <= world->now_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    return mesh_sim_scheduler_schedule(world,
                          SIM_EVENT_ROUTE_DISCOVERY_RETRY,
                          retry_at_us,
                          node_index);
}

static bool gateway_semantic_delivery_requires_commit(
    const struct mesh_sim_role_instance *node,
    const struct proto_packet *packet)
{
    if (node == NULL || packet == NULL ||
        node->role != MESH_SIM_ROLE_GATEWAY ||
        packet->dst_id != node->id ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
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
    struct mesh_relay_result semantic_commit = {0};
    uint32_t unsupported;
    bool semantic_delivery = gateway_semantic_delivery_requires_commit(
        node, received_packet);
    bool semantic_committed = false;
    bool semantic_rejected = false;
    int ret;

    if ((result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        node->route_discovery_requests++;
        (void)mesh_sim_trace_add(world,
                             world->now_us,
                             node->id,
                             node->gateway_id,
                             MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                             received_packet == NULL ? 0u : received_packet->msg_type,
                             result->status);
        if (node->route_waiting_valid) {
            ret = start_route_discovery_for_waiting(world, node_index);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
        received_packet != NULL) {
        if (semantic_delivery &&
            node->gateway_semantic_rejections_remaining != 0u) {
            node->gateway_semantic_rejections_remaining--;
            node->gateway_semantic_rejection_count++;
            semantic_rejected = true;
        } else if (semantic_delivery) {
            if (node->delivery_count >= MESH_SIM_DELIVERY_CAPACITY) {
                return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
            }
            ret = mesh_relay_commit_gateway_delivery(
                &node->relay,
                received_packet,
                received_payload,
                received_payload_len,
                previous_hop_id,
                mesh_sim_time_ms(world->now_us),
                &semantic_commit);
            if (ret != PROTO_OK ||
                semantic_commit.status != PROTO_OK ||
                semantic_commit.actions != MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
                return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
            }
            node->gateway_semantic_commit_count++;
            semantic_committed = true;
        }

        if (!semantic_rejected) {
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
    }
    if ((result->actions & MESH_RELAY_ACTION_FORWARD) != 0u) {
        ret = queue_outbound(world, node_index, &result->forward, true);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) != 0u) {
        if (semantic_rejected || semantic_committed) {
            return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
        }
        if (semantic_delivery &&
            (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) == 0u) {
            node->gateway_semantic_duplicate_ack_count++;
        }
        ret = queue_outbound(world, node_index, &result->gateway_ack, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if (semantic_committed) {
        ret = queue_outbound(world,
                             node_index,
                             &semantic_commit.gateway_ack,
                             false);
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
        ret = mesh_sim_trace_add(world,
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
        ret = mesh_sim_trace_add(world,
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
        ret = mesh_sim_trace_add(world,
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
        return mesh_sim_fail(world, MESH_SIM_ERR_UNSUPPORTED_ACTION);
    }
    return MESH_SIM_OK;
}

int mesh_sim_relay_dispatch_packet(struct mesh_sim_world *world,
                           uint8_t receiver_index,
                           uint8_t sender_index,
                           const struct proto_packet *packet,
                           const uint8_t *payload,
                           size_t payload_len)
{
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    struct mesh_relay_result result;
    uint32_t random_value;
    bool event_control_handled;
    int ret;

    ret = mesh_sim_connection_process_control_packet(world,
                                            receiver_index,
                                            sender_index,
                                            packet,
                                            payload,
                                            payload_len,
                                            &event_control_handled);
    if (ret != MESH_SIM_OK || event_control_handled) {
        return ret;
    }
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
        mesh_sim_time_ms(world->now_us),
        random_value,
        &result);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = process_relay_actions(world,
                                receiver_index,
                                world->roles[sender_index].id,
                                packet,
                                payload,
                                payload_len,
                                &result);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return start_route_discovery_for_waiting(world, receiver_index);
}

int mesh_sim_relay_process_tick(struct mesh_sim_world *world, uint8_t node_index)
{
    struct mesh_sim_role_instance *node;
    struct mesh_relay_result result;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    node = &world->roles[node_index];
    if (!node->relay_initialized) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    ret = mesh_relay_tick_with_random(&node->relay,
                                      mesh_sim_time_ms(world->now_us),
                                      mesh_sim_random(world),
                                      &result);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
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
    return MESH_SIM_OK;
}

int mesh_sim_schedule_relay_tick(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t at_us)
{
    if (!mesh_sim_node_index_valid(world, node_index) || at_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_sim_scheduler_schedule(world, SIM_EVENT_RELAY_TICK, at_us, node_index);
}

int mesh_sim_relay_process_route_discovery_retry(
    struct mesh_sim_world *world,
    uint8_t node_index)
{
    return start_route_discovery_for_waiting(world, node_index);
}

int mesh_sim_relay_start_connection_tx(
    struct mesh_sim_world *world,
    struct mesh_sim_connection_event *event,
    const struct mesh_channel5_requirements *requirements,
    const struct mesh_event_plan *plan,
    uint64_t tx_start_us,
    struct mesh_sim_connection_tx_result *result)
{
    struct mesh_sim_role_instance *sender = &world->roles[event->sender_index];
    struct mesh_sim_role_instance *receiver =
        &world->roles[event->receiver_index];
    struct mesh_sim_queued_tx queued;
    struct mesh_event_plan tx_plan;
    struct mesh_outbound outbound;
    uint16_t connection_event_index;
    int queue_index;
    int ret;

    *result = (struct mesh_sim_connection_tx_result) {0};
    connection_event_index = (uint16_t)(event - world->connection_events);
    queue_index = mesh_sim_relay_queue_index_for_peer(sender, receiver->id);
    if (queue_index < 0) {
        return MESH_SIM_OK;
    }

    queued = sender->tx_queue[queue_index];
    outbound = queued.outbound;
    if (queued.needs_relay_start) {
        ret = mesh_relay_start_channel9_tx(&sender->relay,
                                            &queued.outbound.packet,
                                            queued.outbound.payload,
                                            queued.outbound.payload_len,
                                            requirements,
                                            mesh_sim_time_ms(event->start_us),
                                            &tx_plan,
                                            &outbound);
        if (ret != PROTO_OK) {
            return mesh_sim_fail(world, ret);
        }
        if ((tx_plan.action != MESH_EVENT_PLAN_START &&
             tx_plan.action != MESH_EVENT_PLAN_CLIP) ||
            tx_plan.start_ms != plan->start_ms ||
            tx_plan.end_ms != plan->end_ms) {
            return mesh_sim_fail(world, MESH_SIM_ERR_SENDER_PLAN);
        }
    }

    ret = mesh_sim_radio_schedule_channel9_runtime_tx(world,
                                                       event->sender_index,
                                                       tx_start_us,
                                                       event->end_us,
                                                       &outbound,
                                                       connection_event_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    mesh_sim_relay_remove_queue_entry(sender, (size_t)queue_index);
    result->had_packet = true;
    result->msg_type = outbound.packet.msg_type;
    return MESH_SIM_OK;
}
